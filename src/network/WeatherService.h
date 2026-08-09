#pragma once

#include <stdint.h>

namespace irrigator::network {

struct GeoLocationResult {
    bool success = false;
    // Set only when success is false — see WeatherFetchResult::errorReason for the rationale.
    // 200 bytes: comfortably fits a message plus free-heap/content-length/timing diagnostics
    // without truncating them away (an 80-byte buffer previously cut the heap figure off
    // mid-number; 160 left too little headroom once httpCode/timing were added on top).
    char errorReason[200] = {};
    float latitude = 0.0f;
    float longitude = 0.0f;
    char city[32] = {};
    // ip-api.com's `offset`: the located region's UTC offset in seconds *at the moment of this
    // lookup*, DST included. Only meaningful when success is true. The caller converts it to the
    // plain standard-time hour offset config.json stores — see
    // domain::standardUtcOffsetHoursFromCurrent.
    int32_t utcOffsetSeconds = 0;
};

struct WeatherFetchResult {
    bool success = false;
    // Set only when success is false — what went wrong (HTTP code, parse error, insufficient
    // data, ...), for surfacing in alerts/logs without the caller needing to re-derive it. 200
    // bytes: see GeoLocationResult::errorReason for why this isn't 80 anymore.
    char errorReason[200] = {};
    // Historical (trailing board::AUTOADJUSTMENT_PAST_WEATHER_DAYS) — the correction formula's
    // inputs (Step 2). Never forecast. avgPastTempC is the average of the daily peak (max)
    // temperatures, not a 24h mean — see domain::summarizePastAverageDailyMaxTemperature.
    float avgPastTempC = 0.0f;
    float rainPastDaysMm = 0.0f;
    // Forecast (next board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS) — the rain-skip check's inputs
    // (Step 1) only. Never used by the adjustment formula.
    float maxRainProbabilityPercent = 0.0f;
    float rainAmountNextWindowMm = 0.0f;
};

// Blocking HTTP clients for geolocation + weather lookups.
// Mirrors OtaService's synchronous-GET-with-status-check style: these are infrequent, bounded
// calls (once at boot for location, at most every 5 minutes for weather), not hot-path work, so
// blocking loop() for their duration follows the same accepted precedent as OTA.
class WeatherService {
public:
    // ip-api.com is HTTP-only on its free tier; only called while Settings::locationKnown is
    // false.
    GeoLocationResult fetchGeoLocation();

    // Open-Meteo: primary provider, no API key needed. Fetches both the trailing
    // AUTOADJUSTMENT_PAST_WEATHER_DAYS (temperature average + rainfall total) and the forward
    // AUTOADJUSTMENT_FORECAST_WINDOW_HOURS (rain-skip check) in one request. nowEpochUtc anchors
    // both windows (see the .cpp for the index arithmetic this relies on).
    WeatherFetchResult fetchForecastOpenMeteo(float latitude, float longitude, int64_t nowEpochUtc);

    // WeatherAPI.com: fallback used only when Open-Meteo fails (Phase 4). Returns success=false
    // immediately if apiKey is empty rather than attempting a call that can't succeed.
    WeatherFetchResult fetchForecastWeatherApi(float latitude,
                                               float longitude,
                                               int64_t nowEpochUtc,
                                               const char* apiKey);
};

}  // namespace irrigator::network
