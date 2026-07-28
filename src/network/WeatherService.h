#pragma once

#include <stdint.h>

namespace irrigator::network {

struct GeoLocationResult {
    bool success = false;
    float latitude = 0.0f;
    float longitude = 0.0f;
    char city[32] = {};
};

struct WeatherFetchResult {
    bool success = false;
    // Historical (trailing board::AUTOADJUSTMENT_PAST_WEATHER_DAYS) — the correction formula's
    // inputs (Step 2). Never forecast.
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
