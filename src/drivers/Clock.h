#pragma once

#include <Arduino.h>
#include <time.h>

namespace irrigator::drivers {

class Clock {
public:
    // utcOffsetHours: plain UTC offset (-13..+13, no DST rules) — matches the spec's
    // Config.json Timezone field, deliberately simpler than a POSIX TZ database string.
    bool begin(int8_t utcOffsetHours);

    // Waits (bounded, ~5s) for NTP sync, then writes the result to the DS3231.
    bool syncFromNtp();

    // Sets the system clock from the DS3231 — used as a boot-time fallback before/without NTP.
    bool restoreFromRtc();

    bool now(struct tm& out) const;
    time_t epoch() const;

private:
    bool writeToRtc(const struct tm& utcTime);
    bool readFromRtc(struct tm& out) const;
};

}  // namespace irrigator::drivers
