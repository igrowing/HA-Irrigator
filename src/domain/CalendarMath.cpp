#include "CalendarMath.h"

namespace irrigator::domain {

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    const int y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>((month + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

CivilDate civilFromDays(int64_t daysSinceEpoch) {
    const int64_t z = daysSinceEpoch + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;

    CivilDate result;
    result.year = static_cast<int>(y + (month <= 2 ? 1 : 0));
    result.month = month;
    result.day = day;
    return result;
}

int weekdayFromDays(int64_t daysSinceEpoch) {
    // 1970-01-01 (day 0) was a Thursday (weekday 4 in a Sun=0..Sat=6 scheme).
    return static_cast<int>(daysSinceEpoch >= -4 ? (daysSinceEpoch + 4) % 7
                                                  : (daysSinceEpoch + 5) % 7 + 6);
}

int64_t epochFromUtc(int year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second) {
    return daysFromCivil(year, month, day) * 86400 + static_cast<int64_t>(hour) * 3600 +
           static_cast<int64_t>(minute) * 60 + static_cast<int64_t>(second);
}

}  // namespace irrigator::domain
