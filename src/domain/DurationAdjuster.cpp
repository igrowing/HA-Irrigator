#include "DurationAdjuster.h"

namespace irrigator::domain {

uint16_t adjustDuration(uint16_t current,
                        RotationDirection direction,
                        uint16_t minValue,
                        uint16_t maxValue) {
    if (minValue > maxValue) {
        return current;
    }

    if (current < minValue) {
        current = minValue;
    } else if (current > maxValue) {
        current = maxValue;
    }

    const uint16_t step = static_cast<uint16_t>((current / 10U) + 1U);
    if (direction == RotationDirection::Clockwise) {
        const uint32_t next = static_cast<uint32_t>(current) + step;
        return next > maxValue ? maxValue : static_cast<uint16_t>(next);
    }

    if (step >= current || current - step < minValue) {
        return minValue;
    }
    return static_cast<uint16_t>(current - step);
}

uint16_t adjustManualDuration(uint16_t current, RotationDirection direction) {
    return adjustDuration(current,
                          direction,
                          board::MANUAL_DURATION_MIN_MINUTES,
                          board::MANUAL_DURATION_MAX_MINUTES);
}

uint16_t adjustScheduledDuration(uint16_t current, RotationDirection direction) {
    return adjustDuration(current,
                          direction,
                          board::SCHEDULE_DURATION_MIN_MINUTES,
                          board::SCHEDULE_DURATION_MAX_MINUTES);
}

uint16_t applyWateringPercentage(uint16_t durationMin, int16_t wateringPercentage) {
    if (wateringPercentage <= 0 || durationMin == 0) {
        return 0;
    }

    const uint32_t adjusted =
        (static_cast<uint32_t>(durationMin) * static_cast<uint32_t>(wateringPercentage) + 50U) /
        100U;
    return adjusted > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(adjusted);
}

}  // namespace irrigator::domain