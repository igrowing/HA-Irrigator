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

    // True-RMS current "base" per sensor (see drivers::CurrentSensor::sampleRmsMv) — a virtual
    // reference level, not a real mA value, captured by MQTT cmd/calibrate while a zone/pump is
    // under normal running load. 0 means "never calibrated": main.ino's over/undercurrent checks
    // skip a sensor entirely until this is set. Device-owned, same as
    // location_known/latitude/longitude/city below — only cmd/calibrate ever writes these, never
    // an uploaded settings.json (see mergeSettings's doc below).
    uint16_t valveCurrentBaseLevel = 0;
    uint16_t pumpCurrentBaseLevel = 0;
    // Per-fault enable toggles for drainCurrentBurstResults()'s current-fault alerting (main.ino) —
    // independent of, and in addition to, that check's own base-calibrated (!=0) gate. All default
    // true so upgrading an existing settings.json (missing these fields) preserves today's
    // unconditional-when-calibrated alerting behavior. Editable from the on-device Settings menu
    // (above "Calibrate CS") and from HA; lives in settings.json's Current_sensor group.
    bool alertPumpUc = true;
    bool alertPumpOc = true;
    bool alertValveUc = true;
    bool alertValveOc = true;

    // Weather-based auto-adjustment. Master on/off switch for the whole feature (both the
    // percentage recalculation and the rain-skip check) — defaults true so upgrading an existing
    // settings.json (missing this field) preserves today's behavior instead of silently disabling
    // a feature that was already running. Geolocation resolution (tryGeolocateIfDue in main.ino)
    // is intentionally NOT gated by this: it's cheap, one-time, and its result (city/coordinates)
    // is still worth displaying even with adjustment disabled.
    bool autoAdjustmentEnabled = true;
    // locationKnown is an explicit flag rather
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
    // The correction formula's three coefficients (domain::computeAutoCorrectedPercentage).
    // Compile-time constants between af3497b and this change, which made every calibration
    // experiment cost a firmware flash - the reason a known-wrong base temperature survived two
    // months of tuning. Floats, so unlike the rain-skip thresholds below they are settings.json/HA
    // -editable only, not rotary-editable: the on-device UI has no float editor and adding one for
    // fields that are set once per site isn't worth the flash.
    float baseTempC = board::AUTOADJUSTMENT_BASE_TEMP_C;
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

// settings.json is nested into four top-level groups — Zones (per-zone name/enabled, keyed "1".."N",
// same shape as before), Location (location_known/latitude/longitude/city), Current_sensor
// (valve/pump_current_base_level, alertPumpUc/Oc/ValveUc/Oc), Watering (everything else: watering
// percentage, schedule_active, default_manual_duration, auto_adjustment_enabled, upper/lower limit,
// rain_skip_probability_percent/rain_skip_amount_mm, weatherapi_key). parseSettings() additionally
// falls back to each field's old flat top-level key when its new nested key is absent, so a
// pre-restructuring settings.json still loads its real values instead of resetting to defaults —
// see main.ino's boot-time load, which persists once right after a successful parse to rewrite the
// file in the new nested shape (self-migrating; the flat fallback is then never exercised again on
// that device). mergeSettings() (below) does NOT have this fallback — by the time an upload can
// reach it, HA is publishing nested settings.json too.
//
// Missing fields fall back to makeDefaultSettings() values (this is the spec-defined default,
// not a guess). Rejects (returns false) only on malformed JSON or values validateSettings()
// rejects, e.g. an out-of-range watering percentage.
bool parseSettings(const char* json, size_t length, Settings& out);
size_t serializeSettings(const Settings& settings, char* buffer, size_t bufferSize);

// Same field-by-field merge semantics as ConfigStore.h's mergeConfig(): a field present in
// `json` (at its nested group path, e.g. "Watering.watering_percentage") with a non-null,
// non-empty value is validated on its own and applied to `out` only if it individually passes; a
// field that's missing, null, an empty string, or fails its own check instead keeps `existing`'s
// value, and (only for the fails-its-own-check case) its dotted path is appended,
// comma-separated, into `rejectedFieldsOut`. Zones is merged per zone index, so an upload naming
// only zone 3 doesn't disturb zones 1, 2, 4... Watering.upper_limit/lower_limit are validated
// together (upper >= lower), matching validateSettings().
//
// Location.location_known/latitude/longitude/city, and Current_sensor.valve/pump_current_base_level,
// are device-owned (the former populated only by the device's own geolocation resolution — see
// main.ino's resolveGeolocationIfNeeded() — the latter only by cmd/calibrate) and are deliberately
// never read from `json` here; `out` always keeps `existing`'s values for those six. Callers
// that also guard schedule_active/auto_adjustment_enabled as device-owned (see main.ino's
// handleMqttUpload) still need to do that themselves after calling this — those two are still
// merged like any other field here.
//
// Returns false only when `json` doesn't parse as a JSON object at all — there's nothing to
// merge from then, and `out`/`rejectedFieldsOut` are left untouched.
bool mergeSettings(const Settings& existing,
                    const char* json,
                    size_t length,
                    Settings& out,
                    char* rejectedFieldsOut,
                    size_t rejectedFieldsOutSize);

}  // namespace irrigator::domain