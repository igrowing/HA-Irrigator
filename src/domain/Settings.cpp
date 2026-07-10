#include "Settings.h"

#include <ArduinoJson.h>
#include <stdio.h>

namespace irrigator::domain {

Settings makeDefaultSettings() {
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
    return true;
}

bool parseSettings(const char* json, size_t length, Settings& out) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, length);
    if (error) {
        return false;
    }

    Settings parsed = makeDefaultSettings();
    parsed.wateringPercentage = doc["watering_percentage"] | board::DEFAULT_WATERING_PERCENTAGE;
    parsed.scheduleActive = doc["schedule_active"] | true;
    parsed.defaultManualDuration =
        doc["default_manual_duration"] | board::DEFAULT_MANUAL_DURATION_MINUTES;

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

    JsonObject zoneNames = doc["zone_names"].to<JsonObject>();
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
        zoneNames[key] = settings.zoneNames[i];
    }

    return serializeJson(doc, buffer, bufferSize);
}

}  // namespace irrigator::domain