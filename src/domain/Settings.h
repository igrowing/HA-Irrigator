#pragma once

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

namespace irrigator::domain {

struct Settings {
    int16_t wateringPercentage = board::DEFAULT_WATERING_PERCENTAGE;
    char zoneNames[board::ZONE_COUNT][16] = {};
    // Disabled zones are excluded from all watering activity — scheduled, manual (local UI), and
    // MQTT cmd/run alike; see enqueueOrStart() in main.ino, the single choke point all three
    // funnel through.
    bool zoneEnabled[board::ZONE_COUNT] = {};
    bool scheduleActive = true;
    uint8_t defaultManualDuration = board::DEFAULT_MANUAL_DURATION_MINUTES;

    // Current-sensor calibration (see drivers::CurrentSensor). Board-config constants are only
    // the pre-calibration fallback used until a real calibration result is saved here (e.g. via
    // MQTT cmd/set_calibration).
    uint16_t valveCurrentZeroOffsetMv = board::VALVE_CURRENT_SENSOR_ZERO_OFFSET_MV;
    float valveCurrentMvPerMa = board::VALVE_CURRENT_SENSOR_MV_PER_MA;
    uint16_t pumpCurrentZeroOffsetMv = board::PUMP_CURRENT_SENSOR_ZERO_OFFSET_MV;
    float pumpCurrentMvPerMa = board::PUMP_CURRENT_SENSOR_MV_PER_MA;

    // Weather-based auto-adjustment. locationKnown is an explicit flag rather
    // than inferring "unset" from lat/lon both being 0 — (0,0) is a real place (Gulf of Guinea),
    // so a sentinel value would be ambiguous with a genuine reading there.
    bool locationKnown = false;
    float latitude = 0.0f;
    float longitude = 0.0f;
    char city[32] = {};
    // Deliberately NOT here: a "last successful auto-adjustment date" field. It's in-RAM only
    // (see main.ino's lastWeatherUpdateDate) so a power cycle naturally re-triggers today's cycle
    // instead of remembering it was already done — the auto-adjustment window (3h before the
    // first scheduled watering) is designed to tolerate re-running, not to be skipped after a
    // reboot.
    int16_t upperLimitPercent = board::AUTOADJUSTMENT_DEFAULT_UPPER_LIMIT_PERCENT;
    int16_t lowerLimitPercent = board::AUTOADJUSTMENT_DEFAULT_LOWER_LIMIT_PERCENT;
    float tempModifierPctPerDegree = board::AUTOADJUSTMENT_DEFAULT_TEMP_MODIFIER_PCT_PER_DEGREE;
    float rainModifierPctPerMm = board::AUTOADJUSTMENT_DEFAULT_RAIN_MODIFIER_PCT_PER_MM;
    // Step 1's rain-skip trigger thresholds (see domain::shouldSkipForRain) — whole percent/mm,
    // matching the rotary UI's existing integer-only editing convention (no field in this project
    // is rotary-edited as a float today).
    int16_t rainSkipProbabilityPercent = board::AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_PROBABILITY_PERCENT;
    int16_t rainSkipAmountMm = board::AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_AMOUNT_MM;
    // WeatherAPI.com key, used only as the Phase 4 fallback when Open-Meteo (no key required)
    // fails. Note: sensor.irrigator_settings in the HA package mirrors the whole settings.json
    // payload as attributes (see homeassistant/packages/irrigator.yaml), so this key is visible
    // there too — not selectively excludable without reworking that sensor.
    char weatherApiKey[48] = {};
};

Settings makeDefaultZones();
bool validateSettings(const Settings& settings);

// Missing fields fall back to makeDefaultSettings() values (this is the spec-defined default,
// not a guess). Rejects (returns false) only on malformed JSON or values validateSettings()
// rejects, e.g. an out-of-range watering percentage.
bool parseSettings(const char* json, size_t length, Settings& out);
size_t serializeSettings(const Settings& settings, char* buffer, size_t bufferSize);

}  // namespace irrigator::domain