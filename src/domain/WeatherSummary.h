#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace irrigator::domain {

// Index into Open-Meteo's hourly arrays corresponding to "now", given how many past_days were
// requested (see network::WeatherService::fetchForecastOpenMeteo, which always also passes
// timezone=UTC) — the arrays start at 00:00 UTC of (today - pastDays), one entry per hour, so
// this is pure arithmetic rather than timestamp parsing.
size_t openMeteoNowIndex(int64_t nowEpochUtc, size_t pastDays);

// Averages hourlyTemperatureC over the pastWeatherDays*24 hours immediately preceding nowIndex —
// the historical moving-average temperature term the adjustment formula (Step 2) uses. Returns
// false (output untouched) if there isn't that much history before nowIndex in the array.
bool summarizePastAverageTemperature(JsonArrayConst hourlyTemperatureC,
                                     size_t nowIndex,
                                     size_t pastWeatherDays,
                                     float& outAvgTempC);

// Sums dailyRainSumMm over its first pastWeatherDays entries — total precipitation over the same
// historical window the temperature average above covers (Step 2's rain-subtraction term).
// Returns false if the array is shorter than pastWeatherDays.
bool summarizePastRainfall(JsonArrayConst dailyRainSumMm,
                           size_t pastWeatherDays,
                           float& outRainPastDaysMm);

// Rain-skip forecast window (Step 1) — always forward-looking, independent of the historical
// window above: max rain probability and total rain amount over
// [nowIndex, nowIndex+forecastWindowHours). Returns false if either array is too short to cover
// the requested window.
bool summarizeForecastRainWindow(JsonArrayConst hourlyRainProbabilityPercent,
                                 JsonArrayConst hourlyRainAmountMm,
                                 size_t nowIndex,
                                 size_t forecastWindowHours,
                                 float& outMaxRainProbabilityPercent,
                                 float& outRainAmountNextWindowMm);

// WeatherAPI.com fallback's rain-skip window: flattens forecastDays[].hour[] (each hour carrying
// time_epoch/chance_of_rain/precip_mm) and maxes/sums the first windowHours entries at or after
// nowEpochUtc — WeatherAPI, unlike Open-Meteo, doesn't guarantee a fixed array index for "now".
// Returns false if fewer than windowHours qualifying entries are found.
bool summarizeWeatherApiForecastRainWindow(JsonArrayConst forecastDays,
                                           int64_t nowEpochUtc,
                                           size_t windowHours,
                                           float& outMaxRainProbabilityPercent,
                                           float& outRainAmountMm);

}  // namespace irrigator::domain
