#include "Schedule.h"

#include <ArduinoJson.h>
#include <stdio.h>

namespace irrigator::domain {

namespace {

bool isDigit(char value) {
    return value >= '0' && value <= '9';
}

}  // namespace

bool isValidStartTime(const char* value) {
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

bool parseSchedule(const char* json, size_t length, ScheduleList& out) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, length);
    if (error || !doc.is<JsonArrayConst>()) {
        return false;
    }

    JsonArrayConst array = doc.as<JsonArrayConst>();
    if (array.size() > board::SCHEDULE_MAX_RECORDS) {
        return false;
    }

    ScheduleList parsed;
    for (JsonVariantConst entry : array) {
        ScheduleRecord record;
        record.zone = entry["zone"] | 0;
        record.dayOfWeek = entry["day_of_week"] | 0;
        snprintf(record.startTime, sizeof(record.startTime), "%s", entry["start_time"] | "");
        record.durationMin = entry["duration_min"] | 0;
        record.active = entry["active"] | false;

        if (!validateScheduleRecord(record)) {
            return false;
        }

        parsed.records[parsed.count++] = record;
    }

    out = parsed;
    return true;
}

size_t serializeSchedule(const ScheduleList& list, char* buffer, size_t bufferSize) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    for (uint8_t i = 0; i < list.count; ++i) {
        JsonObject entry = array.add<JsonObject>();
        entry["zone"] = list.records[i].zone;
        entry["day_of_week"] = list.records[i].dayOfWeek;
        entry["start_time"] = list.records[i].startTime;
        entry["duration_min"] = list.records[i].durationMin;
        entry["active"] = list.records[i].active;
    }
    return serializeJson(doc, buffer, bufferSize);
}

}  // namespace irrigator::domain
