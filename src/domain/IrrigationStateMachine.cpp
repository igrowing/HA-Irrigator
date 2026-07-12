#include "IrrigationStateMachine.h"

#include "board_config.h"

namespace irrigator::domain {

static uint32_t durationToMs(uint16_t minutes) {
    return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

bool IrrigationStateMachine::start(const WateringRequest& request, uint32_t nowMs) {
    if (!canStartImmediately() || request.durationMin == 0 || request.zone > board::ZONE_COUNT) {
        return false;
    }

    // Seamless hand-off: if the pump is still spinning down from the previous run
    // (PumpGracePeriod), it never actually stopped — skip the prestart ramp-up and open the
    // next valve immediately instead of cycling the pump off and back on.
    const bool pumpAlreadyRunning = state_ == IrrigationState::PumpGracePeriod;

    current_ = request;
    startedAtMs_ = nowMs;
    pumpGraceStartedAtMs_ = 0;
    if (pumpAlreadyRunning) {
        wateringStartedAtMs_ = nowMs;
        state_ = request.zone == 0 ? IrrigationState::PumpOnly : IrrigationState::Watering;
    } else {
        wateringStartedAtMs_ = request.zone == 0 ? nowMs : 0;
        state_ = request.zone == 0 ? IrrigationState::PumpOnly : IrrigationState::StartingPump;
    }
    return true;
}

void IrrigationStateMachine::abort(uint32_t nowMs) {
    (void)nowMs;
    current_ = {};
    startedAtMs_ = 0;
    wateringStartedAtMs_ = 0;
    pumpGraceStartedAtMs_ = 0;
    state_ = IrrigationState::Idle;
}

void IrrigationStateMachine::tick(uint32_t nowMs) {
    switch (state_) {
        case IrrigationState::StartingPump:
            if (nowMs - startedAtMs_ >= board::PUMP_PRESTART_MS) {
                wateringStartedAtMs_ = nowMs;
                state_ = IrrigationState::Watering;
            }
            break;
        case IrrigationState::Watering:
            if (nowMs - wateringStartedAtMs_ >= durationToMs(current_.durationMin)) {
                pumpGraceStartedAtMs_ = nowMs;
                state_ = IrrigationState::PumpGracePeriod;
            }
            break;
        case IrrigationState::PumpOnly:
            if (nowMs - wateringStartedAtMs_ >= durationToMs(current_.durationMin)) {
                abort(nowMs);
            }
            break;
        case IrrigationState::PumpGracePeriod:
            if (nowMs - pumpGraceStartedAtMs_ >= board::PUMP_GRACEFUL_SHUTDOWN_MS) {
                abort(nowMs);
            }
            break;
        case IrrigationState::Idle:
        default:
            break;
    }
}

IrrigationSnapshot IrrigationStateMachine::snapshot(uint32_t nowMs) const {
    IrrigationSnapshot shot;
    shot.state = state_;
    shot.activeZone = current_.zone;
    shot.pumpOn = state_ != IrrigationState::Idle;

    if (state_ == IrrigationState::Watering || state_ == IrrigationState::PumpOnly) {
        shot.elapsedMs = nowMs - wateringStartedAtMs_;
        const uint32_t total = durationToMs(current_.durationMin);
        shot.remainingMs = shot.elapsedMs >= total ? 0 : total - shot.elapsedMs;
    }
    return shot;
}

}  // namespace irrigator::domain