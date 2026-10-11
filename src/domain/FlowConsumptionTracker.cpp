#include "FlowConsumptionTracker.h"

namespace irrigator::domain {

void FlowLifetimeCounters::addDelta(uint8_t activeZone, bool pumpOn, float deltaLiters) {
    if (activeZone >= 1 && activeZone <= board::ZONE_COUNT) {
        zoneLiters_[activeZone - 1] += deltaLiters;
    } else if (activeZone == 0 && !pumpOn) {
        otherLiters_ += deltaLiters;
    }
    // else: pump-only run or pump-graceful-shutdown tail - discarded, see the class comment.
}

float FlowLifetimeCounters::zoneLiters(uint8_t zone) const {
    if (zone < 1 || zone > board::ZONE_COUNT) {
        return 0.0f;
    }
    return zoneLiters_[zone - 1];
}

void FlowRunVolumeTracker::beginRun(float totalizerM3, bool haveTotalizer) {
    startTotalizerM3_ = totalizerM3;
    captured_ = haveTotalizer;
}

float FlowRunVolumeTracker::takeVolumeLitersOnStop(float totalizerM3, bool haveTotalizer) {
    if (!captured_) {
        return 0.0f;
    }
    captured_ = false;
    if (!haveTotalizer) {
        return 0.0f;
    }
    const float deltaM3 = totalizerM3 - startTotalizerM3_;
    return deltaM3 > 0.0f ? deltaM3 * 1000.0f : 0.0f;
}

}  // namespace irrigator::domain
