#include "Settings.h"

#include <ArduinoJson.h>
#include <stdio.h>

namespace irrigator::domain {

Settings makeDefaultZones() {
    Settings settings;
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        snprintf(settings.zoneNames[i], sizeof(settings.zoneNames[i]), "Zone %u", static_cast<unsigned>(i + 1));
    }
    return settings;
}

bool validateSettings(const Settings& settings) {
    if (settings.wateringPercentage < 0 || settings.wateringPercentage > 300) {
        return false;
    }
    if (settings.defaultManualDuration < board::MANUAL_DURATION_MIN_MINUTES ||
        settings.defaultManualDuration > board::MANUAL_DURATION_MAX_MINUTES) {
        return false;
    }
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        if (settings.zoneNames[i][0] == '\0') {
            return false;
        }
    }
    // 3300mV covers the ESP32 ADC's full-scale range at 11dB attenuation; mv-per-mA must be
    // positive or readMilliAmps()'s division becomes meaningless.
    if (settings.valveCurrentZeroOffsetMv > 3300 || settings.valveCurrentMvPerMa <= 0.0f) {
        return false;
    }
    if (settings.pumpCurrentZeroOffsetMv > 3300 || settings.pumpCurrentMvPerMa <= 0.0f) {
        return false;
    }
    return true;
}

bool parseSettings(const char* json, size_t length, Settings& out) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, length);
    if (error) {
        return false;
    }

    Settings parsed = makeDefaultZones();
    parsed.wateringPercentage = doc["watering_percentage"] | board::DEFAULT_WATERING_PERCENTAGE;
    parsed.scheduleActive = doc["schedule_active"] | false;
    parsed.defaultManualDuration =
        doc["default_manual_duration"] | board::DEFAULT_MANUAL_DURATION_MINUTES;
    parsed.valveCurrentZeroOffsetMv =
        doc["valve_current_zero_offset_mv"] | board::VALVE_CURRENT_SENSOR_ZERO_OFFSET_MV;
    parsed.valveCurrentMvPerMa = doc["valve_current_mv_per_ma"] | board::VALVE_CURRENT_SENSOR_MV_PER_MA;
    parsed.pumpCurrentZeroOffsetMv =
        doc["pump_current_zero_offset_mv"] | board::PUMP_CURRENT_SENSOR_ZERO_OFFSET_MV;
    parsed.pumpCurrentMvPerMa = doc["pump_current_mv_per_ma"] | board::PUMP_CURRENT_SENSOR_MV_PER_MA;

    JsonVariantConst zoneNames = doc["zone_names"];
    if (!zoneNames.isNull()) {
        for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
            char key[4];
            snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
            const char* name = zoneNames[key] | "";
            if (name[0] != '\0') {
                snprintf(parsed.zoneNames[i], sizeof(parsed.zoneNames[i]), "%s", name);
            }
        }
    }

    if (!validateSettings(parsed)) {
        return false;
    }

    out = parsed;
    return true;
}

size_t serializeSettings(const Settings& settings, char* buffer, size_t bufferSize) {
    JsonDocument doc;
    doc["watering_percentage"] = settings.wateringPercentage;
    doc["schedule_active"] = settings.scheduleActive;
    doc["default_manual_duration"] = settings.defaultManualDuration;
    doc["valve_current_zero_offset_mv"] = settings.valveCurrentZeroOffsetMv;
    doc["valve_current_mv_per_ma"] = settings.valveCurrentMvPerMa;
    doc["pump_current_zero_offset_mv"] = settings.pumpCurrentZeroOffsetMv;
    doc["pump_current_mv_per_ma"] = settings.pumpCurrentMvPerMa;

    JsonObject zoneNames = doc["zone_names"].to<JsonObject>();
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
        zoneNames[key] = settings.zoneNames[i];
    }

    return serializeJson(doc, buffer, bufferSize);
}

}  // namespace irrigator::domain