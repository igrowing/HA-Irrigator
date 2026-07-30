#include "Settings.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace irrigator::domain {

namespace {

// True if `value` is present and, for a string, non-empty — the "there's something here worth
// considering" test every merged field starts with. A JSON null and an absent key both read as
// isNull() via ArduinoJson.
bool hasValue(JsonVariantConst value) {
    if (value.isNull()) {
        return false;
    }
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        return text != nullptr && text[0] != '\0';
    }
    return true;
}

// Appends `field` to the caller's comma-separated rejected-fields list, truncating silently if
// it doesn't fit — this is a best-effort alert payload, not something worth failing the merge over.
void appendRejected(char* rejectedFieldsOut, size_t rejectedFieldsOutSize, const char* field) {
    if (rejectedFieldsOut == nullptr || rejectedFieldsOutSize == 0) {
        return;
    }
    size_t used = strlen(rejectedFieldsOut);
    if (used > 0 && used + 1 < rejectedFieldsOutSize) {
        rejectedFieldsOut[used++] = ',';
        rejectedFieldsOut[used] = '\0';
    }
    if (used < rejectedFieldsOutSize) {
        snprintf(rejectedFieldsOut + used, rejectedFieldsOutSize - used, "%s", field);
    }
}

}  // namespace

Settings makeDefaultZones() {
    Settings settings;
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        snprintf(settings.zoneNames[i], sizeof(settings.zoneNames[i]), "Zone %u", static_cast<unsigned>(i + 1));
        settings.zoneEnabled[i] = true;
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
    if (settings.latitude < -90.0f || settings.latitude > 90.0f) {
        return false;
    }
    if (settings.longitude < -180.0f || settings.longitude > 180.0f) {
        return false;
    }
    if (settings.lowerLimitPercent < 0 || settings.upperLimitPercent < settings.lowerLimitPercent ||
        settings.upperLimitPercent > 500) {
        return false;
    }
    if (settings.rainSkipProbabilityPercent < 0 || settings.rainSkipProbabilityPercent > 100) {
        return false;
    }
    if (settings.rainSkipAmountMm < 0) {
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

    parsed.autoAdjustmentEnabled = doc["auto_adjustment_enabled"] | true;
    parsed.locationKnown = doc["location_known"] | false;
    parsed.latitude = doc["latitude"] | 0.0f;
    parsed.longitude = doc["longitude"] | 0.0f;
    snprintf(parsed.city, sizeof(parsed.city), "%s", doc["city"] | "");
    parsed.upperLimitPercent = doc["upper_limit"] | board::AUTOADJUSTMENT_DEFAULT_UPPER_LIMIT_PERCENT;
    parsed.lowerLimitPercent = doc["lower_limit"] | board::AUTOADJUSTMENT_DEFAULT_LOWER_LIMIT_PERCENT;
    parsed.rainSkipProbabilityPercent = doc["rain_skip_probability_percent"] |
                                        board::AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_PROBABILITY_PERCENT;
    parsed.rainSkipAmountMm =
        doc["rain_skip_amount_mm"] | board::AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_AMOUNT_MM;
    snprintf(parsed.weatherApiKey, sizeof(parsed.weatherApiKey), "%s", doc["weatherapi_key"] | "");

    JsonVariantConst zoneNames = doc["zone_names"];
    if (!zoneNames.isNull()) {
        for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
            char key[4];
            snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
            JsonVariantConst entry = zoneNames[key];

            // Accepts either the legacy plain-string form ("1": "Trees left" — enabled defaults
            // true) or the current {"name":...,"enabled":...} object form, so settings.json
            // files written before "enabled" existed keep working unchanged.
            const char* name = "";
            if (entry.is<JsonObjectConst>()) {
                name = entry["name"] | "";
                parsed.zoneEnabled[i] = entry["enabled"] | true;
            } else {
                name = entry.is<const char*>() ? entry.as<const char*>() : "";
            }
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

bool mergeSettings(const Settings& existing,
                    const char* json,
                    size_t length,
                    Settings& out,
                    char* rejectedFieldsOut,
                    size_t rejectedFieldsOutSize) {
    if (rejectedFieldsOut != nullptr && rejectedFieldsOutSize > 0) {
        rejectedFieldsOut[0] = '\0';
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, length);
    if (error || !doc.is<JsonObjectConst>()) {
        return false;
    }

    out = existing;

    JsonVariantConst wateringPercentage = doc["watering_percentage"];
    if (hasValue(wateringPercentage)) {
        if (wateringPercentage.is<long>() && wateringPercentage.as<long>() >= 0 &&
            wateringPercentage.as<long>() <= 300) {
            out.wateringPercentage = static_cast<int16_t>(wateringPercentage.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "watering_percentage");
        }
    }

    // schedule_active merges like any other bool field here; main.ino's handleMqttUpload treats
    // it as device-owned and overrides it back to `existing`'s value afterward regardless (with
    // its own alert if the upload tried to change it) — see Settings.h's mergeSettings doc.
    JsonVariantConst scheduleActive = doc["schedule_active"];
    if (hasValue(scheduleActive)) {
        if (scheduleActive.is<bool>()) {
            out.scheduleActive = scheduleActive.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "schedule_active");
        }
    }

    JsonVariantConst defaultManualDuration = doc["default_manual_duration"];
    if (hasValue(defaultManualDuration)) {
        if (defaultManualDuration.is<long>() &&
            defaultManualDuration.as<long>() >= board::MANUAL_DURATION_MIN_MINUTES &&
            defaultManualDuration.as<long>() <= board::MANUAL_DURATION_MAX_MINUTES) {
            out.defaultManualDuration = static_cast<uint8_t>(defaultManualDuration.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "default_manual_duration");
        }
    }

    JsonVariantConst valveZeroOffset = doc["valve_current_zero_offset_mv"];
    if (hasValue(valveZeroOffset)) {
        if (valveZeroOffset.is<long>() && valveZeroOffset.as<long>() >= 0 &&
            valveZeroOffset.as<long>() <= 3300) {
            out.valveCurrentZeroOffsetMv = static_cast<uint16_t>(valveZeroOffset.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "valve_current_zero_offset_mv");
        }
    }

    JsonVariantConst valveMvPerMa = doc["valve_current_mv_per_ma"];
    if (hasValue(valveMvPerMa)) {
        if (valveMvPerMa.is<float>() && valveMvPerMa.as<float>() > 0.0f) {
            out.valveCurrentMvPerMa = valveMvPerMa.as<float>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "valve_current_mv_per_ma");
        }
    }

    JsonVariantConst pumpZeroOffset = doc["pump_current_zero_offset_mv"];
    if (hasValue(pumpZeroOffset)) {
        if (pumpZeroOffset.is<long>() && pumpZeroOffset.as<long>() >= 0 &&
            pumpZeroOffset.as<long>() <= 3300) {
            out.pumpCurrentZeroOffsetMv = static_cast<uint16_t>(pumpZeroOffset.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "pump_current_zero_offset_mv");
        }
    }

    JsonVariantConst pumpMvPerMa = doc["pump_current_mv_per_ma"];
    if (hasValue(pumpMvPerMa)) {
        if (pumpMvPerMa.is<float>() && pumpMvPerMa.as<float>() > 0.0f) {
            out.pumpCurrentMvPerMa = pumpMvPerMa.as<float>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "pump_current_mv_per_ma");
        }
    }

    // auto_adjustment_enabled merges like any other bool field here; it's device-owned the same
    // way schedule_active is (see comment above) and gets the same post-merge override treatment
    // in main.ino.
    JsonVariantConst autoAdjustmentEnabled = doc["auto_adjustment_enabled"];
    if (hasValue(autoAdjustmentEnabled)) {
        if (autoAdjustmentEnabled.is<bool>()) {
            out.autoAdjustmentEnabled = autoAdjustmentEnabled.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "auto_adjustment_enabled");
        }
    }

    // location_known/latitude/longitude/city: deliberately not read from `doc` at all -- see this
    // function's doc comment in Settings.h. `out` already carries `existing`'s values for them.

    // upper_limit/lower_limit are cross-validated together (upper >= lower, matching
    // validateSettings()) instead of field-by-field, so a combination that only makes sense
    // together doesn't get half-applied; if the combination doesn't hold, whichever of the two
    // was actually present in this upload gets rejected (and alerted), not silently kept as the
    // old value under a new, now-inconsistent partner.
    JsonVariantConst upperLimit = doc["upper_limit"];
    JsonVariantConst lowerLimit = doc["lower_limit"];
    {
        int16_t newUpper = out.upperLimitPercent;
        int16_t newLower = out.lowerLimitPercent;
        bool upperOk = true;
        bool lowerOk = true;
        if (hasValue(upperLimit)) {
            if (upperLimit.is<long>() && upperLimit.as<long>() <= 500) {
                newUpper = static_cast<int16_t>(upperLimit.as<long>());
            } else {
                upperOk = false;
            }
        }
        if (hasValue(lowerLimit)) {
            if (lowerLimit.is<long>() && lowerLimit.as<long>() >= 0) {
                newLower = static_cast<int16_t>(lowerLimit.as<long>());
            } else {
                lowerOk = false;
            }
        }
        if (!upperOk) {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "upper_limit");
        }
        if (!lowerOk) {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "lower_limit");
        }
        if (upperOk && lowerOk) {
            if (newUpper >= newLower) {
                out.upperLimitPercent = newUpper;
                out.lowerLimitPercent = newLower;
            } else {
                if (hasValue(upperLimit)) {
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "upper_limit");
                }
                if (hasValue(lowerLimit)) {
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "lower_limit");
                }
            }
        }
    }

    JsonVariantConst rainSkipProbability = doc["rain_skip_probability_percent"];
    if (hasValue(rainSkipProbability)) {
        if (rainSkipProbability.is<long>() && rainSkipProbability.as<long>() >= 0 &&
            rainSkipProbability.as<long>() <= 100) {
            out.rainSkipProbabilityPercent = static_cast<int16_t>(rainSkipProbability.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "rain_skip_probability_percent");
        }
    }

    JsonVariantConst rainSkipAmount = doc["rain_skip_amount_mm"];
    if (hasValue(rainSkipAmount)) {
        if (rainSkipAmount.is<long>() && rainSkipAmount.as<long>() >= 0) {
            out.rainSkipAmountMm = static_cast<int16_t>(rainSkipAmount.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "rain_skip_amount_mm");
        }
    }

    JsonVariantConst weatherApiKey = doc["weatherapi_key"];
    if (hasValue(weatherApiKey)) {
        if (weatherApiKey.is<const char*>() &&
            strlen(weatherApiKey.as<const char*>()) < sizeof(out.weatherApiKey)) {
            snprintf(out.weatherApiKey, sizeof(out.weatherApiKey), "%s", weatherApiKey.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "weatherapi_key");
        }
    }

    // zone_names is merged per zone index rather than as a single field, so an upload naming
    // only e.g. zone 3 doesn't wipe the other zones' names/enabled state the way replacing the
    // whole sub-object would.
    JsonVariantConst zoneNames = doc["zone_names"];
    if (!zoneNames.isNull()) {
        for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
            char key[4];
            snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
            JsonVariantConst entry = zoneNames[key];
            if (entry.isNull()) {
                continue;
            }

            char fieldLabel[24];
            if (entry.is<const char*>()) {
                // Legacy plain-string form: just the name, enabled left untouched.
                const char* name = entry.as<const char*>();
                if (name != nullptr && name[0] != '\0') {
                    if (strlen(name) < sizeof(out.zoneNames[i])) {
                        snprintf(out.zoneNames[i], sizeof(out.zoneNames[i]), "%s", name);
                    } else {
                        snprintf(fieldLabel, sizeof(fieldLabel), "zone_names.%u",
                                 static_cast<unsigned>(i + 1));
                        appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                    }
                }
                continue;
            }

            if (!entry.is<JsonObjectConst>()) {
                snprintf(fieldLabel, sizeof(fieldLabel), "zone_names.%u", static_cast<unsigned>(i + 1));
                appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                continue;
            }

            JsonVariantConst name = entry["name"];
            if (hasValue(name)) {
                if (name.is<const char*>() && strlen(name.as<const char*>()) < sizeof(out.zoneNames[i])) {
                    snprintf(out.zoneNames[i], sizeof(out.zoneNames[i]), "%s", name.as<const char*>());
                } else {
                    snprintf(fieldLabel, sizeof(fieldLabel), "zone_names.%u.name",
                             static_cast<unsigned>(i + 1));
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                }
            }

            JsonVariantConst enabled = entry["enabled"];
            if (hasValue(enabled)) {
                if (enabled.is<bool>()) {
                    out.zoneEnabled[i] = enabled.as<bool>();
                } else {
                    snprintf(fieldLabel, sizeof(fieldLabel), "zone_names.%u.enabled",
                             static_cast<unsigned>(i + 1));
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                }
            }
        }
    }

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

    doc["auto_adjustment_enabled"] = settings.autoAdjustmentEnabled;
    doc["location_known"] = settings.locationKnown;
    doc["latitude"] = settings.latitude;
    doc["longitude"] = settings.longitude;
    doc["city"] = settings.city;
    doc["upper_limit"] = settings.upperLimitPercent;
    doc["lower_limit"] = settings.lowerLimitPercent;
    doc["rain_skip_probability_percent"] = settings.rainSkipProbabilityPercent;
    doc["rain_skip_amount_mm"] = settings.rainSkipAmountMm;
    doc["weatherapi_key"] = settings.weatherApiKey;

    JsonObject zoneNames = doc["zone_names"].to<JsonObject>();
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
        JsonObject entry = zoneNames[key].to<JsonObject>();
        entry["name"] = settings.zoneNames[i];
        entry["enabled"] = settings.zoneEnabled[i];
    }

    return serializeJson(doc, buffer, bufferSize);
}

}  // namespace irrigator::domain