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
    http.setConnectTimeout(board::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(board::HTTP_RESPONSE_TIMEOUT_MS);
    if (!http.begin(board::GEO_IP_API_URL)) {
        ::irrigator::drivers::Log.error("Geolocation: could not begin HTTP request");
        snprintf(result.errorReason, sizeof(result.errorReason), "could not begin HTTP request");
        return result;
    }

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] Geolocation: HTTP GET failed, code %d\n", httpCode);
        snprintf(result.errorReason, sizeof(result.errorReason), "HTTP GET failed, code %d", httpCode);
        http.end();
        return result;
    }

    JsonDocument filter;
    filter["status"] = true;
    filter["lat"] = true;
    filter["lon"] = true;
    filter["city"] = true;
    filter["offset"] = true;

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (error) {
        ::irrigator::drivers::Log.printf("[WARN] Geolocation: JSON parse failed: %s\n", error.c_str());
        snprintf(result.errorReason, sizeof(result.errorReason), "JSON parse failed: %s", error.c_str());
        return result;
    }

    const char* status = doc["status"] | "";
    if (strcmp(status, "success") != 0) {
        ::irrigator::drivers::Log.warn("Geolocation: provider did not return status=success");
        snprintf(result.errorReason, sizeof(result.errorReason), "provider did not return status=success");
        return result;
    }

    result.latitude = doc["lat"] | 0.0f;
    result.longitude = doc["lon"] | 0.0f;
    const char* city = doc["city"] | "";
    snprintf(result.city, sizeof(result.city), "%s", city);
    result.utcOffsetSeconds = doc["offset"] | 0;
    result.success = true;
    return result;
}

WeatherFetchResult WeatherService::fetchOpenMeteoForecastWindow(float latitude,
                                                                float longitude,
                                                                int64_t nowEpochUtc) {
    WeatherFetchResult result;

    // Hourly only: this call exists solely for the forward rain-skip window. The formula's
    // historical inputs come from the ERA5 archive (fetchOpenMeteoArchivePast), not from this
    // endpoint's past_days daily series. past_days stays 5 purely because the hourly-array "now"
    // index arithmetic (domain::openMeteoNowIndex) is built around it.
    char url[256];
    snprintf(url,
            sizeof(url),
            "%s?latitude=%.4f&longitude=%.4f&hourly=precipitation_probability,precipitation"
            "&past_days=%u&forecast_days=2&timezone=UTC",
            board::OPEN_METEO_BASE_URL,
            latitude,
            longitude,
            static_cast<unsigned>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS));

    WiFiClientSecure client;
    // Public weather API, no cert pinning worth the flash/RAM cost on an ESP32 for this class of
    // read-only, non-sensitive data — the same trade-off common ESP32 weather integrations make.
    client.setInsecure();
    // Bounds the mbedTLS handshake, which HTTPClient's own connect timeout does NOT cover - a
    // stalled handshake here is what blocked this task for 70 seconds. See TLS_HANDSHAKE_TIMEOUT_S.
    client.setHandshakeTimeout(board::TLS_HANDSHAKE_TIMEOUT_S);
    HTTPClient http;
    http.setConnectTimeout(board::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(board::HTTP_RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        ::irrigator::drivers::Log.error("Open-Meteo: could not begin HTTPS request");
        snprintf(result.errorReason, sizeof(result.errorReason), "could not begin HTTPS request");
        return result;
    }

    // Logged so a "parsed OK but arrays came back empty" failure (below) is distinguishable from a
    // heap-pressure cause: the TLS handshake itself is the single biggest heap consumer in this
    // whole call (mbedTLS session state), and arduino-esp32's own ssl_client.cpp tracks the same
    // free-heap-before/after pair internally (at a log level this build doesn't enable).
    const uint32_t freeHeapBeforeGet = ESP.getFreeHeap();
    // getElapsedMs covers DNS + TCP connect + TLS handshake + request + status/headers — a
    // healthy call to a real internet host takes at minimum a few hundred ms for the handshake
    // alone. A near-zero value here (rather than a slow/truncated transfer) would mean http.GET()
    // is failing/returning almost instantly despite reporting HTTP_CODE_OK, which points at
    // something short-circuiting well before real network I/O rather than a resource constraint.
    const uint32_t getStartMs = millis();
    const int httpCode = http.GET();
    const uint32_t getElapsedMs = millis() - getStartMs;
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] Open-Meteo: HTTP GET failed, code %d (took %u ms)\n", httpCode,
                                         getElapsedMs);
        snprintf(result.errorReason, sizeof(result.errorReason), "HTTP GET failed, code %d (took %u ms)", httpCode,
                getElapsedMs);
        http.end();
        return result;
    }
    // Content-Length as the server declared it, vs. what actually gets parsed below: a mismatch
    // (server promised N bytes, deserializeJson got EmptyInput or a short array) points at the
    // connection dying mid-transfer; a declared length of 0 would instead mean the server itself
    // sent nothing, which is a server/query problem, not a device-side resource one.
    const int contentLength = http.getSize();

    JsonDocument filter;
    filter["hourly"]["precipitation_probability"] = true;
    filter["hourly"]["precipitation"] = true;

    // getString() (not getStream()) so HTTPClient de-chunks the body: these providers reply with
    // Transfer-Encoding: chunked (no Content-Length, hence getSize()==-1), and getStream() hands
    // back the raw socket with chunk-size framing still in it — feeding that to deserializeJson
    // parses the leading hex chunk-size as the root value and yields empty arrays / EmptyInput.
    // getString() decodes chunked correctly; the buffered body is well under 20 KB here, ample
    // headroom against free heap.
    const String payload = http.getString();
    const uint32_t totalElapsedMs = millis() - getStartMs;
    http.end();

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (error) {
        ::irrigator::drivers::Log.printf(
            "[WARN] Open-Meteo: JSON parse failed: %s (http=%d, content-length=%d, free heap %u, took %u ms)\n",
            error.c_str(), httpCode, contentLength, freeHeapBeforeGet, totalElapsedMs);
        snprintf(result.errorReason, sizeof(result.errorReason),
                "JSON parse failed: %s (http=%d, content-length=%d, free heap %u, took %u ms)",
                error.c_str(), httpCode, contentLength, freeHeapBeforeGet, totalElapsedMs);
        return result;
    }

    JsonArrayConst probArr = doc["hourly"]["precipitation_probability"].as<JsonArrayConst>();
    JsonArrayConst precipArr = doc["hourly"]["precipitation"].as<JsonArrayConst>();

    // timezone=UTC above makes the hourly arrays (one entry per hour) start at 00:00 UTC of
    // (today - past_days), so nowIndex indexes straight into them for the forward-looking rain
    // window below.
    const size_t nowIndex =
        ::irrigator::domain::openMeteoNowIndex(nowEpochUtc, board::AUTOADJUSTMENT_PAST_WEATHER_DAYS);

    // overflowed(): ArduinoJson's own "ran out of memory while parsing/filtering" flag — set
    // without necessarily raising a hard DeserializationError, so a truncated-by-RAM-pressure
    // response can otherwise look identical to a genuinely short one. Checked here (not up at the
    // `error` branch above) because overflow can happen after the last hard error check succeeds.
    const bool docOverflowed = doc.overflowed();
    // A healthy response always yields (past_days+2)*24 hourly entries, so a short array here
    // almost always means a truncated transfer (weak Wi-Fi), not a bad query. httpCode is
    // HTTP_CODE_OK by this point; totalElapsedMs is still reported — a near-zero value across a
    // GET *and* a full body parse would mean the whole exchange short-circuited, which no genuine
    // round trip can do.
    if (!::irrigator::domain::summarizeForecastRainWindow(probArr,
                                                          precipArr,
                                                          nowIndex,
                                                          board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS,
                                                          result.maxRainProbabilityPercent,
                                                          result.rainAmountNextWindowMm)) {
        ::irrigator::drivers::Log.printf(
            "[WARN] Open-Meteo: hourly rain arrays too short for the forecast window (prob got %u, precip got %u, "
            "nowIndex=%u, need >= %u, free heap %u, content-length %d, overflowed %d, took %u ms)\n",
            static_cast<unsigned>(probArr.size()), static_cast<unsigned>(precipArr.size()),
            static_cast<unsigned>(nowIndex),
            static_cast<unsigned>(nowIndex + board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS), freeHeapBeforeGet,
            contentLength, static_cast<int>(docOverflowed), totalElapsedMs);
        snprintf(result.errorReason, sizeof(result.errorReason),
                "hourly rain arrays too short (prob=%u, precip=%u, nowIndex=%u, free heap %u, overflowed %d, "
                "took %u ms)",
                static_cast<unsigned>(probArr.size()), static_cast<unsigned>(precipArr.size()),
                static_cast<unsigned>(nowIndex), freeHeapBeforeGet, static_cast<int>(docOverflowed), totalElapsedMs);
        return result;
    }

    result.success = true;
    return result;
}

// Historical half of Step 2: average daily max temperature and total rainfall over the trailing
// AUTOADJUSTMENT_PAST_WEATHER_DAYS complete UTC days (yesterday back, today excluded), from the
// ERA5 reanalysis archive. Returns false with result.errorReason set on any failure; on success
// fills result.avgPastTempC / result.rainPastDaysMm and leaves the forecast fields alone.
bool WeatherService::fetchOpenMeteoArchivePast(float latitude,
                                               float longitude,
                                               int64_t nowEpochUtc,
                                               WeatherFetchResult& result) {
    // Date-bounded (start=today-N, end=today-1) rather than past_days: the archive endpoint has no
    // past_days parameter, and asking for exactly N days makes the array indices trivial - the
    // first N entries are the whole window, matching what the summarize helpers already expect.
    // end_date beyond today is rejected outright by the API, so yesterday is also the newest day
    // it will ever accept without an error.
    char startIso[24];
    char endIso[24];
    ::irrigator::domain::formatIso8601Utc(
        nowEpochUtc - static_cast<int64_t>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS) * 86400, startIso, sizeof(startIso));
    ::irrigator::domain::formatIso8601Utc(nowEpochUtc - 86400, endIso, sizeof(endIso));

    char url[256];
    snprintf(url,
            sizeof(url),
            "%s?latitude=%.4f&longitude=%.4f&start_date=%.10s&end_date=%.10s"
            "&daily=precipitation_sum,temperature_2m_max&timezone=UTC",
            board::OPEN_METEO_ARCHIVE_URL,
            latitude,
            longitude,
            startIso,
            endIso);

    WiFiClientSecure client;
    client.setInsecure();  // Same trade-off as the forecast fetch above.
    client.setHandshakeTimeout(board::TLS_HANDSHAKE_TIMEOUT_S);
    HTTPClient http;
    http.setConnectTimeout(board::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(board::HTTP_RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        ::irrigator::drivers::Log.error("Open-Meteo archive: could not begin HTTPS request");
        snprintf(result.errorReason, sizeof(result.errorReason), "archive: could not begin HTTPS request");
        return false;
    }

    const uint32_t freeHeapBeforeGet = ESP.getFreeHeap();
    const uint32_t getStartMs = millis();
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] Open-Meteo archive: HTTP GET failed, code %d (took %u ms)\n",
                                         httpCode, millis() - getStartMs);
        snprintf(result.errorReason, sizeof(result.errorReason), "archive: HTTP GET failed, code %d (took %u ms)",
                httpCode, millis() - getStartMs);
        http.end();
        return false;
    }

    JsonDocument filter;
    filter["daily"]["precipitation_sum"] = true;
    filter["daily"]["temperature_2m_max"] = true;

    // getString() so a chunked reply is de-chunked - same reason as fetchOpenMeteoForecastWindow.
    const String payload = http.getString();
    const uint32_t totalElapsedMs = millis() - getStartMs;
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (error) {
        ::irrigator::drivers::Log.printf(
            "[WARN] Open-Meteo archive: JSON parse failed: %s (http=%d, free heap %u, took %u ms)\n",
            error.c_str(), httpCode, freeHeapBeforeGet, totalElapsedMs);
        snprintf(result.errorReason, sizeof(result.errorReason),
                "archive: JSON parse failed: %s (http=%d, free heap %u, took %u ms)", error.c_str(), httpCode,
                freeHeapBeforeGet, totalElapsedMs);
        return false;
    }

    JsonArrayConst dailyRain = doc["daily"]["precipitation_sum"].as<JsonArrayConst>();
    JsonArrayConst dailyMaxTemp = doc["daily"]["temperature_2m_max"].as<JsonArrayConst>();
    float avgPastTempC = 0.0f;
    float rainPastDaysMm = 0.0f;
    // Both helpers reject a short array *and* any null inside the window - the archive reports a
    // day it doesn't have yet as null, which must fail the fetch rather than read as 0mm / 0°C.
    if (!::irrigator::domain::summarizePastAverageDailyMaxTemperature(
            dailyMaxTemp, board::AUTOADJUSTMENT_PAST_WEATHER_DAYS, avgPastTempC) ||
        !::irrigator::domain::summarizePastRainfall(dailyRain, board::AUTOADJUSTMENT_PAST_WEATHER_DAYS, rainPastDaysMm)) {
        ::irrigator::drivers::Log.printf(
            "[WARN] Open-Meteo archive: incomplete window %.10s..%.10s (temp days %u, rain days %u, need %u, "
            "overflowed %d, free heap %u, took %u ms)\n",
            startIso, endIso, static_cast<unsigned>(dailyMaxTemp.size()), static_cast<unsigned>(dailyRain.size()),
            static_cast<unsigned>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS), static_cast<int>(doc.overflowed()),
            freeHeapBeforeGet, totalElapsedMs);
        snprintf(result.errorReason, sizeof(result.errorReason),
                "archive: incomplete window %.10s..%.10s (temp days %u, rain days %u, need %u, overflowed %d, "
                "free heap %u, took %u ms)",
                startIso, endIso, static_cast<unsigned>(dailyMaxTemp.size()), static_cast<unsigned>(dailyRain.size()),
                static_cast<unsigned>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS), static_cast<int>(doc.overflowed()),
                freeHeapBeforeGet, totalElapsedMs);
        return false;
    }
    result.avgPastTempC = avgPastTempC;
    result.rainPastDaysMm = rainPastDaysMm;
    return true;
}

WeatherFetchResult WeatherService::fetchForecastOpenMeteo(float latitude,
                                                          float longitude,
                                                          int64_t nowEpochUtc) {
    // Forecast first: it carries the rain-skip decision, and there's no point spending a second
    // TLS handshake on history when it already failed. The two fetches are separate calls so each
    // one's WiFiClientSecure (~40-60KB of mbedTLS state) is fully released before the next starts.
    WeatherFetchResult result = fetchOpenMeteoForecastWindow(latitude, longitude, nowEpochUtc);
    if (!result.success) {
        return result;
    }
    // Any archive failure fails the whole Open-Meteo attempt (-> WeatherAPI fallback), on purpose:
    // quietly substituting the forecast endpoint's own past_days series would be ~25 percentage
    // points different from the archive the constants are calibrated on (see board_config.h).
    result.success = false;
    if (!fetchOpenMeteoArchivePast(latitude, longitude, nowEpochUtc, result)) {
        return result;
    }
    result.errorReason[0] = '\0';
    result.success = true;
    return result;
}

namespace {

bool fetchWeatherApiJson(const char* url, JsonDocument& filter, JsonDocument& doc, char* errorReason, size_t errorReasonSize) {
    WiFiClientSecure client;
    client.setInsecure();
    // Bounds the mbedTLS handshake, which HTTPClient's own connect timeout does NOT cover - a
    // stalled handshake here is what blocked this task for 70 seconds. See TLS_HANDSHAKE_TIMEOUT_S.
    client.setHandshakeTimeout(board::TLS_HANDSHAKE_TIMEOUT_S);
    HTTPClient http;
    http.setConnectTimeout(board::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(board::HTTP_RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        snprintf(errorReason, errorReasonSize, "could not begin HTTPS request");
        return false;
    }
    // See the matching comments in fetchForecastOpenMeteo — same TLS-handshake heap-pressure
    // suspect, same reason heap/timing are captured around the GET rather than only on failure.
    const uint32_t freeHeapBeforeGet = ESP.getFreeHeap();
    const uint32_t getStartMs = millis();
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ::irrigator::drivers::Log.printf("[WARN] WeatherAPI: HTTP GET failed, code %d (took %u ms)\n", httpCode,
                                         millis() - getStartMs);
        snprintf(errorReason, errorReasonSize, "HTTP GET failed, code %d (took %u ms)", httpCode,
                millis() - getStartMs);
        http.end();
        return false;
    }
    const int contentLength = http.getSize();
    // getString() (not getStream()) so HTTPClient de-chunks the body — see the matching comment in
    // fetchForecastOpenMeteo. WeatherAPI replies chunked (getSize()==-1), and the raw stream's chunk
    // framing was surfacing as EmptyInput here.
    const String payload = http.getString();
    const uint32_t totalElapsedMs = millis() - getStartMs;
    http.end();
    const DeserializationError error =
        deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (error) {
        ::irrigator::drivers::Log.printf(
            "[WARN] WeatherAPI: JSON parse failed: %s (http=%d, content-length=%d, free heap %u, took %u ms)\n",
            error.c_str(), httpCode, contentLength, freeHeapBeforeGet, totalElapsedMs);
        snprintf(errorReason, errorReasonSize,
                "JSON parse failed: %s (http=%d, content-length=%d, free heap %u, took %u ms)", error.c_str(),
                httpCode, contentLength, freeHeapBeforeGet, totalElapsedMs);
        return false;
    }
    if (doc.overflowed()) {
        // Didn't raise a hard DeserializationError, but ArduinoJson ran out of memory while
        // filtering/keeping fields — the caller's array-size checks downstream would otherwise
        // report this identically to a genuinely short response.
        ::irrigator::drivers::Log.printf(
            "[WARN] WeatherAPI: document overflowed while parsing (content-length=%d, free heap %u, took %u ms)\n",
            contentLength, freeHeapBeforeGet, totalElapsedMs);
        snprintf(errorReason, errorReasonSize,
                "document overflowed while parsing (content-length=%d, free heap %u, took %u ms)", contentLength,
                freeHeapBeforeGet, totalElapsedMs);
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
        snprintf(result.errorReason, sizeof(result.errorReason), "no API key configured");
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

    // See the matching comment in fetchForecastOpenMeteo above — same rationale, same TLS-handshake
    // heap-pressure suspect.
    const uint32_t freeHeapBeforeForecastGet = ESP.getFreeHeap();
    const uint32_t forecastFetchStartMs = millis();
    JsonDocument forecastDoc;
    if (!fetchWeatherApiJson(forecastUrl, forecastFilter, forecastDoc, result.errorReason, sizeof(result.errorReason))) {
        ::irrigator::drivers::Log.printf("[WARN] WeatherAPI fallback: forecast request failed: %s\n",
                                         result.errorReason);
        return result;
    }
    const uint32_t forecastFetchElapsedMs = millis() - forecastFetchStartMs;

    JsonArrayConst days = forecastDoc["forecast"]["forecastday"].as<JsonArrayConst>();
    size_t qualifyingHours = 0;
    const bool windowOk =
        ::irrigator::domain::summarizeWeatherApiForecastRainWindow(days,
                                                                   nowEpochUtc,
                                                                   board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS,
                                                                   result.maxRainProbabilityPercent,
                                                                   result.rainAmountNextWindowMm,
                                                                   qualifyingHours);
    if (!windowOk) {
        // qualifyingHours==0 across every day/hour usually means time_epoch came back
        // missing/unparseable for the whole response (silently defaults to epoch 0, so every hour
        // looks like "the past" and gets skipped) rather than a genuinely short response — see
        // domain::summarizeWeatherApiForecastRainWindow.
        ::irrigator::drivers::Log.printf(
            "[WARN] WeatherAPI fallback: not enough forecast hours returned (got %u, need >= %u, free heap %u, "
            "took %u ms)\n",
            static_cast<unsigned>(qualifyingHours),
            static_cast<unsigned>(board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS), freeHeapBeforeForecastGet,
            forecastFetchElapsedMs);
        snprintf(result.errorReason, sizeof(result.errorReason),
                "not enough forecast hours returned (got %u, need >= %u, free heap %u, took %u ms)",
                static_cast<unsigned>(qualifyingHours),
                static_cast<unsigned>(board::AUTOADJUSTMENT_FORECAST_WINDOW_HOURS), freeHeapBeforeForecastGet,
                forecastFetchElapsedMs);
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
        // maxtemp_c (not avgtemp_c) to match Open-Meteo's daily-peak temperature term above.
        historyFilter["forecast"]["forecastday"][0]["day"]["maxtemp_c"] = true;

        JsonDocument historyDoc;
        if (!fetchWeatherApiJson(historyUrl, historyFilter, historyDoc, result.errorReason, sizeof(result.errorReason))) {
            ::irrigator::drivers::Log.printf(
                "[WARN] WeatherAPI fallback: history request failed for %s: %s\n", dateStr, result.errorReason);
            // Prefixing "history <date>: " onto an already-enriched reason (heap/content-length
            // detail from fetchWeatherApiJson) needs headroom beyond errorReason's own 200-byte
            // size, or the prefix would silently push the useful part off the end — same
            // truncation this whole buffer-sizing pass is fixing elsewhere.
            char detail[240];
            snprintf(detail, sizeof(detail), "history %s: %s", dateStr, result.errorReason);
            snprintf(result.errorReason, sizeof(result.errorReason), "%s", detail);
            return result;
        }
        pastRainSum += historyDoc["forecast"]["forecastday"][0]["day"]["totalprecip_mm"] | 0.0f;
        pastTempSum += historyDoc["forecast"]["forecastday"][0]["day"]["maxtemp_c"] | 0.0f;
    }

    result.rainPastDaysMm = pastRainSum;
    result.avgPastTempC = pastTempSum / static_cast<float>(board::AUTOADJUSTMENT_PAST_WEATHER_DAYS);
    result.success = true;
    return result;
}

}  // namespace irrigator::network
