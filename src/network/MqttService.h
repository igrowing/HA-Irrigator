#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <functional>

#include "../domain/IrrigationTypes.h"

namespace irrigator::network {

struct MqttConfig {
    String broker;
    uint16_t port = 1883;
    String user;
    String password;
    String prefix;
};

// Called for every message on `<prefix>/cmd/<commandName>` — commandName is everything after
// `cmd/`, so a message on `cmd/upload/settings.json` arrives as commandName="upload/settings.json".
using MqttCommandHandler =
    std::function<void(const String& commandName, const uint8_t* payload, size_t length)>;

class MqttService {
public:
    void begin(const MqttConfig& config);
    void loop();
    bool connected() const;

    void setCommandHandler(MqttCommandHandler handler) { commandHandler_ = handler; }

    // Publishes retained online/offline to `<prefix>/availability`. The broker-driven LWT
    // (registered at connect time) already covers ungraceful disconnects; call this explicitly
    // before a deliberate restart so the retained value doesn't lie during the reboot window.
    void publishAvailability(bool online);

    void publishStatus(const String& jsonPayload);
    void publishState(uint8_t zone,
                      const char* name,
                      bool on,
                      bool pump,
                      ::irrigator::domain::RunReason reason);
    void publishAlert(const String& jsonOrTextPayload);
    void publishSchedule(const String& jsonPayload);

    // Publishes file content to `<prefix>/download/<filename>` in response to a
    // `<prefix>/cmd/download` request (payload = filename).
    void publishDownload(const String& filename, const String& content);

    // Returns true exactly once, ~15s after begin(), the first time the client is connected
    // at or after that point — the caller should build and publish the status payload then.
    bool consumeBootStatusDue(uint32_t nowMs);

    const String& prefix() const { return config_.prefix; }

private:
    void attemptReconnect(uint32_t nowMs);
    void onConnected();
    void dispatchMessage(char* topic, uint8_t* payload, unsigned int length);
    bool publishRaw(const String& topicSuffix, const String& payload, bool retained);

    static void staticMessageCallback(char* topic, uint8_t* payload, unsigned int length);
    static MqttService* activeInstance_;

    MqttConfig config_;
    MqttCommandHandler commandHandler_;
    WiFiClient wifiClient_;
    PubSubClient mqttClient_;

    uint32_t bootMs_ = 0;
    bool bootStatusPublished_ = false;

    uint32_t lastReconnectAttemptMs_ = 0;
    uint32_t reconnectBackoffMs_ = 5000;
    static constexpr uint32_t kReconnectBackoffMinMs = 5000;
    static constexpr uint32_t kReconnectBackoffMaxMs = 60000;
};

}  // namespace irrigator::network
