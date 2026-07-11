#include "DstRules.h"

#include "CalendarMath.h"

namespace irrigator::domain {

namespace {

int64_t lastSundayOfMonthEpochUtc(int year, unsigned month, int hourUtc) {
    // Only ever called for March/October, both 31-day months.
    int64_t days = daysFromCivil(year, month, 31);
    days -= weekdayFromDays(days);  // Step back to the Sunday on/before the month's last day.
    return days * 86400 + static_cast<int64_t>(hourUtc) * 3600;
}

}  // namespace

bool isEuDstActive(int64_t utcEpochSeconds) {
    const int64_t days =
        utcEpochSeconds >= 0 ? utcEpochSeconds / 86400 : (utcEpochSeconds - 86399) / 86400;
    const CivilDate civil = civilFromDays(days);

    const int64_t marchTransition = lastSundayOfMonthEpochUtc(civil.year, 3, 1);
    const int64_t octoberTransition = lastSundayOfMonthEpochUtc(civil.year, 10, 1);

    return utcEpochSeconds >= marchTransition && utcEpochSeconds < octoberTransition;
}

}  // namespace irrigator::domain
