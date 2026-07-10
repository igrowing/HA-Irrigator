#include "Clock.h"

#include "Logger.h"

namespace irrigator::drivers {

bool Clock::begin(const char* timezoneSpec) {
    if (timezoneSpec != nullptr && timezoneSpec[0] != '\0') {
        setenv("TZ", timezoneSpec, 1);
        tzset();
    }
    configTime(0, 0, "pool.ntp.org");
    return true;
}

bool Clock::syncFromNtp() {
    struct tm timeinfo;
    const bool ok = getLocalTime(&timeinfo, 5000);
    if (!ok) {
        Log.warn("NTP time is not available yet");
    }
    // TODO: write synchronized time to DS3231 at board::DS3231_ADDRESS.
    return ok;
}

bool Clock::now(struct tm& out) const {
    return getLocalTime(&out, 10);
}

time_t Clock::epoch() const {
    time_t current;
    time(&current);
    return current;
}

}  // namespace irrigator::drivers