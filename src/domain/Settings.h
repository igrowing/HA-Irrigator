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
};

Settings makeDefaultZones();
bool validateSettings(const Settings& settings);

// Missing fields fall back to makeDefaultSettings() values (this is the spec-defined default,
// not a guess). Rejects (returns false) only on malformed JSON or values validateSettings()
// rejects, e.g. an out-of-range watering percentage.
bool parseSettings(const char* json, size_t length, Settings& out);
size_t serializeSettings(const Settings& settings, char* buffer, size_t bufferSize);

}  // namespace irrigator::domain