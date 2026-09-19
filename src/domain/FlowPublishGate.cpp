#include "FlowPublishGate.h"

namespace irrigator::domain {

bool FlowPublishGate::shouldPublish(uint32_t nowMs, bool active, float totalizerLiters) {
    // Delta since the previous reading, in liters - only ever credited to the idle accumulator
    // below. A negative delta (the totalizer going backwards - shouldn't happen on real hardware,
    // but defensively ignored rather than letting it cancel out real accumulated volume) and the
    // very first reading (no prior baseline yet) both contribute nothing.
    float deltaLiters = 0.0f;
    if (everRead_) {
        const float delta = totalizerLiters - lastTotalizerLiters_;
        if (delta > 0.0f) {
            deltaLiters = delta;
        }
    }
    everRead_ = true;
    lastTotalizerLiters_ = totalizerLiters;

    if (active) {
        // A run right after an idle period shouldn't have that period's stale, not-yet-published
        // accumulation carry into its own once-a-minute cadence.
        idleAccumulatedLiters_ = 0.0f;
    } else {
        idleAccumulatedLiters_ += deltaLiters;
    }

    const bool minuteElapsedSincePublish =
        !everPublished_ || (nowMs - lastPublishMs_) >= kMinPublishIntervalMs;

    bool publish = !everPublished_;
    if (active) {
        publish = publish || minuteElapsedSincePublish;
    } else {
        publish = publish || (idleAccumulatedLiters_ > kIdleThresholdLiters && minuteElapsedSincePublish);
    }

    if (!publish) {
        return false;
    }

    everPublished_ = true;
    lastPublishMs_ = nowMs;
    idleAccumulatedLiters_ = 0.0f;
    return true;
}

}  // namespace irrigator::domain
