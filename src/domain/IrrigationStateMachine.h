#pragma once

#include <stdint.h>

#include "IrrigationTypes.h"

namespace irrigator::domain {

enum class IrrigationState : uint8_t { Idle, StartingPump, Watering, PumpOnly, PumpGracePeriod };

struct IrrigationSnapshot {
    IrrigationState state = IrrigationState::Idle;
    uint8_t activeZone = 0;
    bool pumpOn = false;
    uint32_t elapsedMs = 0;
    uint32_t remainingMs = 0;
};

class IrrigationStateMachine {
public:
    bool start(const WateringRequest& request, uint32_t nowMs);
    void abort(uint32_t nowMs);
    void tick(uint32_t nowMs);
    IrrigationSnapshot snapshot(uint32_t nowMs) const;

    bool isActive() const { return state_ != IrrigationState::Idle; }
    IrrigationState state() const { return state_; }

    // True in Idle, and also during PumpGracePeriod — the pump is still spinning down from the
    // previous run but hasn't stopped yet, so a queued request can hand off to it seamlessly
    // (see start()) instead of waiting for the full graceful-shutdown window to elapse.
    bool canStartImmediately() const {
        return state_ == IrrigationState::Idle || state_ == IrrigationState::PumpGracePeriod;
    }

private:
    IrrigationState state_ = IrrigationState::Idle;
    WateringRequest current_ = {};
    uint32_t startedAtMs_ = 0;
    uint32_t wateringStartedAtMs_ = 0;
    uint32_t pumpGraceStartedAtMs_ = 0;
};

}  // namespace irrigator::domain