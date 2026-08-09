#include "CaptivePortalService.h"

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "../drivers/Logger.h"
#include "CaptivePortalHtml.h"
#include "board_config.h"

namespace irrigator::network {

namespace {
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint16_t kMqttProbeTimeoutSeconds = 5;
}  // namespace

void CaptivePortalService::begin(const String& apSsid, ::irrigator::drivers::FileStore& fileStore) {
    active_ = true;
    apSsid_ = apSsid;
    fileStore_ = &fileStore;

    WiFi.mode(WIFI_AP);
    const IPAddress apIp(192, 168, 4, 1);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSsid_.c_str());

    dnsServer_.start(53, "*", apIp);

    server_.on("/", HTTP_GET, [this]() { handleRoot(); });
    server_.on("/save", HTTP_POST, [this]() { handleSave(); });
    server_.onNotFound([this]() { handleNotFound(); });
    server_.begin();

    ::irrigator::drivers::Log.printf("[INFO] Captive portal started: %s (%s)\n",
                                     apSsid_.c_str(),
                                     apIp.toString().c_str());
}

void CaptivePortalService::loop() {
    dnsServer_.processNextRequest();
    server_.handleClient();
}

String CaptivePortalService::renderForm(const ::irrigator::domain::DeviceConfig& values,
                                        const char* errorMessage) {
    String html = CAPTIVE_PORTAL_HTML;
    html.replace("%SSID_VALUE%", values.ssid);
    html.replace("%PASSWORD_VALUE%", values.password);
    html.replace("%MQTT_BROKER_VALUE%", values.mqttBroker);
    html.replace("%MQTT_PORT_VALUE%", String(values.mqttPort));
    html.replace("%MQTT_USER_VALUE%", values.mqttUser);
    html.replace("%MQTT_PASSWORD_VALUE%", values.mqttPassword);
    html.replace("%ERROR_MESSAGE%", errorMessage == nullptr ? "" : errorMessage);
    return html;
}

void CaptivePortalService::handleRoot() {
    server_.send(200, "text/html", renderForm(::irrigator::domain::DeviceConfig{}, nullptr));
}

bool CaptivePortalService::parseFormConfig(::irrigator::domain::DeviceConfig& out) {
    snprintf(out.ssid, sizeof(out.ssid), "%s", server_.arg("ssid").c_str());
    snprintf(out.password, sizeof(out.password), "%s", server_.arg("password").c_str());
    snprintf(out.mqttBroker, sizeof(out.mqttBroker), "%s", server_.arg("mqtt_broker").c_str());
    snprintf(out.mqttUser, sizeof(out.mqttUser), "%s", server_.arg("mqtt_user").c_str());
    snprintf(
        out.mqttPassword, sizeof(out.mqttPassword), "%s", server_.arg("mqtt_password").c_str());
    out.mqttPort = static_cast<uint16_t>(server_.arg("mqtt_port").toInt());
    // timezoneOffsetHours is intentionally left at DeviceConfig's 0 (UTC): the form no longer asks
    // for it, and the device overwrites it from ip-api.com's `offset` the first time geolocation
    // succeeds after this reboot (main.ino's tryGeolocateIfDue). UTC is the right placeholder for
    // the gap in between — it is what an unanswered dropdown defaulted to before, too.
    return ::irrigator::domain::validateConfig(out);
}

bool CaptivePortalService::testMqttOnce(const ::irrigator::domain::DeviceConfig& config) {
    WiFiClient client;
    PubSubClient probe(client);
    probe.setServer(config.mqttBroker, config.mqttPort);
    probe.setSocketTimeout(kMqttProbeTimeoutSeconds);

    const char* user = config.mqttUser[0] != '\0' ? config.mqttUser : nullptr;
    const char* password = config.mqttPassword[0] != '\0' ? config.mqttPassword : nullptr;
    const bool ok = probe.connect("irrigator-setup-probe", user, password);
    if (ok) {
        probe.disconnect();
    }
    return ok;
}

void CaptivePortalService::failAndStayInApMode(const ::irrigator::domain::DeviceConfig& formValues,
                                                const char* errorMessage) {
    ::irrigator::drivers::Log.printf("[WARN] Captive portal setup failed: %s\n", errorMessage);
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    fileStore_->remove(irrigator::board::CONFIG_FILE_PATH);
    server_.send(200, "text/html", renderForm(formValues, errorMessage));
}

void CaptivePortalService::handleSave() {
    ::irrigator::domain::DeviceConfig config;
    if (!parseFormConfig(config)) {
        failAndStayInApMode(config, "Missing or invalid required fields.");
        return;
    }

    // Try WiFi exactly once, no retries (spec: Captive portal step 2).
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(config.ssid, config.password);

    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < kWifiConnectTimeoutMs) {
        delay(100);
    }

    if (WiFi.status() != WL_CONNECTED) {
        failAndStayInApMode(config, "Wi-Fi connection failed. Check SSID/password.");
        return;
    }

    // Try MQTT exactly once, no retries (spec: Captive portal step 3) — skipped entirely when no
    // broker was given, since MQTT is optional and an empty broker means "don't use it".
    const bool mqttConfigured = config.mqttBroker[0] != '\0';
    if (mqttConfigured && !testMqttOnce(config)) {
        failAndStayInApMode(config, "Wi-Fi connected, but MQTT broker unreachable.");
        return;
    }

    char jsonBuffer[irrigator::board::CONFIG_JSON_MAX_BYTES];
    const size_t written = ::irrigator::domain::serializeConfig(config, jsonBuffer, sizeof(jsonBuffer));
    if (written == 0 ||
        !fileStore_->writeTextWithBackup(irrigator::board::CONFIG_FILE_PATH, String(jsonBuffer))) {
        failAndStayInApMode(config, "Could not save configuration to flash.");
        return;
    }

    // Mixed AP+STA mode: the client that just submitted the form is still on the AP link,
    // so it can receive this response reporting the new station IP before we restart.
    String html = CONFIG_SAVED_HTML;
    html.replace("%DEVICE_IP%", WiFi.localIP().toString());
    html.replace("%MQTT_STATUS_LINE%",
                mqttConfigured ? "Wi-Fi and MQTT connectivity both confirmed."
                               : "Wi-Fi connectivity confirmed. MQTT is disabled (no broker configured).");
    server_.send(200, "text/html", html);

    ::irrigator::drivers::Log.info("Captive portal setup succeeded, restarting");
    delay(2000);
    ESP.restart();
}

void CaptivePortalService::handleNotFound() {
    server_.sendHeader("Location", "http://192.168.4.1/");
    server_.send(302, "text/plain", "Redirecting to captive portal");
}

}  // namespace irrigator::network
