#include "ScheduleEngine.h"

#include "DurationAdjuster.h"

namespace irrigator::domain {

uint8_t ScheduleEngine::tick(const WallClock& now,
                             int64_t nowEpochSeconds,
                             const ScheduleList& schedule,
                             bool scheduleActive,
                             const RainSkip& rainSkip,
                             int16_t wateringPercentage,
                             WateringRequest* due,
                             uint8_t dueCapacity) {
    const int64_t minuteIndex = nowEpochSeconds / 60;
    if (minuteIndex == lastEvaluatedMinuteIndex_) {
        return 0;
    }
    lastEvaluatedMinuteIndex_ = minuteIndex;

    if (!scheduleActive || rainSkip.shouldSkipScheduledRun(nowEpochSeconds)) {
        return 0;
    }

    uint8_t count = 0;
    for (uint8_t i = 0; i < schedule.count && count < dueCapacity; ++i) {
        const ScheduleRecord& record = schedule.records[i];
        if (!record.active || record.dayOfWeek != now.isoDayOfWeek) {
            continue;
        }

        // startTime is already validated as HH:MM by parseSchedule()/validateScheduleRecord()
        // at load time, so plain digit arithmetic here is safe.
        const int hour = (record.startTime[0] - '0') * 10 + (record.startTime[1] - '0');
        const int minute = (record.startTime[3] - '0') * 10 + (record.startTime[4] - '0');
        if (hour != now.hour || minute != now.minute) {
            continue;
        }

        WateringRequest request;
        request.zone = record.zone;
        request.durationMin = applyWateringPercentage(record.durationMin, wateringPercentage);
        request.reason = RunReason::Schedule;
        if (request.durationMin == 0) {
            continue;  // Adjustment percentage brought this run down to nothing.
        }
        due[count++] = request;
    }
    return count;
}

}  // namespace irrigator::domain
