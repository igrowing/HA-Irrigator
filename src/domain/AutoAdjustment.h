#pragma once

#include <stdint.h>
#include "board_config.h"

namespace irrigator::domain {

// Weather-based watering-percentage adjustment. schedule.json's per-zone
// duration_min is already the base runtime at board::AUTOADJUSTMENT_BASE_TEMP_C, so this computes
// a single percentage the same way the existing manual watering_percentage works — no separate
// mm-of-water/flow-rate conversion step (see the resolved design decision in the feature plan).
//
// avgPastTempC is the average of the daily peak (max) temperatures over the trailing
// board::AUTOADJUSTMENT_PAST_WEATHER_DAYS. rainMm is the rain total the formula treats as already
// fallen: the same rolling total of past rainfall, plus the next-12h forecast when
// effectiveRainMm() (below) decides that forecast is confident enough to count.
//
// Percentage = 100 + (avgPastTempC - baseTempC) * tempModifierPctPerDegree
//                   - rainMm * rainModifierPctPerMm
// then: below lowerLimitPercent -> 0 (skip the cycle entirely); above upperLimitPercent -> clamped
// to upperLimitPercent. Matches DurationAdjuster.cpp's rounding convention (nearest integer).
int16_t computeAutoCorrectedPercentage(float avgPastTempC,
                                       float rainMm,
                                       int16_t lowerLimitPercent,
                                       int16_t upperLimitPercent);

// Step 1: the rain total fed into computeAutoCorrectedPercentage(). Past rainfall always counts;
// the whole next-12h forecast amount is added to it, as if it had already fallen, only when the
// forecast is confident enough — probability at or over thresholdProbabilityPercent. Otherwise the
// forecast is ignored. The threshold is user-configurable (Settings::rainForecastProbabilityPercent),
// passed in rather than read from board_config.h directly. There is deliberately no amount
// threshold: the formula already scales linearly with the amount, so a trace forecast barely moves
// the result and a gate would only add a cliff. Example with the default 70%: 3mm past + 6mm
// forecast at 70% -> 9mm; the same forecast at 69% -> 3mm.
float effectiveRainMm(float rainPastDaysMm,
                      float rainProbabilityPercent,
                      float rainAmountNextWindowMm,
                      int16_t thresholdProbabilityPercent);

}  // namespace irrigator::domain
