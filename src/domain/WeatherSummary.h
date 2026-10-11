#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace irrigator::domain {

// Index into Open-Meteo's hourly arrays corresponding to "now", given how many past_days were
// requested (see network::WeatherService::fetchForecastOpenMeteo, which always also passes
// timezone=UTC) — the arrays start at 00:00 UTC of (today - pastDays), one entry per hour, so
// this is pure arithmetic rather than timestamp parsing.
size_t openMeteoNowIndex(int64_t nowEpochUtc, size_t pastDays);

// Averages dailyMaxTemperatureC over its first pastWeatherDays entries — the historical daytime
// *peak* temperature term the adjustment formula (Step 2) uses. Uses the daily high rather than a
// 24h mean: the mean folds in cool nights and reads well below the daytime temperature that
// actually drives watering need. Open-Meteo's daily arrays start at (today - pastDays), so the
// first pastWeatherDays entries are exactly the past window with today excluded, mirroring
// summarizePastRainfall. Returns false (output untouched) if the array is shorter than
// pastWeatherDays, or if any entry inside the window is null (a missing reading is not 0°C).
bool summarizePastAverageDailyMaxTemperature(JsonArrayConst dailyMaxTemperatureC,
                                             size_t pastWeatherDays,
                                             float& outAvgMaxTempC);

// Sums dailyRainSumMm over its first pastWeatherDays entries — total precipitation over the same
// historical window the temperature average above covers (Step 2's rain-subtraction term).
// Returns false if the array is shorter than pastWeatherDays or any entry inside the window is null
// (a missing reading is not 0mm - it would silently raise the percentage).
bool summarizePastRainfall(JsonArrayConst dailyRainSumMm,
                           size_t pastWeatherDays,
                           float& outRainPastDaysMm);

// Forecast-rain window (Step 1) — always forward-looking, independent of the historical
// window above: max rain probability and total rain amount over
// [nowIndex, nowIndex+forecastWindowHours). Returns false if either array is too short to cover
// the requested window.
bool summarizeForecastRainWindow(JsonArrayConst hourlyRainProbabilityPercent,
                                 JsonArrayConst hourlyRainAmountMm,
                                 size_t nowIndex,
                                 size_t forecastWindowHours,
                                 float& outMaxRainProbabilityPercent,
                                 float& outRainAmountNextWindowMm);

// WeatherAPI.com fallback's forecast-rain window: flattens forecastDays[].hour[] (each hour carrying
// time_epoch/chance_of_rain/precip_mm) and maxes/sums the first windowHours entries at or after
// nowEpochUtc — WeatherAPI, unlike Open-Meteo, doesn't guarantee a fixed array index for "now".
// Returns false if fewer than windowHours qualifying entries are found. outQualifyingHours is
// always set (even on failure) to how many entries actually qualified, so a caller can tell "found
// 0" (e.g. every hour missing/misnamed time_epoch, defaulting to epoch 0 and always comparing as
// past) apart from "found some but not enough" (a genuinely short response).
bool summarizeWeatherApiForecastRainWindow(JsonArrayConst forecastDays,
                                           int64_t nowEpochUtc,
                                           size_t windowHours,
                                           float& outMaxRainProbabilityPercent,
                                           float& outRainAmountMm,
                                           size_t& outQualifyingHours);

}  // namespace irrigator::domain
