#pragma once

#include <Arduino.h>
#include <time.h>

namespace irrigator::drivers {

class Clock {
public:
    bool begin(const char* timezoneSpec);
    bool syncFromNtp();
    bool now(struct tm& out) const;
    time_t epoch() const;
};

}  // namespace irrigator::drivers