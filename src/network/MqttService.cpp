#include "MqttService.h"

#include <WiFi.h>

#include "../domain/CalendarMath.h"
#include "../drivers/Logger.h"
#include "board_config.h"

namespace irrigator::network {

MqttService* MqttService::activeInstance_ = nullptr;

namespace {
// Must comfortably fit a whole publishSchedule() payload (the full schedule.json, echoed on every
// edit) plus topic/header overhead — sized off SCHEDULE_JSON_MAX_BYTES rather than a standalone
// number so the two can't silently drift out of sync again (2048 was already too small for even
// the pre-widening 32-record/~2.5KB max). PubSubClient's bufferSize is a uint16_t, so this must
// stay under 65536 — comfortably true here.
constexpr uint16_t kMqttBufferBytes =
    static_cast<uint16_t>(irrigator::board::SCHEDULE_JSON_MAX_BYTES + 1024);
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
    ensureBufferSize();
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

uint16_t MqttService::bufferSize() const {
    return const_cast<PubSubClient&>(mqttClient_).getBufferSize();
}

// PubSubClient allocates a single 256-byte buffer in its constructor and uses it for BOTH receive
// and transmit, so this has to grow it to fit the largest thing we ever publish (a whole
// schedule.json — see kMqttBufferBytes). Because bufferSize is already non-zero by then, the
// library takes its realloc() path, and on failure it returns false while KEEPING the old 256
// bytes (PubSubClient.cpp's setBufferSize) — it does not throw, disconnect, or mark itself broken.
//
// That failure is silent in the worst possible way: publish() then just returns false for anything
// over ~250 bytes, so /online, /state and /alert keep working normally while /status and every
// /download/<file> response disappear with no error anywhere on MQTT. The device looks like it is
// ignoring those commands. Hence: check the return, say so loudly, and retry.
//
// Retried from attemptReconnect() rather than only here because the failure is heap-driven and
// therefore transient — begin() runs right after Wi-Fi association and an NTP sync, which is close
// to the heap's low-water mark for the whole session. A later retry, once that has settled, will
// usually succeed and the device silently heals instead of staying half-mute until someone reboots
// it. Cheap to call repeatedly: once the size matches, it does nothing.
void MqttService::ensureBufferSize() {
    if (mqttClient_.getBufferSize() >= kMqttBufferBytes) {
        return;
    }
    if (mqttClient_.setBufferSize(kMqttBufferBytes)) {
        ::irrigator::drivers::Log.printf("[INFO] MQTT buffer set to %u bytes\n",
                                         static_cast<unsigned>(mqttClient_.getBufferSize()));
        return;
    }
    ::irrigator::drivers::Log.printf(
        "[ERROR] MQTT buffer resize to %u bytes FAILED (still %u) - free heap %u. Large payloads "
        "(/status, /download/*) will be silently dropped until this succeeds; retrying on the next "
        "reconnect\n",
        static_cast<unsigned>(kMqttBufferBytes),
        static_cast<unsigned>(mqttClient_.getBufferSize()),
        static_cast<unsigned>(ESP.getFreeHeap()));
}

void MqttService::attemptReconnect(uint32_t nowMs) {
    (void)nowMs;
    // Self-heal an earlier undersized-buffer failure now that the heap has had time to settle —
    // see ensureBufferSize(). No-op in the normal case where begin() already got what it asked for.
    ensureBufferSize();
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
    // Nagle holds a small outgoing segment while a previous small one is still unacknowledged;
    // paired with the broker's delayed ACK that can park a ~150-byte /state in the send buffer for
    // up to ~200ms. Normally invisible - but this device has been resetting mid-run (boot_reason
    // "other_watchdog"), and anything still buffered when the CPU resets is discarded silently.
    // startIrrigation() publishes /state and the relay energises ~20ms later in the same loop()
    // iteration, squarely inside that window, which is how a /state could be published
    // successfully (publish() returned true) and never observed on the broker. Disabling Nagle
    // pushes each publish out on its own segment immediately. Must be called AFTER connect() -
    // there is no socket fd before that, so a call in begin() would silently do nothing.
    wifiClient_.setNoDelay(true);

    const String cmdTopic = config_.prefix + "/cmd/#";
    mqttClient_.subscribe(cmdTopic.c_str(), 1);
    delay(kConnectSeedSpacingMs);
    publishAvailability(true);
    // Every publish failure observed on this device was the LAST of the four small writes that go
    // out here back-to-back into a socket only milliseconds old. Pacing them costs ~80ms on a path
    // that runs once per connection, and takes the burst out of the failure window.
    delay(kConnectSeedSpacingMs);
    // After `/online`=true so any HA sensor gated on availability is already live when the seeded
    // state/echo topics land. main.ino publishes the retained `/scheduled`, plus `/auto_adjustment`
    // and `/watering_percentage` (not retained -- see their publish*() comments below), here from
    // `settings` — see setConnectedHandler's declaration.
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
        // Counted rather than reported: publishing an alert about being unable to publish is
        // futile, and this path is legitimately hit during a reconnect window. It used to be
        // entirely invisible, which is what made "the device ignored my command" unfalsifiable.
        ++publishSkippedOffline_;
        return false;
    }
    const String topic = config_.prefix + topicSuffix;

    // PubSubClient::publish() returns false for exactly three reasons: not connected (excluded
    // above), payload larger than the buffer (excluded in practice - 25600 bytes against payloads
    // of 1-2KB), and _client->write() returning short. On this device it is always the third, and
    // a short write is a momentary radio/TX-queue condition, not a permanent one - so a couple of
    // retries a few ms apart usually lands it.
    for (uint8_t attempt = 1; attempt <= kPublishAttempts; ++attempt) {
        if (mqttClient_.publish(topic.c_str(),
                                reinterpret_cast<const uint8_t*>(payload.c_str()),
                                payload.length(),
                                retained)) {
            if (attempt > 1) {
                ++publishRetrySuccesses_;
                ::irrigator::drivers::Log.printf("[INFO] MQTT publish to %s succeeded on attempt %u\n",
                                                 topic.c_str(),
                                                 static_cast<unsigned>(attempt));
            }
            return true;
        }
        if (attempt < kPublishAttempts) {
            delay(kPublishRetryDelayMs);
        }
    }

    ++publishFailures_;
    ::irrigator::drivers::Log.printf(
        "[WARN] MQTT publish to %s failed all %u attempts (payload %u B, buffer %u B, rssi %d, state %d)\n",
        topic.c_str(),
        static_cast<unsigned>(kPublishAttempts),
        static_cast<unsigned>(payload.length()),
        static_cast<unsigned>(mqttClient_.getBufferSize()),
        static_cast<int>(WiFi.RSSI()),
        mqttClient_.state());
    reportPublishFailure(topicSuffix, payload.length());

    // Skipped while reporting: the /alert publish is nested inside the outer failing one, and
    // tearing the session down here would kill the report before it goes out. The outer call
    // does it a moment later.
    if (!reportingPublishFailure_) {
        forceReconnect("publish failed on every attempt");
    }
    return false;
}

// A failed write means the socket is very likely already dead - and PubSubClient does NOT act on
// that: write() returns false while _state stays MQTT_CONNECTED and the fd stays open, so
// connected() keeps answering true. Left alone, nothing notices until the keepalive machinery
// does, and that machinery is slow by construction: the client sends its first PINGREQ only after
// kMqttKeepAliveSec and concludes the connection is dead only after twice that, while the broker
// gives up at 1.5x and fires the LWT first. Observed on this device: a failed publish at 23:57:28
// was the last thing to reach the broker, /online went false exactly 300s later, and the device
// did not reconnect until after that. Every command HA sent in between went into a dead socket.
//
// Tearing the session down here collapses that to one kReconnectIntervalMs tick. disconnect()
// attempts a clean DISCONNECT first, which on a genuinely dead socket simply fails and costs
// nothing; on a merely degraded one it spares the broker (and HA) a spurious LWT.
void MqttService::forceReconnect(const char* reason) {
    ++forcedReconnects_;
    ::irrigator::drivers::Log.printf("[WARN] MQTT forcing reconnect: %s\n", reason);
    mqttClient_.disconnect();
    // So loop()'s !connected() branch reconnects on its very next pass, rather than waiting out a
    // fresh kReconnectIntervalMs measured from whenever the previous attempt happened to be.
    lastReconnectAttemptMs_ = 0;
}

// A failed publish used to be visible only on a serial console nobody is watching (agents.md
// section 3) - and it is precisely the failure that makes the device look like it ignored a
// command outright, since PubSubClient::publish() returns false for an oversized payload without
// any other trace. The alert built here is deliberately tiny so it still fits even the 256-byte
// default buffer that is the most likely reason the original publish failed; it names both sizes
// rather than asserting a cause, because publish() also returns false on a plain socket-write
// failure and the two are indistinguishable from its return value alone.
void MqttService::reportPublishFailure(const String& topicSuffix, size_t payloadLength) {
    if (reportingPublishFailure_ || topicSuffix == "/alert") {
        // Either the failure alert itself just failed, or the thing that failed WAS an alert -
        // both would recurse. The serial log above still records it.
        return;
    }
    reportingPublishFailure_ = true;
    char alertMsg[240];
    snprintf(alertMsg, sizeof(alertMsg),
             R"({"publish":"%s","error":"failed %u attempts - payload %u B, MQTT buffer %u B","rssi_db":%d,"mqtt_state":%d})",
             topicSuffix.c_str(),
             static_cast<unsigned>(kPublishAttempts),
             static_cast<unsigned>(payloadLength),
             static_cast<unsigned>(mqttClient_.getBufferSize()),
             static_cast<int>(WiFi.RSSI()),
             mqttClient_.state());
    publishRaw("/alert", withDatetime(alertMsg), false);
    reportingPublishFailure_ = false;
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
                               uint16_t remainingMin,
                               float volumeLiters) {
    char payload[256];
    snprintf(payload,
             sizeof(payload),
             R"({"zone":%u,"name":"%s","on":%s,"pump":%s,"reason":"%s","scheduled":%s,"remaining_min":%u,"volume":%.2f})",
             static_cast<unsigned>(zone),
             name == nullptr ? "" : name,
             on ? "true" : "false",
             pump ? "true" : "false",
             ::irrigator::domain::toString(reason),
             scheduled ? "true" : "false",
             static_cast<unsigned>(remainingMin),
             static_cast<double>(volumeLiters));
    publishRaw("/state", withDatetime(payload), false);
}

void MqttService::publishAlert(const String& jsonOrTextPayload) {
    publishRaw("/alert", withDatetime(jsonOrTextPayload), false);
}

void MqttService::publishFlow(const String& jsonPayload) {
    publishRaw("/flow", withDatetime(jsonPayload), false);
}

void MqttService::publishMeter(const String& jsonPayload) {
    publishRaw("/meter", withDatetime(jsonPayload), false);
}

void MqttService::publishSchedule(const String& jsonPayload) {
    publishRaw("/schedule", jsonPayload, false);
}

void MqttService::publishQueue(const String& jsonPayload) {
    publishRaw("/queue", jsonPayload, false);
}

void MqttService::publishCalibrate(const String& jsonPayload) {
    publishRaw("/calibrate", withDatetime(jsonPayload), false);
}

// Retained: HA's switch.irrigator_schedule_active display reads this echo (via
// binary_sensor.irrigator_scheduled_state) and no longer gets it re-pushed from a settings sync,
// so the last value must persist on the broker for HA to recover it after an HA restart or a
// fresh subscribe. Seeded on every connect via the ConnectedHandler.
void MqttService::publishScheduled(bool active) {
    publishRaw("/scheduled", active ? "true" : "false", true);
}

// NOT retained (deliberately, unlike publishScheduled): a stale retained value here would
// misrepresent switch.irrigator_auto_adjustment's echo indefinitely if the broker held it past a
// device restart. Still seeded on every connect via the ConnectedHandler, so a subscriber that's
// already listening at that moment gets the current value promptly; it just isn't held by the
// broker for one that subscribes later.
void MqttService::publishAutoAdjustmentEnabled(bool enabled) {
    publishRaw("/auto_adjustment", enabled ? "true" : "false", false);
}

// Retained as of 2026-08-29; it deliberately was not before. This is the most consequential
// number the device publishes: computeAdjustedPercentage() returns 0 to mean "skip today's cycle
// entirely" rather than clamping to lower_limit, so a lost message is the difference between HA
// showing "not watering today" and showing yesterday's figure. It was also, repeatedly, the exact
// publish that failed - being the last of the connect-time seed burst.
//
// The original objection was that a stale retained value would misrepresent
// sensor.irrigator_watering_percentage_echo past a device restart. That no longer holds: HA gates
// these entities on the retained `/online`, which the LWT drives to false when the device goes
// away, so a retained value cannot be shown as live while the device is offline.
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

void MqttService::publishCoreDump(const String& jsonPayload) {
    publishRaw("/coredump", withDatetime(jsonPayload), false);
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
