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
// nowMs is the same timestamp the caller's loop() iteration is using elsewhere (e.g. for
// irrigation.tick()) — the handler must not sample its own millis(), since that would run later
// than the loop's nowMs and could make a just-started watering run's start time appear to be in
// the future relative to the next tick(), underflowing the elapsed-time subtraction.
using MqttCommandHandler =
    std::function<void(const String& commandName, const uint8_t* payload, size_t length, uint32_t nowMs)>;

// Supplies the current UTC epoch for stamping outgoing messages with a "datetime" field — set
// once via setNowEpochProvider() so publishState/publishStatus/publishAlert don't each need the
// caller to thread a clock reading through.
using NowEpochProvider = std::function<int64_t()>;

class MqttService {
public:
    void begin(const MqttConfig& config);
    void loop(uint32_t nowMs);
    bool connected() const;

    void setCommandHandler(MqttCommandHandler handler) { commandHandler_ = handler; }
    void setNowEpochProvider(NowEpochProvider provider) { nowEpochProvider_ = provider; }

    // Publishes retained true/false to `<prefix>/online`. The broker-driven LWT
    // (registered at connect time) already covers ungraceful disconnects; call this explicitly
    // before a deliberate restart so the retained value doesn't lie during the reboot window.
    void publishAvailability(bool online);

    void publishStatus(const String& jsonPayload);
    void publishState(uint8_t zone,
                      const char* name,
                      bool on,
                      bool pump,
                      ::irrigator::domain::RunReason reason,
                      bool scheduled,
                      uint16_t remainingMin);
    void publishAlert(const String& jsonOrTextPayload);
    void publishSchedule(const String& jsonPayload);
    void publishQueue(const String& jsonPayload);
    void publishScheduled(bool active);
    void publishWateringPercentage(int16_t value);
    void publishOtaCompleted(const String& isoTimestamp);
    // Publishes to `<prefix>/ota_started` the moment a `cmd/ota` command is actually accepted -
    // lets a caller (e.g. scripts/ota.py) distinguish "broker accepted my publish" from "the
    // device received it", since cmd/# is delivered at QoS 0 and a message can be silently
    // dropped if the device is mid Wi-Fi-reconnect when it's sent.
    void publishOtaStarted();
    void publishSkipRainUntil(const String& isoTimestamp);

    // Publishes file content to `<prefix>/download/<filename>` in response to a
    // `<prefix>/cmd/download/<filename>` request.
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

    // Splices a "datetime" field (from nowEpochProvider_) into a JSON object payload, or wraps a
    // plain-text payload as {"datetime":...,"message":...}. Returns the payload unchanged if no
    // time provider has been set yet.
    String withDatetime(const String& payload) const;

    static void staticMessageCallback(char* topic, uint8_t* payload, unsigned int length);
    static MqttService* activeInstance_;

    MqttConfig config_;
    MqttCommandHandler commandHandler_;
    NowEpochProvider nowEpochProvider_;
    WiFiClient wifiClient_;
    PubSubClient mqttClient_;

    uint32_t bootMs_ = 0;
    bool bootStatusPublished_ = false;
    uint32_t currentNowMs_ = 0;

    uint32_t lastReconnectAttemptMs_ = 0;
    uint32_t reconnectBackoffMs_ = 5000;
    static constexpr uint32_t kReconnectBackoffMinMs = 5000;
    static constexpr uint32_t kReconnectBackoffMaxMs = 60000;
};

}  // namespace irrigator::network
