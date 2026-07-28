#include "WeatherService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include "../domain/CalendarMath.h"
#include "../domain/WeatherSummary.h"
#include "../drivers/Logger.h"
#include "board_config.h"

namespace irrigator::network {

GeoLocationResult WeatherService::fetchGeoLocation() {
    GeoLocationResult result;

    HTTPClient http;
    if (!http.begin(board::GEO_IP_API_URL)) {
        ::irrigator::drivers::Log.error("Geolocation: could not begin HTTP request");
        return result;
    }

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] Geolocation: HTTP GET failed, code %d\n", httpCode);
        http.end();
        return result;
    }

    JsonDocument filter;
    filter["status"] = true;
    filter["lat"] = true;
    filter["lon"] = true;
    filter["city"] = true;

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
        ::irrigator::drivers::Log.printf("[WARN] Geolocation: JSON parse failed: %s\n", error.c_str());
        return result;
    }

    const char* status = doc["status"] | "";
    if (strcmp(status, "success") != 0) {
        ::irrigator::drivers::Log.warn("Geolocation: provider did not return status=success");
        return result;
    }

    result.latitude = doc["lat"] | 0.0f;
    result.longitude = doc["lon"] | 0.0f;
    const char* city = doc["city"] | "";
    snprintf(result.city, sizeof(result.city), "%s", city);
    result.success = true;
    return result;
}

WeatherFetchResult WeatherService::fetchForecastOpenMeteo(float latitude,
                                                          float longitude,
                                                          int64_t nowEpochUtc) {
    WeatherFetchResult result;

    char url[224];
    snprintf(url,
            sizeof(url),
            "%s?latitude=%.4f&longitude=%.4f&hourly=temperature_2m,precipitation_probability,"
            "precipitation&daily=precipitation_sum&past_days=%u&forecast_days=2&timezone=UTC",
            board::OPEN_METEO_BASE_URL,
            latitude,
            longitude,
            static_cast<unsigned>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS));

    WiFiClientSecure client;
    // Public weather API, no cert pinning worth the flash/RAM cost on an ESP32 for this class of
    // read-only, non-sensitive data — the same trade-off common ESP32 weather integrations make.
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) {
        ::irrigator::drivers::Log.error("Open-Meteo: could not begin HTTPS request");
        return result;
    }

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] Open-Meteo: HTTP GET failed, code %d\n", httpCode);
        http.end();
        return result;
    }

    JsonDocument filter;
    filter["hourly"]["temperature_2m"] = true;
    filter["hourly"]["precipitation_probability"] = true;
    filter["hourly"]["precipitation"] = true;
    filter["daily"]["precipitation_sum"] = true;

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
        ::irrigator::drivers::Log.printf("[WARN] Open-Meteo: JSON parse failed: %s\n", error.c_str());
        return result;
    }

    JsonArrayConst tempArr = doc["hourly"]["temperature_2m"].as<JsonArrayConst>();
    JsonArrayConst probArr = doc["hourly"]["precipitation_probability"].as<JsonArrayConst>();
    JsonArrayConst precipArr = doc["hourly"]["precipitation"].as<JsonArrayConst>();
    JsonArrayConst dailyArr = doc["daily"]["precipitation_sum"].as<JsonArrayConst>();

    // timezone=UTC above makes the hourly arrays start at 00:00 UTC of (today - past_days), one
    // entry per hour — see domain::openMeteoNowIndex for the arithmetic this relies on.
    const size_t nowIndex =
        ::irrigator::domain::openMeteoNowIndex(nowEpochUtc, board::AUTOADJUSTMENT_PAST_WEATHER_DAYS);

    if (!::irrigator::domain::summarizePastAverageTemperature(
            tempArr, nowIndex, board::AUTOADJUSTMENT_PAST_WEATHER_DAYS, result.avgPastTempC)) {
        ::irrigator::drivers::Log.warn("Open-Meteo: not enough historical temperature data");
        return result;
    }
    if (!::irrigator::domain::summarizePastRainfall(
            dailyArr, board::AUTOADJUSTMENT_PAST_WEATHER_DAYS, result.rainPastDaysMm)) {
        ::irrigator::drivers::Log.warn("Open-Meteo: daily rainfall array shorter than requested");
        return result;
    }
    if (!::irrigator::domain::summarizeForecastRainWindow(probArr,
                                                          precipArr,
                                                          nowIndex,
                                                          board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS,
                                                          result.maxRainProbabilityPercent,
                                                          result.rainAmountNextWindowMm)) {
        ::irrigator::drivers::Log.warn("Open-Meteo: hourly rain arrays too short for the forecast window");
        return result;
    }

    result.success = true;
    return result;
}

namespace {

bool fetchWeatherApiJson(const char* url, JsonDocument& filter, JsonDocument& doc) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) {
        return false;
    }
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] WeatherAPI: HTTP GET failed, code %d\n", httpCode);
        http.end();
        return false;
    }
    const DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
        ::irrigator::drivers::Log.printf("[WARN] WeatherAPI: JSON parse failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

}  // namespace

WeatherFetchResult WeatherService::fetchForecastWeatherApi(float latitude,
                                                           float longitude,
                                                           int64_t nowEpochUtc,
                                                           const char* apiKey) {
    WeatherFetchResult result;
    if (apiKey == nullptr || apiKey[0] == '\0') {
        ::irrigator::drivers::Log.warn("WeatherAPI fallback: no API key configured");
        return result;
    }

    char forecastUrl[224];
    snprintf(forecastUrl,
            sizeof(forecastUrl),
            "%s?key=%s&q=%.4f,%.4f&days=2&aqi=no&alerts=no",
            board::WEATHERAPI_FORECAST_URL,
            apiKey,
            latitude,
            longitude);

    // Forecast call: only used for the next-12h rain-skip window now — temperature and rainfall
    // for the adjustment formula both come from history.json below instead (matching Open-Meteo's
    // historical-vs-forecast split).
    JsonDocument forecastFilter;
    forecastFilter["forecast"]["forecastday"][0]["hour"][0]["time_epoch"] = true;
    forecastFilter["forecast"]["forecastday"][0]["hour"][0]["chance_of_rain"] = true;
    forecastFilter["forecast"]["forecastday"][0]["hour"][0]["precip_mm"] = true;

    JsonDocument forecastDoc;
    if (!fetchWeatherApiJson(forecastUrl, forecastFilter, forecastDoc)) {
        ::irrigator::drivers::Log.warn("WeatherAPI fallback: forecast request failed");
        return result;
    }

    JsonArrayConst days = forecastDoc["forecast"]["forecastday"].as<JsonArrayConst>();
    const bool windowOk =
        ::irrigator::domain::summarizeWeatherApiForecastRainWindow(days,
                                                                   nowEpochUtc,
                                                                   board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS,
                                                                   result.maxRainProbabilityPercent,
                                                                   result.rainAmountNextWindowMm);
    if (!windowOk) {
        ::irrigator::drivers::Log.warn("WeatherAPI fallback: not enough forecast hours returned");
        return result;
    }

    // History calls: one per past day (this endpoint only ever returns a single day), accumulating
    // both precipitation and average temperature — the two historical inputs the correction
    // formula (Step 2) needs.
    float pastRainSum = 0.0f;
    float pastTempSum = 0.0f;
    for (uint8_t daysAgo = 1; daysAgo <= board::AUTOADJUSTMENT_PAST_WEATHER_DAYS; ++daysAgo) {
        char iso[24];
        ::irrigator::domain::formatIso8601Utc(nowEpochUtc - static_cast<int64_t>(daysAgo) * 86400,
                                              iso,
                                              sizeof(iso));
        char dateStr[11];
        snprintf(dateStr, sizeof(dateStr), "%.10s", iso);

        char historyUrl[224];
        snprintf(historyUrl,
                sizeof(historyUrl),
                "%s?key=%s&q=%.4f,%.4f&dt=%s",
                board::WEATHERAPI_HISTORY_URL,
                apiKey,
                latitude,
                longitude,
                dateStr);

        JsonDocument historyFilter;
        historyFilter["forecast"]["forecastday"][0]["day"]["totalprecip_mm"] = true;
        historyFilter["forecast"]["forecastday"][0]["day"]["avgtemp_c"] = true;

        JsonDocument historyDoc;
        if (!fetchWeatherApiJson(historyUrl, historyFilter, historyDoc)) {
            ::irrigator::drivers::Log.printf(
                "[WARN] WeatherAPI fallback: history request failed for %s\n", dateStr);
            return result;
        }
        pastRainSum += historyDoc["forecast"]["forecastday"][0]["day"]["totalprecip_mm"] | 0.0f;
        pastTempSum += historyDoc["forecast"]["forecastday"][0]["day"]["avgtemp_c"] | 0.0f;
    }

    result.rainPastDaysMm = pastRainSum;
    result.avgPastTempC = pastTempSum / static_cast<float>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS);
    result.success = true;
    return result;
}

}  // namespace irrigator::network
