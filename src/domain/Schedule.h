#pragma once

#include <stddef.h>
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

struct ScheduleList {
    ScheduleRecord records[board::SCHEDULE_MAX_RECORDS];
    uint8_t count = 0;
};

bool isValidStartTime(const char* value);
bool validateScheduleRecord(const ScheduleRecord& record);

// Whole-file reject semantics: malformed JSON, more than board::SCHEDULE_MAX_RECORDS entries,
// or any single invalid record fails the whole parse (returns false) rather than silently
// dropping/clamping the bad entry — callers should mark the schedule inactive on failure
// instead of guessing at intent (see agents.md Section 3).
bool parseSchedule(const char* json, size_t length, ScheduleList& out);
size_t serializeSchedule(const ScheduleList& list, char* buffer, size_t bufferSize);

}  // namespace irrigator::domain
