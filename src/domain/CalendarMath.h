#pragma once

#include <stddef.h>
#include <stdint.h>

namespace irrigator::domain {

struct CivilDate {
    int year;
    unsigned month;  // 1-12
    unsigned day;    // 1-31
};

// Howard Hinnant's days_from_civil / civil_from_days algorithms (proleptic Gregorian, UTC,
// days since 1970-01-01). Shared by anything that needs to convert between calendar dates and
// epoch seconds without relying on timegm()/mktime() (not guaranteed available/TZ-safe on the
// ESP32 toolchain) — cmd/skip_rain's ISO8601 parser, the DS3231 read/write, and DstRules all
// used to duplicate this; consolidated here.
int64_t daysFromCivil(int year, unsigned month, unsigned day);
CivilDate civilFromDays(int64_t daysSinceEpoch);
int weekdayFromDays(int64_t daysSinceEpoch);  // 0=Sunday..6=Saturday

int64_t epochFromUtc(int year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second);

// Epoch seconds -> "YYYY-MM-DDTHH:MM:SS" (UTC), null-terminated. Inverse of epochFromUtc; buffer
// must be at least 20 bytes.
void formatIso8601Utc(int64_t epochSeconds, char* buffer, size_t bufferSize);

}  // namespace irrigator::domain
