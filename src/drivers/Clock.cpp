#include "Clock.h"

#include <Wire.h>
#include <string.h>
#include <sys/time.h>

#include "Logger.h"
#include "board_config.h"
#include "domain/CalendarMath.h"
#include "domain/DstRules.h"

namespace irrigator::drivers {

namespace {

// Below this, the system clock is still at its post-boot default and hasn't been set by NTP or
// the DS3231 yet (~2023-11-14); used purely as a "is it set at all" sanity check, not a real
// calendar bound.
constexpr time_t kMinPlausibleEpoch = 1700000000;

uint8_t toBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t fromBcd(uint8_t value) {
    return static_cast<uint8_t>(((value >> 4) * 10) + (value & 0x0F));
}

}  // namespace

bool Clock::begin(int8_t utcOffsetHours) {
    utcOffsetHours_ = utcOffsetHours;
    // gmtOffset/daylightOffset both 0: the system clock always tracks plain UTC. The configured
    // offset and EU DST are applied on read, in now() below, instead of being baked in here.
    configTime(0, 0, "pool.ntp.org");
    return true;
}

bool Clock::syncFromNtp() {
    struct tm utcTm;
    const bool ok = getLocalTime(&utcTm, 5000);  // gmtOffset is 0, so this is already UTC.
    if (!ok) {
        Log.warn("NTP time is not available yet");
        return false;
    }
    writeToRtc(utcTm);
    return true;
}

bool Clock::restoreFromRtc() {
    struct tm utcTm;
    if (!readFromRtc(utcTm)) {
        return false;
    }

    const int64_t epochUtc = domain::epochFromUtc(utcTm.tm_year + 1900,
                                                  static_cast<unsigned>(utcTm.tm_mon + 1),
                                                  static_cast<unsigned>(utcTm.tm_mday),
                                                  static_cast<unsigned>(utcTm.tm_hour),
                                                  static_cast<unsigned>(utcTm.tm_min),
                                                  static_cast<unsigned>(utcTm.tm_sec));
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epochUtc);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    Log.info("System clock restored from DS3231 (NTP not yet available)");
    return true;
}

bool Clock::now(struct tm& out) const {
    time_t utcNow;
    time(&utcNow);
    return localTimeAt(utcNow, out);
}

bool Clock::localTimeAt(time_t utcEpoch, struct tm& out) const {
    if (utcEpoch < kMinPlausibleEpoch) {
        return false;
    }

    const bool dst = domain::isEuDstActive(static_cast<int64_t>(utcEpoch));
    const time_t localEpoch =
        utcEpoch + static_cast<time_t>(utcOffsetHours_) * 3600 + (dst ? 3600 : 0);
    gmtime_r(&localEpoch, &out);  // gmtime_r on a pre-shifted epoch yields the correct local fields.
    return true;
}

time_t Clock::epoch() const {
    time_t current;
    time(&current);
    return current;
}

bool Clock::writeToRtc(const struct tm& utcTime) {
    Wire.beginTransmission(board::DS3231_ADDRESS);
    Wire.write(0x00);  // Seconds register — start of the burst-write window below.
    Wire.write(toBcd(static_cast<uint8_t>(utcTime.tm_sec)));
    Wire.write(toBcd(static_cast<uint8_t>(utcTime.tm_min)));
    Wire.write(toBcd(static_cast<uint8_t>(utcTime.tm_hour)));  // Bit 6 = 0 selects 24h mode.
    Wire.write(toBcd(static_cast<uint8_t>(utcTime.tm_wday == 0 ? 7 : utcTime.tm_wday)));
    Wire.write(toBcd(static_cast<uint8_t>(utcTime.tm_mday)));
    Wire.write(toBcd(static_cast<uint8_t>(utcTime.tm_mon + 1)));
    Wire.write(toBcd(static_cast<uint8_t>((utcTime.tm_year + 1900) % 100)));
    const uint8_t error = Wire.endTransmission();
    if (error != 0) {
        Log.printf("[ERROR] DS3231 write failed: %u\n", static_cast<unsigned>(error));
        return false;
    }
    return true;
}

bool Clock::readFromRtc(struct tm& out) const {
    Wire.beginTransmission(board::DS3231_ADDRESS);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) {
        Log.error("DS3231 read: register pointer write failed");
        return false;
    }

    if (Wire.requestFrom(static_cast<int>(board::DS3231_ADDRESS), 7) != 7) {
        Log.error("DS3231 read: short response");
        return false;
    }

    const uint8_t sec = Wire.read();
    const uint8_t minute = Wire.read();
    const uint8_t hourRaw = Wire.read();
    Wire.read();  // Day-of-week register — unused, tm_wday is left at 0 (caller doesn't need it).
    const uint8_t date = Wire.read();
    const uint8_t monthRaw = Wire.read();
    const uint8_t year = Wire.read();

    memset(&out, 0, sizeof(out));
    out.tm_sec = fromBcd(sec & 0x7F);
    out.tm_min = fromBcd(minute & 0x7F);
    out.tm_hour = fromBcd(hourRaw & 0x3F);  // Mask off the 12/24h + AM/PM bits; we always write 24h.
    out.tm_mday = fromBcd(date & 0x3F);
    out.tm_mon = fromBcd(monthRaw & 0x1F) - 1;
    out.tm_year = fromBcd(year) + 100;  // DS3231 stores 00-99 => 2000-2099; tm_year is since 1900.
    return true;
}

}  // namespace irrigator::drivers
