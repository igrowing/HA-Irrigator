#pragma once

#include <stdint.h>
#include "board_config.h"

namespace irrigator::domain {

// Weather-based watering-percentage adjustment. schedule.json's per-zone
// duration_min is already the base runtime at board::AUTOADJUSTMENT_BASE_TEMP_C, so this computes
// a single percentage the same way the existing manual watering_percentage works — no separate
// mm-of-water/flow-rate conversion step (see the resolved design decision in the feature plan).
//
// Both inputs are historical (trailing board::AUTOADJUSTMENT_PAST_WEATHER_DAYS), not forecast —
// avgPastTempC is the average of the daily peak (max) temperatures over that window, rainPastDaysMm
// a rolling rainfall total. The forecast is only ever consulted for the separate rain-skip check
// (shouldSkipForRain, below).
//
// Percentage = 100 + (avgPastTempC - baseTempC) * tempModifierPctPerDegree
//                   - rainPastDaysMm * rainModifierPctPerMm
// then: below lowerLimitPercent -> 0 (skip the cycle entirely); above upperLimitPercent -> clamped
// to upperLimitPercent. Matches DurationAdjuster.cpp's rounding convention (nearest integer).
//
// baseTempC is the avg-daily-max reading of a "normal" day, so it only means anything alongside
// the daily-max input above - see board_config.h's AUTOADJUSTMENT_BASE_TEMP_C for why that pairing
// is load-bearing. All three coefficients are passed in (from Settings, defaulting to the
// board_config.h constants) rather than read from board_config.h here, so a site can be
// recalibrated without a firmware flash.
int16_t computeAutoCorrectedPercentage(float avgPastTempC,
                                       float rainPastDaysMm,
                                       float baseTempC,
                                       float tempModifierPctPerDegree,
                                       float rainModifierPctPerMm,
                                       int16_t lowerLimitPercent,
                                       int16_t upperLimitPercent);

// Step 1's predictive rain-skip trigger, evaluated against the forecast (the only forward-looking
// input anywhere in auto-adjustment): probability at or over thresholdProbabilityPercent AND
// amount strictly over thresholdAmountMm in the next-12h forecast window. The two comparisons are
// intentionally asymmetric (>= vs >) — both thresholds are user-configurable
// (Settings::rainSkipProbabilityPercent/rainSkipAmountMm), passed in rather than read from
// board_config.h directly.
bool shouldSkipForRain(float rainProbabilityPercent,
                       float rainAmountNextWindowMm,
                       int16_t thresholdProbabilityPercent,
                       int16_t thresholdAmountMm);

}  // namespace irrigator::domain
