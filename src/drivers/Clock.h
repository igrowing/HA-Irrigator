#pragma once

#include <Arduino.h>
#include <time.h>

namespace irrigator::drivers {

class Clock {
public:
    // utcOffsetHours: plain UTC offset (-13..+13, no DST rules) — matches the spec's
    // Config.json Timezone field, deliberately simpler than a POSIX TZ database string.
    bool begin(int8_t utcOffsetHours);
    bool syncFromNtp();
    bool now(struct tm& out) const;
    time_t epoch() const;
};

}  // namespace irrigator::drivers