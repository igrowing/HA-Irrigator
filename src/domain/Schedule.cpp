#include "Schedule.h"

namespace irrigator::domain {

static bool isDigit(char value) {
    return value >= '0' && value <= '9';
}

static bool isValidStartTime(const char* value) {
    if (value == nullptr) {
        return false;
    }
    if (!isDigit(value[0]) || !isDigit(value[1]) || value[2] != ':' || !isDigit(value[3]) ||
        !isDigit(value[4]) || value[5] != '\0') {
        return false;
    }

    const uint8_t hour = static_cast<uint8_t>((value[0] - '0') * 10 + (value[1] - '0'));
    const uint8_t minute = static_cast<uint8_t>((value[3] - '0') * 10 + (value[4] - '0'));
    return hour <= 23 && minute <= 59;
}

bool validateScheduleRecord(const ScheduleRecord& record) {
    return record.zone >= 1 && record.zone <= board::ZONE_COUNT && record.dayOfWeek >= 1 &&
           record.dayOfWeek <= 7 && isValidStartTime(record.startTime) &&
           record.durationMin >= board::SCHEDULE_DURATION_MIN_MINUTES &&
           record.durationMin <= board::SCHEDULE_DURATION_MAX_MINUTES;
}

}  // namespace irrigator::domain