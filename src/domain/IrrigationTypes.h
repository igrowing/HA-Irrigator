#pragma once

#include <stdint.h>

namespace irrigator::domain {

enum class RunReason : uint8_t { Manual, Schedule, Mqtt, Abort };
enum class RunKind : uint8_t { ZoneWithPump, PumpOnly };
enum class RotationDirection : uint8_t { CounterClockwise, Clockwise };

struct WateringRequest {
    uint8_t zone = 0;          // 0 = pump-only, 1..11 = irrigation zone.
    uint16_t durationMin = 0;  // Duration in minutes.
    RunReason reason = RunReason::Manual;

    constexpr RunKind kind() const {
        return zone == 0 ? RunKind::PumpOnly : RunKind::ZoneWithPump;
    }
};

inline const char* toString(RunReason reason) {
    switch (reason) {
        case RunReason::Manual:
            return "manual";
        case RunReason::Schedule:
            return "schedule";
        case RunReason::Mqtt:
            return "mqtt";
        case RunReason::Abort:
            return "abort";
    }
    return "unknown";
}

}  // namespace irrigator::domain