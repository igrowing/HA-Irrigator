#pragma once

#include <stdint.h>

namespace irrigator::domain {

// Pure decision logic for main.ino's `/flow` MQTT publish cadence (see docs/technical.md's "Water
// flow sensing (TUF-2000M)" section for the human-readable rule this implements) - independent of
// the underlying Modbus poll cadence, which happens every FLOW_METER_POLL_INTERVAL_MS regardless
// of whether a given poll's result is actually published:
//   - While a zone or pump is active: once a minute.
//   - While idle: threshold-triggered rather than timer-driven - once accumulated idle-time volume
//     has grown past 1 liter since the last idle publish, capped to never fire more than once a
//     minute even if that threshold keeps getting crossed faster.
//   - The very first reading ever passed to shouldPublish() always publishes immediately, so an
//     MQTT explorer/HA sees something right away instead of waiting out the first cadence window.
// Holds no MQTT/Modbus/hardware state itself - main.ino owns the actual publish call and just asks
// "should I publish now."
class FlowPublishGate {
public:
    // Call once per drained flow-meter reading, in poll order. totalizerLiters is the meter's
    // cumulative totalizer (already unit-converted to liters, i.e. Reading::totalizerM3 * 1000);
    // active is whether a zone/pump is currently running. Returns true if the caller should
    // publish now - this call already updates internal bookkeeping (last-publish time, idle
    // accumulator) as if it had.
    bool shouldPublish(uint32_t nowMs, bool active, float totalizerLiters);

private:
    static constexpr uint32_t kMinPublishIntervalMs = 60000UL;
    static constexpr float kIdleThresholdLiters = 1.0f;

    bool everRead_ = false;
    bool everPublished_ = false;
    float lastTotalizerLiters_ = 0.0f;
    float idleAccumulatedLiters_ = 0.0f;
    uint32_t lastPublishMs_ = 0;
};

}  // namespace irrigator::domain
