#include "MqttService.h"

#include <WiFi.h>

#include "../domain/CalendarMath.h"
#include "../drivers/Logger.h"

namespace irrigator::network {

MqttService* MqttService::activeInstance_ = nullptr;

namespace {
constexpr uint16_t kMqttBufferBytes = 2048;
constexpr uint32_t kBootStatusDelayMs = 15000;
}  // namespace

void MqttService::begin(const MqttConfig& config) {
    config_ = config;
    activeInstance_ = this;
    bootMs_ = millis();
    bootStatusPublished_ = false;
    reconnectBackoffMs_ = kReconnectBackoffMinMs;
    lastReconnectAttemptMs_ = 0;

    mqttClient_.setClient(wifiClient_);
    mqttClient_.setServer(config_.broker.c_str(), config_.port);
    mqttClient_.setBufferSize(kMqttBufferBytes);
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
        reconnectBackoffMs_ = kReconnectBackoffMinMs;
        onConnected();
    } else {
        ::irrigator::drivers::Log.printf("[WARN] MQTT connect failed, state=%d, retry in %lums\n",
                                         mqttClient_.state(),
                                         static_cast<unsigned long>(reconnectBackoffMs_));
        reconnectBackoffMs_ = min(reconnectBackoffMs_ * 3U / 2U, kReconnectBackoffMaxMs);
    }
}

void MqttService::onConnected() {
    const String cmdTopic = config_.prefix + "/cmd/#";
    mqttClient_.subscribe(cmdTopic.c_str());
    publishAvailability(true);
}

void MqttService::loop() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!mqttClient_.connected()) {
        const uint32_t nowMs = millis();
        if (nowMs - lastReconnectAttemptMs_ >= reconnectBackoffMs_) {
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

void MqttService::publishScheduled(bool active) {
    publishRaw("/scheduled", active ? "true" : "false", false);
}

void MqttService::publishWateringPercentage(int16_t value) {
    publishRaw("/watering_percentage", String(value), false);
}

void MqttService::publishOtaCompleted(const String& isoTimestamp) {
    publishRaw("/ota_completed", isoTimestamp, false);
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
        commandHandler_(commandName, payload, length);
    }
}

void MqttService::staticMessageCallback(char* topic, uint8_t* payload, unsigned int length) {
    if (activeInstance_ != nullptr) {
        activeInstance_->dispatchMessage(topic, payload, length);
    }
}

}  // namespace irrigator::network
