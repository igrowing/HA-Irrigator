#pragma once

#include <stdint.h>

#include "IrrigationTypes.h"
#include "RainSkip.h"
#include "Schedule.h"

namespace irrigator::domain {

struct WallClock {
    uint8_t isoDayOfWeek = 1;  // 1=Monday..7=Sunday (ISO 8601)
    uint8_t hour = 0;
    uint8_t minute = 0;
};

class ScheduleEngine {
public:
    // Evaluates the schedule at most once per calendar minute (edge-triggered on
    // nowEpochSeconds crossing into a new minute), so a matching record fires exactly once
    // rather than on every loop() iteration for the whole 60s window it's due. Matching, active
    // records get appended to `due` (capacity `dueCapacity`, ScheduleList entries always fit
    // since it's sized board::SCHEDULE_MAX_RECORDS) with the watering-percentage adjustment
    // already applied; returns how many were appended. Skips entirely when `scheduleActive` is
    // false or `rainSkip` currently applies — this only gates scheduled runs, never manual/MQTT.
    uint8_t tick(const WallClock& now,
                int64_t nowEpochSeconds,
                const ScheduleList& schedule,
                bool scheduleActive,
                const RainSkip& rainSkip,
                int16_t wateringPercentage,
                WateringRequest* due,
                uint8_t dueCapacity);

private:
    int64_t lastEvaluatedMinuteIndex_ = -1;
};

}  // namespace irrigator::domain
