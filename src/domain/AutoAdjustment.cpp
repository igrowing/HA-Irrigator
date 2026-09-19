#include "AutoAdjustment.h"

namespace irrigator::domain {

int16_t computeAutoCorrectedPercentage(float avgPastTempC,
                                       float rainPastDaysMm,
                                       float baseTempC,
                                       float tempModifierPctPerDegree,
                                       float rainModifierPctPerMm,
                                       int16_t lowerLimitPercent,
                                       int16_t upperLimitPercent) {
    float percentage = 100.0f + (avgPastTempC - baseTempC) * tempModifierPctPerDegree -
                        rainPastDaysMm * rainModifierPctPerMm;

    if (percentage < static_cast<float>(lowerLimitPercent)) {
        return 0;  // Below the lower limit skips the cycle entirely (spec Step 5), not clamps to it.
    }
    if (percentage > static_cast<float>(upperLimitPercent)) {
        percentage = static_cast<float>(upperLimitPercent);
    }
    if (percentage < 0.0f) {
        percentage = 0.0f;
    }
    return static_cast<int16_t>(percentage + 0.5f);
}

bool shouldSkipForRain(float rainProbabilityPercent,
                       float rainAmountNextWindowMm,
                       int16_t thresholdProbabilityPercent,
                       int16_t thresholdAmountMm) {
    return rainProbabilityPercent >= static_cast<float>(thresholdProbabilityPercent) &&
           rainAmountNextWindowMm > static_cast<float>(thresholdAmountMm);
}

}  // namespace irrigator::domain
