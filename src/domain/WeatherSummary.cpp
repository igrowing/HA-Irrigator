#include "WeatherSummary.h"

namespace irrigator::domain {

namespace {

constexpr size_t kHoursPerDay = 24;

bool windowSum(JsonArrayConst arr, size_t startIndex, size_t count, float& outSum) {
    if (arr.size() < startIndex + count) {
        return false;
    }
    outSum = 0.0f;
    size_t i = 0;
    for (JsonVariantConst v : arr) {
        if (i >= startIndex && i < startIndex + count) {
            outSum += v.as<float>();
        }
        ++i;
    }
    return true;
}

// Tracks the max via "first qualifying value sets the baseline" rather than starting at 0.0f —
// values like temperature can be genuinely negative, so seeding at 0 would silently under-report
// the max on an all-negative window.
bool windowMax(JsonArrayConst arr, size_t startIndex, size_t count, float& outMax) {
    if (arr.size() < startIndex + count) {
        return false;
    }
    outMax = 0.0f;
    size_t i = 0;
    bool any = false;
    for (JsonVariantConst v : arr) {
        if (i >= startIndex && i < startIndex + count) {
            const float value = v.as<float>();
            if (!any || value > outMax) {
                outMax = value;
            }
            any = true;
        }
        ++i;
    }
    return true;
}

}  // namespace

size_t openMeteoNowIndex(int64_t nowEpochUtc, size_t pastDays) {
    const size_t hourOfDayUtc = static_cast<size_t>((nowEpochUtc % 86400) / 3600);
    return pastDays * kHoursPerDay + hourOfDayUtc;
}

bool summarizePastAverageDailyMaxTemperature(JsonArrayConst dailyMaxTemperatureC,
                                             size_t pastWeatherDays,
                                             float& outAvgMaxTempC) {
    if (dailyMaxTemperatureC.size() < pastWeatherDays) {
        return false;
    }
    float sum = 0.0f;
    size_t i = 0;
    for (JsonVariantConst v : dailyMaxTemperatureC) {
        if (i >= pastWeatherDays) {
            break;
        }
        sum += v.as<float>();
        ++i;
    }
    outAvgMaxTempC = sum / static_cast<float>(pastWeatherDays);
    return true;
}

bool summarizePastRainfall(JsonArrayConst dailyRainSumMm,
                           size_t pastWeatherDays,
                           float& outRainPastDaysMm) {
    if (dailyRainSumMm.size() < pastWeatherDays) {
        return false;
    }
    float sum = 0.0f;
    size_t i = 0;
    for (JsonVariantConst v : dailyRainSumMm) {
        if (i >= pastWeatherDays) {
            break;
        }
        sum += v.as<float>();
        ++i;
    }
    outRainPastDaysMm = sum;
    return true;
}

bool summarizeForecastRainWindow(JsonArrayConst hourlyRainProbabilityPercent,
                                 JsonArrayConst hourlyRainAmountMm,
                                 size_t nowIndex,
                                 size_t forecastWindowHours,
                                 float& outMaxRainProbabilityPercent,
                                 float& outRainAmountNextWindowMm) {
    return windowMax(hourlyRainProbabilityPercent, nowIndex, forecastWindowHours, outMaxRainProbabilityPercent) &&
           windowSum(hourlyRainAmountMm, nowIndex, forecastWindowHours, outRainAmountNextWindowMm);
}

bool summarizeWeatherApiForecastRainWindow(JsonArrayConst forecastDays,
                                           int64_t nowEpochUtc,
                                           size_t windowHours,
                                           float& outMaxRainProbabilityPercent,
                                           float& outRainAmountMm,
                                           size_t& outQualifyingHours) {
    float probMax = 0.0f;
    float precipSum = 0.0f;
    size_t windowCount = 0;
    for (JsonObjectConst day : forecastDays) {
        for (JsonObjectConst hour : day["hour"].as<JsonArrayConst>()) {
            if (windowCount >= windowHours) {
                break;
            }
            const int64_t epoch = hour["time_epoch"] | static_cast<int64_t>(0);
            if (epoch < nowEpochUtc) {
                continue;
            }
            const float chance = hour["chance_of_rain"] | 0.0f;
            if (windowCount == 0 || chance > probMax) {
                probMax = chance;
            }
            precipSum += hour["precip_mm"] | 0.0f;
            ++windowCount;
        }
    }
    outQualifyingHours = windowCount;
    if (windowCount < windowHours) {
        return false;
    }

    outMaxRainProbabilityPercent = probMax;
    outRainAmountMm = precipSum;
    return true;
}

}  // namespace irrigator::domain
