#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <functional>

#include "../domain/IrrigationTypes.h"
#include "../domain/Settings.h"

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

// Called from onConnected() on every (re)connect, right after the retained `/online`=true is
// published. Lets the owner (main.ino, which holds `settings`) seed the state/echo topics HA
// relies on for display — currently `/scheduled` (retained), and `/auto_adjustment` and
// `/watering_percentage` (not retained) — so a subscriber that's already listening gets the
// current value right away on every reconnect, and `/scheduled` additionally survives an HA
// restart or a fresh subscribe while the device stays up, without HA ever having to write a value
// back to the device.
using ConnectedHandler = std::function<void()>;

class MqttService {
public:
    void begin(const MqttConfig& config);
    void loop(uint32_t nowMs);
    bool connected() const;

    void setCommandHandler(MqttCommandHandler handler) { commandHandler_ = handler; }
    void setNowEpochProvider(NowEpochProvider provider) { nowEpochProvider_ = provider; }
    void setConnectedHandler(ConnectedHandler handler) { connectedHandler_ = handler; }

    // Publishes retained true/false to `<prefix>/online`. The broker-driven LWT
    // (registered at connect time) already covers ungraceful disconnects; call this explicitly
    // before a deliberate restart so the retained value doesn't lie during the reboot window.
    void publishAvailability(bool online);

    void publishStatus(const String& jsonPayload);
    // volumeLiters: 0 while a zone/pump is running or idle; carries the just-completed run's total
    // liters ONLY in the specific /state message that reports "on":false at the exact moment that
    // run's watering ends (see main.ino's FlowRunVolumeTracker/reportStop()/reportValveClosed()) -
    // not a persisted "last run" figure, just a one-shot field on that one message.
    void publishState(uint8_t zone,
                      const char* name,
                      bool on,
                      bool pump,
                      ::irrigator::domain::RunReason reason,
                      bool scheduled,
                      uint16_t remainingMin,
                      float volumeLiters);
    void publishAlert(const String& jsonOrTextPayload);
    // Publishes to `<prefix>/flow` - raw TUF-2000M flow-meter telemetry (see main.ino's
    // updateFlowPublishCadence() for the publish-cadence rule; every poll's result is a candidate,
    // not every one goes out). Non-retained, like `/status`.
    void publishFlow(const String& jsonPayload);
    // Publishes to `<prefix>/flow_totals` - lifetime (since-boot) water consumption per enabled
    // zone plus "other" (see main.ino's FlowLifetimeCounters/buildFlowTotalsJson()), roughly once a
    // minute. Feeds the HA "Water Consumption" dashboard group's long-term-statistics sensors.
    // Non-retained, like `/status`.
    void publishFlowTotals(const String& jsonPayload);
    void publishSchedule(const String& jsonPayload);
    void publishQueue(const String& jsonPayload);
    // Publishes to `<prefix>/calibrate` in response to `<prefix>/cmd/calibrate` — one call per
    // sensor (see main.ino's handleCalibrateCommand), since a combined valve+pump payload with
    // both sensors' raw burst samples risks the MQTT buffer size.
    void publishCalibrate(const String& jsonPayload);
    void publishScheduled(bool active);
    void publishAutoAdjustmentEnabled(bool enabled);
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

    // Publishes the decoded summary of a stored panic dump to `<prefix>/coredump` — on request via
    // `cmd/coredump`, and once unprompted at boot when a dump is present (see main.ino's
    // reportCoreDumpAtBoot). The raw image itself is far too big for one message and goes out as
    // base64 chunks over `/download/coredump` instead — see docs/coredump.md.
    void publishCoreDump(const String& jsonPayload);

    // Returns true exactly once, ~15s after begin(), the first time the client is connected
    // at or after that point — the caller should build and publish the status payload then.
    bool consumeBootStatusDue(uint32_t nowMs);

    const String& prefix() const { return config_.prefix; }

    // PubSubClient's actual current buffer size, which is NOT necessarily the size begin() asked
    // for — see ensureBufferSize(). Surfaced in the /status payload (`mqtt_buffer_bytes`) because
    // an undersized buffer is otherwise invisible from MQTT: it silently drops exactly the large
    // payloads (/status, /download/*) you would need in order to notice.
    uint16_t bufferSize() const;

    // Publish-path health counters, surfaced in /status. A single failure is a transient; a
    // climbing count correlated against rssi_db is the RF link telling you something.
    uint32_t publishFailures() const { return publishFailures_; }
    uint32_t publishRetrySuccesses() const { return publishRetrySuccesses_; }
    uint32_t publishSkippedOffline() const { return publishSkippedOffline_; }
    uint32_t forcedReconnects() const { return forcedReconnects_; }

    // Spacing between the small publishes that go out back-to-back on a fresh connection. Four
    // rapid writes into a socket that is milliseconds old is where every observed publish failure
    // clustered (always the LAST of the burst, /watering_percentage). Public so main.ino's
    // ConnectedHandler paces its own seed publishes with the same number.
    static constexpr uint32_t kConnectSeedSpacingMs = 20;

private:
    void attemptReconnect(uint32_t nowMs);
    void onConnected();
    void dispatchMessage(char* topic, uint8_t* payload, unsigned int length);
    bool publishRaw(const String& topicSuffix, const String& payload, bool retained);

    // Grows PubSubClient's single shared rx/tx buffer from its 256-byte default to
    // kMqttBufferBytes. Idempotent and cheap once satisfied, so it is retried on every reconnect
    // rather than only at begin() — see its definition for why that matters.
    void ensureBufferSize();

    // Turns a failed publish into a small (always-fits) /alert naming the topic, both sizes, and
    // the two fields that actually discriminate the cause (RSSI and PubSubClient's state).
    void reportPublishFailure(const String& topicSuffix, size_t payloadLength);

    // Tears the MQTT session down after a publish has failed every attempt, so loop()'s normal
    // reconnect path rebuilds it within kReconnectIntervalMs. See its definition for why this is
    // the right response to a write failure even though PubSubClient itself shrugs one off.
    void forceReconnect(const char* reason);

    // Splices a "datetime" field (from nowEpochProvider_) into a JSON object payload, or wraps a
    // plain-text payload as {"datetime":...,"message":...}. Returns the payload unchanged if no
    // time provider has been set yet.
    String withDatetime(const String& payload) const;

    static void staticMessageCallback(char* topic, uint8_t* payload, unsigned int length);
    static MqttService* activeInstance_;

    MqttConfig config_;
    MqttCommandHandler commandHandler_;
    NowEpochProvider nowEpochProvider_;
    ConnectedHandler connectedHandler_;
    WiFiClient wifiClient_;
    PubSubClient mqttClient_;
    // False when config_.broker is empty at begin() — MQTT is entirely optional (spec: captive
    // portal / config.json may omit broker/port/user/password). loop() short-circuits and no
    // reconnect is ever attempted in that case; connected()/publish* already degrade safely to
    // no-ops via PubSubClient's own null-client handling, so nothing else needs to branch on this.
    bool enabled_ = false;

    // Re-entrancy guard for reportPublishFailure(): the alert it publishes goes through
    // publishRaw() itself, so a broker-side/socket failure (as opposed to an oversized payload)
    // would otherwise have each failed alert trigger another one, without bound.
    bool reportingPublishFailure_ = false;

    // Retry budget for a single publish. Three attempts spaced kPublishRetryDelayMs apart covers
    // a momentary TX-queue stall (the radio busy, a beacon interval, a retransmit burst) without
    // adding meaningful latency to the overwhelmingly common first-attempt success.
    static constexpr uint8_t kPublishAttempts = 3;
    static constexpr uint32_t kPublishRetryDelayMs = 5;

    uint32_t publishFailures_ = 0;
    uint32_t publishRetrySuccesses_ = 0;
    uint32_t publishSkippedOffline_ = 0;
    uint32_t forcedReconnects_ = 0;

    uint32_t bootMs_ = 0;
    bool bootStatusPublished_ = false;
    uint32_t currentNowMs_ = 0;

    uint32_t lastReconnectAttemptMs_ = 0;
    static constexpr uint32_t kReconnectIntervalMs = 1000;
};

}  // namespace irrigator::network
