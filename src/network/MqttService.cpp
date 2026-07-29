#include "MqttService.h"

#include <WiFi.h>

#include "../domain/CalendarMath.h"
#include "../drivers/Logger.h"

namespace irrigator::network {

MqttService* MqttService::activeInstance_ = nullptr;

namespace {
constexpr uint16_t kMqttBufferBytes = 2048;
constexpr uint32_t kBootStatusDelayMs = 15000;
// PubSubClient defaults to a 15s keepalive, so on the garage's weak Wi-Fi (~-70dBm) a single
// missed PING during a brief signal dip was enough for the broker to declare the client dead
// (~1.5x keepalive) and fire the LWT, even when the underlying link recovered moments later.
// 200s keeps that 1.5x window (~300s) at 5 minutes, so transient drops that resolve within that
// window never flap `.../online` or re-trigger the "request data" automation in HA.
constexpr uint16_t kMqttKeepAliveSec = 200;
}  // namespace

void MqttService::begin(const MqttConfig& config) {
    config_ = config;
    enabled_ = config_.broker.length() > 0;
    activeInstance_ = this;
    bootMs_ = millis();
    bootStatusPublished_ = false;
    lastReconnectAttemptMs_ = 0;

    if (!enabled_) {
        ::irrigator::drivers::Log.info("MQTT broker not configured; MQTT disabled");
        return;
    }

    mqttClient_.setClient(wifiClient_);
    mqttClient_.setServer(config_.broker.c_str(), config_.port);
    mqttClient_.setBufferSize(kMqttBufferBytes);
    mqttClient_.setKeepAlive(kMqttKeepAliveSec);
    mqttClient_.setCallback(&MqttService::staticMessageCallback);

    ::irrigator::drivers::Log.printf("[INFO] MQTT configured for %s:%u prefix=%s\n",
                                     config_.broker.c_str(),
                                     config_.port,
                                     config_.prefix.c_str());
}

bool MqttService::connected() const {
    return const_cast<PubSubClient&>(mqttClient_).connected();
}

void MqttService::attemptReconnect(uint32_t nowMs) {
    (void)nowMs;
    const String clientId = config_.prefix;
    const String availabilityTopic = config_.prefix + "/online";

    const bool ok = mqttClient_.connect(clientId.c_str(),
                                        config_.user.length() ? config_.user.c_str() : nullptr,
                                        config_.password.length() ? config_.password.c_str()
                                                                   : nullptr,
                                        availabilityTopic.c_str(),
                                        1,     // willQos
                                        true,  // willRetain
                                        "false",
                                        true);  // cleanSession

    if (ok) {
        ::irrigator::drivers::Log.info(withDatetime("MQTT connected").c_str());
        onConnected();
    } else {
        ::irrigator::drivers::Log.printf("[WARN] MQTT connect failed, state=%d, retry in %lums\n",
                                         mqttClient_.state(),
                                         static_cast<unsigned long>(kReconnectIntervalMs));
    }
}

void MqttService::onConnected() {
    const String cmdTopic = config_.prefix + "/cmd/#";
    mqttClient_.subscribe(cmdTopic.c_str(), 1);
    publishAvailability(true);
    // After `/online`=true so any HA sensor gated on availability is already live when the seeded
    // state/echo topics land. main.ino publishes the retained `/scheduled` and
    // `/watering_percentage` here from `settings` — see setConnectedHandler's declaration.
    if (connectedHandler_) {
        connectedHandler_();
    }
}

void MqttService::loop(uint32_t nowMs) {
    currentNowMs_ = nowMs;

    if (!enabled_) {
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!mqttClient_.connected()) {
        if (nowMs - lastReconnectAttemptMs_ >= kReconnectIntervalMs) {
            lastReconnectAttemptMs_ = nowMs;
            attemptReconnect(nowMs);
        }
        return;
    }

    mqttClient_.loop();
}

bool MqttService::consumeBootStatusDue(uint32_t nowMs) {
    if (bootStatusPublished_ || !connected()) {
        return false;
    }
    if (nowMs - bootMs_ < kBootStatusDelayMs) {
        return false;
    }
    bootStatusPublished_ = true;
    return true;
}

bool MqttService::publishRaw(const String& topicSuffix, const String& payload, bool retained) {
    if (!connected()) {
        return false;
    }
    const String topic = config_.prefix + topicSuffix;
    const bool ok = mqttClient_.publish(topic.c_str(),
                                        reinterpret_cast<const uint8_t*>(payload.c_str()),
                                        payload.length(),
                                        retained);
    if (!ok) {
        ::irrigator::drivers::Log.printf("[WARN] MQTT publish to %s failed\n", topic.c_str());
    }
    return ok;
}

String MqttService::withDatetime(const String& payload) const {
    if (!nowEpochProvider_) {
        return payload;
    }
    char iso[24];
    ::irrigator::domain::formatIso8601Utc(nowEpochProvider_(), iso, sizeof(iso));

    if (payload.length() > 0 && payload[0] == '{') {
        String result = "{\"datetime\":\"";
        result += iso;
        result += "\",";
        result += payload.substring(1);
        return result;
    }
    String result = "{\"datetime\":\"";
    result += iso;
    result += "\",\"message\":\"";
    result += payload;
    result += "\"}";
    return result;
}

void MqttService::publishAvailability(bool online) {
    publishRaw("/online", online ? "true" : "false", true);
}

void MqttService::publishStatus(const String& jsonPayload) {
    publishRaw("/status", withDatetime(jsonPayload), false);
}

void MqttService::publishState(uint8_t zone,
                               const char* name,
                               bool on,
                               bool pump,
                               ::irrigator::domain::RunReason reason,
                               bool scheduled,
                               uint16_t remainingMin) {
    char payload[224];
    snprintf(payload,
             sizeof(payload),
             R"({"zone":%u,"name":"%s","on":%s,"pump":%s,"reason":"%s","scheduled":%s,"remaining_min":%u})",
             static_cast<unsigned>(zone),
             name == nullptr ? "" : name,
             on ? "true" : "false",
             pump ? "true" : "false",
             ::irrigator::domain::toString(reason),
             scheduled ? "true" : "false",
             static_cast<unsigned>(remainingMin));
    publishRaw("/state", withDatetime(payload), false);
}

void MqttService::publishAlert(const String& jsonOrTextPayload) {
    publishRaw("/alert", withDatetime(jsonOrTextPayload), false);
}

void MqttService::publishSchedule(const String& jsonPayload) {
    publishRaw("/schedule", jsonPayload, false);
}

void MqttService::publishQueue(const String& jsonPayload) {
    publishRaw("/queue", jsonPayload, false);
}

// Retained: HA's switch.irrigator_schedule_active display reads this echo (via
// binary_sensor.irrigator_scheduled_state) and no longer gets it re-pushed from a settings sync,
// so the last value must persist on the broker for HA to recover it after an HA restart or a
// fresh subscribe. Seeded on every connect via the ConnectedHandler.
void MqttService::publishScheduled(bool active) {
    publishRaw("/scheduled", active ? "true" : "false", true);
}

// Retained for the same reason as publishScheduled: sensor.irrigator_watering_percentage_echo is
// the live display of the device's current percentage (including its own weather auto-adjustment
// changes), and HA must be able to recover it on a fresh subscribe.
void MqttService::publishWateringPercentage(int16_t value) {
    publishRaw("/watering_percentage", String(value), true);
}

void MqttService::publishOtaCompleted(const String& isoTimestamp) {
    publishRaw("/ota_completed", isoTimestamp, false);
}

void MqttService::publishOtaStarted() {
    publishRaw("/ota_started", "true", false);
}

void MqttService::publishSkipRainUntil(const String& isoTimestamp) {
    publishRaw("/skip_rain_until", isoTimestamp, false);
}

void MqttService::publishDownload(const String& filename, const String& content) {
    String safeName = filename;
    safeName.replace("/", "_");
    publishRaw("/download/" + safeName, content, false);
}

void MqttService::dispatchMessage(char* topic, uint8_t* payload, unsigned int length) {
    const String topicStr(topic);

    const String cmdPrefix = config_.prefix + "/cmd/";
    if (!topicStr.startsWith(cmdPrefix)) {
        return;
    }
    const String commandName = topicStr.substring(cmdPrefix.length());
    if (commandHandler_) {
        commandHandler_(commandName, payload, length, currentNowMs_);
    }
}

void MqttService::staticMessageCallback(char* topic, uint8_t* payload, unsigned int length) {
    if (activeInstance_ != nullptr) {
        activeInstance_->dispatchMessage(topic, payload, length);
    }
}

}  // namespace irrigator::network
