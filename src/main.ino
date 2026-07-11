#include <Arduino.h>
#include <ArduinoJson.h>
#include <Network.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>

#include "board_config.h"
#include "domain/ConfigStore.h"
#include "domain/CurrentFaultMonitor.h"
#include "domain/IrrigationStateMachine.h"
#include "domain/RainSkip.h"
#include "domain/Schedule.h"
#include "domain/ScheduleEngine.h"
#include "domain/Settings.h"
#include "domain/WateringQueue.h"
#include "drivers/Clock.h"
#include "drivers/CurrentSensor.h"
#include "drivers/FileStore.h"
#include "drivers/LedIndicator.h"
#include "drivers/Logger.h"
#include "drivers/RelayBank.h"
#include "network/CaptivePortalService.h"
#include "network/MqttService.h"
#include "network/OtaService.h"
#include "ui/IrrigatorUi.h"

using irrigator::board::PUMP_CURRENT_ADC_PIN;
using irrigator::board::STATUS_LED_PIN;
using irrigator::board::VALVE_CURRENT_ADC_PIN;
using irrigator::domain::RunReason;
using irrigator::domain::WateringRequest;

namespace {

enum class AppMode { Booting, CaptivePortal, Operational };

AppMode appMode = AppMode::Booting;

irrigator::drivers::FileStore fileStore;
irrigator::drivers::RelayBank relayBank;
irrigator::drivers::CurrentSensor valveCurrentSensor(VALVE_CURRENT_ADC_PIN,
                                                     irrigator::board::VALVE_CURRENT_SENSOR_ZERO_OFFSET_MV,
                                                     irrigator::board::VALVE_CURRENT_SENSOR_MV_PER_MA);
irrigator::drivers::CurrentSensor pumpCurrentSensor(PUMP_CURRENT_ADC_PIN,
                                                    irrigator::board::PUMP_CURRENT_SENSOR_ZERO_OFFSET_MV,
                                                    irrigator::board::PUMP_CURRENT_SENSOR_MV_PER_MA);
irrigator::drivers::LedIndicator led(STATUS_LED_PIN);
irrigator::drivers::Clock clockService;

irrigator::domain::DeviceConfig deviceConfig;
irrigator::domain::Settings settings;
irrigator::domain::ScheduleList scheduleList;
irrigator::domain::WateringQueue<irrigator::board::WATERING_QUEUE_DEPTH> wateringQueue;
irrigator::domain::RainSkip rainSkip;
irrigator::domain::CurrentFaultMonitor valveFaultMonitor({irrigator::board::VALVE_CURRENT_MIN_MA,
                                                          irrigator::board::VALVE_CURRENT_MAX_MA});
irrigator::domain::CurrentFaultMonitor pumpFaultMonitor({irrigator::board::PUMP_CURRENT_MIN_MA,
                                                         irrigator::board::PUMP_CURRENT_MAX_MA});
irrigator::domain::IrrigationStateMachine irrigation;
irrigator::domain::ScheduleEngine scheduleEngine;

irrigator::network::CaptivePortalService captivePortal;
irrigator::network::MqttService mqtt;
irrigator::network::OtaService ota;
irrigator::ui::IrrigatorUi ui;

WateringRequest activeRequestForReporting;
bool reportedActive = false;
bool mqttBreathPlayed = false;
uint32_t lastWifiRetryMs = 0;

bool valveWindowOpen = false;
uint32_t valveWindowCloseAtMs = 0;
bool valveFaultActive = false;
bool pumpWindowOpen = false;
uint32_t pumpWindowCloseAtMs = 0;
bool pumpFaultActive = false;

String makeDevicePrefix() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (mac.length() < 2) {
        return "irrigator00";
    }
    return "irrigator" + mac.substring(mac.length() - 2);
}

const char* zoneName(uint8_t zone) {
    if (zone == 0) {
        return "pump";
    }
    if (zone >= 1 && zone <= irrigator::board::ZONE_COUNT) {
        return settings.zoneNames[zone - 1];
    }
    return "unknown";
}

// Minimal ISO8601 "YYYY-MM-DDTHH:MM:SS" (UTC) -> epoch seconds. Ignores any trailing
// offset/'Z' suffix; HA is expected to publish a plain UTC timestamp for cmd/skip_rain.
int64_t parseIso8601Utc(const char* value) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) !=
        6) {
        return -1;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return -1;
    }

    int y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>((month + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

const char* bootReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "other_watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
}

String buildStatusJson(uint32_t nowMs) {
    JsonDocument doc;
    doc["firmware_version"] = irrigator::board::FIRMWARE_VERSION;
    doc["boot_reason"] = bootReasonString(esp_reset_reason());
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime_s"] = nowMs / 1000UL;
    doc["free_heap_bytes"] = ESP.getFreeHeap();
    doc["free_storage_bytes"] = static_cast<uint32_t>(fileStore.freeBytes());
    doc["cpu_temp_c"] = temperatureRead();

    JsonObject outputs = doc["outputs"].to<JsonObject>();
    if (relayBank.isPumpOn()) {
        JsonObject pumpObj = outputs["pump"].to<JsonObject>();
        pumpObj["on"] = true;
        pumpObj["current_ma"] = pumpCurrentSensor.readMilliAmps();
    }
    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        if (relayBank.isZoneOn(zone)) {
            char key[8];
            snprintf(key, sizeof(key), "zone%u", static_cast<unsigned>(zone));
            JsonObject zoneObj = outputs[key].to<JsonObject>();
            zoneObj["on"] = true;
            zoneObj["current_ma"] = valveCurrentSensor.readMilliAmps();
        }
    }

    String payload;
    serializeJson(doc, payload);
    return payload;
}

void reportStop(RunReason reason) {
    if (!reportedActive) {
        return;
    }
    mqtt.publishState(activeRequestForReporting.zone,
                      zoneName(activeRequestForReporting.zone),
                      false,
                      false,
                      reason);
    reportedActive = false;
}

bool startIrrigation(const WateringRequest& request, uint32_t nowMs) {
    if (!irrigation.start(request, nowMs)) {
        return false;
    }
    activeRequestForReporting = request;
    reportedActive = true;
    mqtt.publishState(
        request.zone, zoneName(request.zone), request.zone != 0, true, request.reason);
    return true;
}

void stopIrrigationExplicit(uint32_t nowMs) {
    if (irrigation.isActive()) {
        irrigation.abort(nowMs);
        reportStop(RunReason::Abort);
    }
}

bool persistSettings() {
    char buffer[irrigator::board::SETTINGS_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSettings(settings, buffer, sizeof(buffer));
    if (written == 0) {
        return false;
    }
    return fileStore.writeTextWithBackup(irrigator::board::SETTINGS_FILE_PATH, String(buffer));
}

void enqueueOrStart(const WateringRequest& request, uint32_t nowMs) {
    if (irrigation.isActive()) {
        if (!wateringQueue.push(request)) {
            char alertMsg[112];
            snprintf(alertMsg,
                    sizeof(alertMsg),
                    "Watering queue is full. Dropping watering: zone=%u duration=%u",
                    static_cast<unsigned>(request.zone),
                    static_cast<unsigned>(request.durationMin));
            mqtt.publishAlert(alertMsg);
        }
    } else {
        startIrrigation(request, nowMs);
    }
}

void handleMqttCommand(const String& commandName, const uint8_t* payload, size_t length) {
    const uint32_t nowMs = millis();
    const String body(reinterpret_cast<const char*>(payload), length);

    if (commandName == "watering_percentage") {
        irrigator::domain::Settings updated = settings;
        updated.wateringPercentage = static_cast<int16_t>(body.toInt());
        if (!irrigator::domain::validateSettings(updated)) {
            irrigator::drivers::Log.printf("[WARN] Rejected watering_percentage=%s (out of range)\n",
                                           body.c_str());
            return;
        }
        settings = updated;
        persistSettings();
        return;
    }

    if (commandName == "run") {
        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            irrigator::drivers::Log.warn("cmd/run: invalid JSON payload");
            return;
        }
        WateringRequest request;
        request.zone = doc["zone"] | 0;
        request.durationMin = doc["duration"] | 0;
        request.reason = RunReason::Mqtt;
        if (request.zone > irrigator::board::ZONE_COUNT || request.durationMin == 0) {
            irrigator::drivers::Log.warn("cmd/run: invalid zone/duration");
            return;
        }

        enqueueOrStart(request, nowMs);
        return;
    }

    if (commandName == "stop") {
        String trimmed = body;
        trimmed.trim();
        stopIrrigationExplicit(nowMs);
        if (trimmed == "all") {
            wateringQueue.clear();
        } else {
            WateringRequest next;
            if (wateringQueue.pop(next)) {
                startIrrigation(next, nowMs);
            }
        }
        return;
    }

    if (commandName == "skip_rain") {
        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            irrigator::drivers::Log.warn("cmd/skip_rain: invalid JSON payload");
            return;
        }
        const char* iso = doc["skip_until"] | "";
        const int64_t epoch = parseIso8601Utc(iso);
        if (epoch > 0) {
            rainSkip.setSkipUntil(epoch);
        } else {
            irrigator::drivers::Log.warn("cmd/skip_rain: invalid timestamp");
        }
        return;
    }

    if (commandName == "restart") {
        mqtt.publishAvailability(false);
        delay(200);
        ESP.restart();
        return;
    }

    if (commandName == "ota") {
        ota.beginUpdateFromUrl(body.c_str());
        return;
    }

    irrigator::drivers::Log.printf("[WARN] Unknown MQTT command: %s\n", commandName.c_str());
}

bool loadOperationalConfig() {
    String raw;
    if (!fileStore.readText(irrigator::board::CONFIG_FILE_PATH, raw)) {
        return false;
    }
    return irrigator::domain::parseConfig(raw.c_str(), raw.length(), deviceConfig);
}

void loadOrCreateSettings() {
    String raw;
    if (fileStore.exists(irrigator::board::SETTINGS_FILE_PATH) &&
        fileStore.readText(irrigator::board::SETTINGS_FILE_PATH, raw) &&
        irrigator::domain::parseSettings(raw.c_str(), raw.length(), settings)) {
        return;
    }

    irrigator::drivers::Log.warn("settings.json missing or invalid; writing defaults");
    settings = irrigator::domain::makeDefaultSettings();
    persistSettings();
}

void loadSchedule() {
    if (!fileStore.exists(irrigator::board::SCHEDULE_FILE_PATH)) {
        scheduleList = {};
        return;
    }

    String raw;
    if (!fileStore.readText(irrigator::board::SCHEDULE_FILE_PATH, raw) ||
        !irrigator::domain::parseSchedule(raw.c_str(), raw.length(), scheduleList)) {
        irrigator::drivers::Log.error("schedule.json invalid; marking schedule inactive");
        scheduleList = {};
        settings.scheduleActive = false;
    }
}

void connectWifiOnce() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(deviceConfig.ssid, deviceConfig.password);
    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000UL) {
        delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
        irrigator::drivers::Log.warn("Initial Wi-Fi connect failed; retrying in background");
    }
}

void enterCaptivePortalMode() {
    appMode = AppMode::CaptivePortal;
    const String apSsid = makeDevicePrefix() + "_SETUP";
    captivePortal.begin(apSsid, fileStore);
    led.setMode(irrigator::drivers::LedMode::CaptivePortal);
    ui.showCaptivePortal(apSsid, IPAddress(192, 168, 4, 1));
}

void enterOperationalMode() {
    appMode = AppMode::Operational;

    loadOrCreateSettings();
    loadSchedule();
    connectWifiOnce();

    led.setMode(irrigator::drivers::LedMode::Off);
    clockService.begin(deviceConfig.timezoneOffsetHours);
    clockService.restoreFromRtc();  // Fallback baseline in case NTP takes a while (or never syncs).
    clockService.syncFromNtp();     // Bounded wait; writes the result back to the DS3231 on success.

    irrigator::network::MqttConfig mqttConfig;
    mqttConfig.broker = deviceConfig.mqttBroker;
    mqttConfig.port = deviceConfig.mqttPort;
    mqttConfig.user = deviceConfig.mqttUser;
    mqttConfig.password = deviceConfig.mqttPassword;
    mqttConfig.prefix = makeDevicePrefix();
    mqtt.setCommandHandler(handleMqttCommand);
    mqtt.begin(mqttConfig);

    ui.showNormal(irrigation.snapshot(millis()));
}

void runScheduleEngine(uint32_t nowMs) {
    struct tm timeinfo;
    if (!clockService.now(timeinfo)) {
        return;  // Clock not synced yet; nothing to evaluate against.
    }

    irrigator::domain::WallClock wallClock;
    wallClock.isoDayOfWeek = timeinfo.tm_wday == 0 ? 7 : static_cast<uint8_t>(timeinfo.tm_wday);
    wallClock.hour = static_cast<uint8_t>(timeinfo.tm_hour);
    wallClock.minute = static_cast<uint8_t>(timeinfo.tm_min);

    WateringRequest dueRequests[irrigator::board::SCHEDULE_MAX_RECORDS];
    const uint8_t dueCount = scheduleEngine.tick(wallClock,
                                                 clockService.epoch(),
                                                 scheduleList,
                                                 settings.scheduleActive,
                                                 rainSkip,
                                                 settings.wateringPercentage,
                                                 dueRequests,
                                                 irrigator::board::SCHEDULE_MAX_RECORDS);
    for (uint8_t i = 0; i < dueCount; ++i) {
        enqueueOrStart(dueRequests[i], nowMs);
    }
}

// Measurement windows per spec: sampling for a given output starts the moment it's commanded on
// and keeps running until board::CURRENT_MEASURE_POST_OFF_MS after it turns off. Fault alerting
// (via CurrentFaultMonitor's begin/endActiveCycle bracket) tracks the same window, which is also
// what gives the "only once per cycle, until the alert clears" de-duplication for free.
void updateCurrentMonitoring(uint32_t nowMs) {
    const auto snapshot = irrigation.snapshot(nowMs);
    const bool valveOn = snapshot.state == irrigator::domain::IrrigationState::Watering;
    const bool pumpOn = snapshot.pumpOn;

    if (valveOn) {
        if (!valveWindowOpen) {
            valveWindowOpen = true;
            valveFaultMonitor.beginActiveCycle();
        }
        valveWindowCloseAtMs = nowMs + irrigator::board::CURRENT_MEASURE_POST_OFF_MS;

        const uint16_t currentMa = valveCurrentSensor.readMilliAmps();
        if (valveFaultMonitor.checkAndConsumeAlert(currentMa)) {
            valveFaultActive = true;
            char alertMsg[80];
            snprintf(alertMsg,
                    sizeof(alertMsg),
                    R"({"valve":%u,"current":%u,"units":"mA"})",
                    static_cast<unsigned>(snapshot.activeZone),
                    static_cast<unsigned>(currentMa));
            mqtt.publishAlert(alertMsg);
        }
    } else if (valveWindowOpen && nowMs >= valveWindowCloseAtMs) {
        valveWindowOpen = false;
        valveFaultActive = false;
        valveFaultMonitor.endActiveCycle();
    }

    if (pumpOn) {
        if (!pumpWindowOpen) {
            pumpWindowOpen = true;
            pumpFaultMonitor.beginActiveCycle();
        }
        pumpWindowCloseAtMs = nowMs + irrigator::board::CURRENT_MEASURE_POST_OFF_MS;

        const uint16_t currentMa = pumpCurrentSensor.readMilliAmps();
        if (pumpFaultMonitor.checkAndConsumeAlert(currentMa)) {
            pumpFaultActive = true;
            char alertMsg[64];
            snprintf(alertMsg,
                    sizeof(alertMsg),
                    R"({"pump":0,"current":%u,"units":"mA"})",
                    static_cast<unsigned>(currentMa));
            mqtt.publishAlert(alertMsg);
        }
    } else if (pumpWindowOpen && nowMs >= pumpWindowCloseAtMs) {
        pumpWindowOpen = false;
        pumpFaultActive = false;
        pumpFaultMonitor.endActiveCycle();
    }
}

void updateOutputsFromDomain() {
    const auto snapshot = irrigation.snapshot(millis());
    relayBank.setPump(snapshot.pumpOn);

    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        const bool zoneOn = snapshot.state == irrigator::domain::IrrigationState::Watering &&
                            snapshot.activeZone == zone;
        relayBank.setZone(zone, zoneOn);
    }

    if (valveFaultActive || pumpFaultActive) {
        led.setMode(irrigator::drivers::LedMode::Alert);
    } else if (snapshot.pumpOn) {
        led.setMode(irrigator::drivers::LedMode::SolidOn);
    } else if (led.mode() == irrigator::drivers::LedMode::SolidOn ||
              led.mode() == irrigator::drivers::LedMode::Alert) {
        led.setMode(irrigator::drivers::LedMode::Off);
    }
}

}  // namespace

void setup() {
    irrigator::drivers::Log.begin(115200);
    irrigator::drivers::Log.info(F("Irrigator firmware starting"));

    Wire.begin(irrigator::board::I2C_SDA_PIN, irrigator::board::I2C_SCL_PIN);
    Wire.setClock(irrigator::board::I2C_CLOCK_HZ);

    analogReadResolution(irrigator::board::ADC_RESOLUTION_BITS);
    analogSetAttenuation(ADC_11db);

    fileStore.begin();
    relayBank.begin();
    valveCurrentSensor.begin();
    pumpCurrentSensor.begin();
    led.begin();
    ui.begin();

    if (!fileStore.exists(irrigator::board::CONFIG_FILE_PATH) || !loadOperationalConfig()) {
        enterCaptivePortalMode();
        return;
    }

    enterOperationalMode();
}

void loop() {
    const uint32_t nowMs = millis();
    led.update(nowMs);

    if (ota.inProgress()) {
        led.setMode(irrigator::drivers::LedMode::Ota);
        return;
    }

    switch (appMode) {
        case AppMode::CaptivePortal:
            captivePortal.loop();
            break;
        case AppMode::Operational:
            if (WiFi.status() != WL_CONNECTED) {
                if (nowMs - lastWifiRetryMs > 30000UL) {
                    lastWifiRetryMs = nowMs;
                    irrigator::drivers::Log.warn("Wi-Fi disconnected; attempting reconnect");
                    WiFi.reconnect();
                }
            } else if (!mqttBreathPlayed && mqtt.connected()) {
                mqttBreathPlayed = true;
                led.setMode(irrigator::drivers::LedMode::BreathOnce);
            }

            mqtt.loop();
            if (mqtt.consumeBootStatusDue(nowMs)) {
                mqtt.publishStatus(buildStatusJson(nowMs));
            }

            runScheduleEngine(nowMs);

            irrigation.tick(nowMs);
            if (reportedActive && !irrigation.isActive()) {
                reportStop(activeRequestForReporting.reason);
                // Pop the next queued watering (schedule-overlap or MQTT-during-active queueing).
                WateringRequest next;
                if (wateringQueue.pop(next)) {
                    startIrrigation(next, nowMs);
                }
            }
            updateCurrentMonitoring(nowMs);
            updateOutputsFromDomain();
            break;
        case AppMode::Booting:
        default:
            break;
    }
}
