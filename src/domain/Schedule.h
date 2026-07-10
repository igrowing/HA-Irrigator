#pragma once

#include <stdint.h>

#include "board_config.h"

namespace irrigator::domain {

struct ScheduleRecord {
    uint8_t zone = 0;
    uint8_t dayOfWeek = 0;  // 1..7
    char startTime[6] = ""; // HH:MM
    uint8_t durationMin = 0;
    bool active = false;
};

static bool isValidStartTime(const char* value);
bool validateScheduleRecord(const ScheduleRecord& record);

}  // namespace irrigator::domain