#include "Clock.h"

#include <Wire.h>
#include <string.h>
#include <sys/time.h>

#include "Logger.h"
#include "board_config.h"

namespace irrigator::drivers {

namespace {

uint8_t toBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t fromBcd(uint8_t value) {
    return static_cast<uint8_t>(((value >> 4) * 10) + (value & 0x0F));
}

// Howard Hinnant's days_from_civil, used instead of timegm() (a GNU/BSD extension not
// guaranteed present in the ESP32 toolchain's newlib) to turn a UTC struct tm into epoch
// seconds without relying on the process TZ.
int64_t epochFromUtcTm(const struct tm& utcTm) {
    int y = (utcTm.tm_year + 1900) - (utcTm.tm_mon + 1 <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>((utcTm.tm_mon + 1 + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(utcTm.tm_mday) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * 86400 + utcTm.tm_hour * 3600 + utcTm.tm_min * 60 + utcTm.tm_sec;
}

}  // namespace

bool Clock::begin(int8_t utcOffsetHours) {
    configTime(static_cast<long>(utcOffsetHours) * 3600L, 0, "pool.ntp.org");
    return true;
}

bool Clock::syncFromNtp() {
    struct tm localTime;
    const bool ok = getLocalTime(&localTime, 5000);
    if (!ok) {
        Log.warn("NTP time is not available yet");
        return false;
    }

    const time_t nowUtc = time(nullptr);
    struct tm utcTm;
    gmtime_r(&nowUtc, &utcTm);
    writeToRtc(utcTm);
    return true;
}

bool Clock::restoreFromRtc() {
    struct tm utcTm;
    if (!readFromRtc(utcTm)) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epochFromUtcTm(utcTm));
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    Log.info("System clock restored from DS3231 (NTP not yet available)");
    return true;
}

bool Clock::now(struct tm& out) const {
    return getLocalTime(&out, 10);
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
