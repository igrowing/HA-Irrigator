#pragma once

#include <Arduino.h>
#include <time.h>

namespace irrigator::drivers {

class Clock {
public:
    // utcOffsetHours: plain UTC offset (-13..+13, no DST baked in) — matches the spec's
    // Config.json Timezone field. EU DST (see domain::isEuDstActive) is applied on top of this
    // dynamically in now(), not baked into the system clock, so it can't drift if the device
    // stays up across a DST transition.
    bool begin(int8_t utcOffsetHours);

    // Waits (bounded, ~5s) for NTP sync, then writes the result to the DS3231.
    bool syncFromNtp();

    // Sets the system clock from the DS3231 — used as a boot-time fallback before/without NTP.
    bool restoreFromRtc();

    // Local wall-clock time: configured UTC offset + EU DST when in effect. Returns false if
    // the system clock hasn't been set yet (by NTP or restoreFromRtc()).
    bool now(struct tm& out) const;

    // Same conversion as now(), but for an arbitrary UTC epoch instead of "right now" - e.g.
    // rendering a stored timestamp (main.ino's lastWeatherUpdateEpoch) in local wall-clock terms.
    // Returns false if utcEpoch predates the clock ever having been set (same sanity floor now()
    // uses), since that means the caller hasn't recorded a real timestamp yet.
    bool localTimeAt(time_t utcEpoch, struct tm& out) const;

    // Raw UTC epoch — unaffected by offset/DST, safe for RainSkip/ScheduleEngine's minute-edge
    // detection.
    time_t epoch() const;

private:
    bool writeToRtc(const struct tm& utcTime);
    bool readFromRtc(struct tm& out) const;

    int8_t utcOffsetHours_ = 0;
};

}  // namespace irrigator::drivers
