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
    // 3300mV covers the ESP32 ADC's full-scale range at 11dB attenuation — a captured base can't
    // legitimately exceed it.
    if (settings.valveCurrentBaseLevel > 3300 || settings.pumpCurrentBaseLevel > 3300) {
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
    // Bounds are sanity rails, not calibration guidance: a negative modifier would invert the
    // formula's sense (hotter -> less water), and a base temperature outside this range can't be a
    // real trailing average of daily maxima anywhere on Earth.
    if (settings.baseTempC < -50.0f || settings.baseTempC > 60.0f) {
        return false;
    }
    if (settings.tempModifierPctPerDegree < 0.0f || settings.tempModifierPctPerDegree > 100.0f) {
        return false;
    }
    if (settings.rainModifierPctPerMm < 0.0f || settings.rainModifierPctPerMm > 100.0f) {
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

    // Nested-group lookups, each falling back to the pre-restructuring flat top-level key when the
    // group (or the field within it) is absent — see this function's doc comment in Settings.h for
    // why: it lets a device's existing flat settings.json keep its real values across the upgrade
    // instead of silently reverting to factory defaults. main.ino persists once right after a
    // successful boot-time parse, which rewrites the file in the new nested shape and makes this
    // fallback a one-time affair per device.
    JsonVariantConst watering = doc["Watering"];
    JsonVariantConst location = doc["Location"];
    JsonVariantConst currentSensor = doc["Current_sensor"];
    JsonVariantConst zones = doc["Zones"];
    if (zones.isNull()) {
        zones = doc["zone_names"];
    }

    Settings parsed = makeDefaultZones();
    parsed.wateringPercentage = watering["watering_percentage"] |
                                (doc["watering_percentage"] | board::DEFAULT_WATERING_PERCENTAGE);
    parsed.scheduleActive = watering["schedule_active"] | (doc["schedule_active"] | false);
    parsed.defaultManualDuration =
        watering["default_manual_duration"] |
        (doc["default_manual_duration"] | board::DEFAULT_MANUAL_DURATION_MINUTES);
    parsed.valveCurrentBaseLevel =
        currentSensor["valve_current_base_level"] | (doc["valve_current_base_level"] | 0);
    parsed.pumpCurrentBaseLevel =
        currentSensor["pump_current_base_level"] | (doc["pump_current_base_level"] | 0);

    // No flat-key fallback for these four — they're new, there's no pre-restructuring value to
    // fall back to. Default true (see Settings.h's field comment).
    parsed.alertPumpUc = currentSensor["alert_pump_uc"] | true;
    parsed.alertPumpOc = currentSensor["alert_pump_oc"] | true;
    parsed.alertValveUc = currentSensor["alert_valve_uc"] | true;
    parsed.alertValveOc = currentSensor["alert_valve_oc"] | true;

    parsed.autoAdjustmentEnabled =
        watering["auto_adjustment_enabled"] | (doc["auto_adjustment_enabled"] | true);
    parsed.locationKnown = location["location_known"] | (doc["location_known"] | false);
    parsed.latitude = location["latitude"] | (doc["latitude"] | 0.0f);
    parsed.longitude = location["longitude"] | (doc["longitude"] | 0.0f);
    snprintf(parsed.city, sizeof(parsed.city), "%s", location["city"] | (doc["city"] | ""));
    parsed.upperLimitPercent =
        watering["upper_limit"] | (doc["upper_limit"] | board::AUTOADJUSTMENT_DEFAULT_UPPER_LIMIT_PERCENT);
    parsed.lowerLimitPercent =
        watering["lower_limit"] | (doc["lower_limit"] | board::AUTOADJUSTMENT_DEFAULT_LOWER_LIMIT_PERCENT);
    parsed.rainSkipProbabilityPercent =
        watering["rain_skip_probability_percent"] |
        (doc["rain_skip_probability_percent"] | board::AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_PROBABILITY_PERCENT);
    parsed.rainSkipAmountMm = watering["rain_skip_amount_mm"] |
                              (doc["rain_skip_amount_mm"] | board::AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_AMOUNT_MM);
    parsed.baseTempC = watering["base_temp_c"] | (doc["base_temp_c"] | board::AUTOADJUSTMENT_BASE_TEMP_C);
    parsed.tempModifierPctPerDegree =
        watering["temp_modifier_pct_per_degree"] |
        (doc["temp_modifier_pct_per_degree"] | board::AUTOADJUSTMENT_DEFAULT_TEMP_MODIFIER_PCT_PER_DEGREE);
    parsed.rainModifierPctPerMm =
        watering["rain_modifier_pct_per_mm"] |
        (doc["rain_modifier_pct_per_mm"] | board::AUTOADJUSTMENT_DEFAULT_RAIN_MODIFIER_PCT_PER_MM);
    snprintf(parsed.weatherApiKey, sizeof(parsed.weatherApiKey), "%s",
             watering["weatherapi_key"] | (doc["weatherapi_key"] | ""));

    if (!zones.isNull()) {
        for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
            char key[4];
            snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
            JsonVariantConst entry = zones[key];

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

    // Nested-only: by the time an upload reaches this function, the publisher (HA's
    // script.irrigator_publish_settings) is writing the new nested shape too — see this
    // function's doc comment in Settings.h. Absent groups just mean every field within them
    // reads as absent below, which is handled the same as any other missing field (kept as
    // `existing`'s value, no rejection).
    JsonVariantConst watering = doc["Watering"];
    JsonVariantConst currentSensor = doc["Current_sensor"];

    JsonVariantConst wateringPercentage = watering["watering_percentage"];
    if (hasValue(wateringPercentage)) {
        if (wateringPercentage.is<long>() && wateringPercentage.as<long>() >= 0 &&
            wateringPercentage.as<long>() <= 300) {
            out.wateringPercentage = static_cast<int16_t>(wateringPercentage.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.watering_percentage");
        }
    }

    // schedule_active merges like any other bool field here; main.ino's handleMqttUpload treats
    // it as device-owned and overrides it back to `existing`'s value afterward regardless (with
    // its own alert if the upload tried to change it) — see Settings.h's mergeSettings doc.
    JsonVariantConst scheduleActive = watering["schedule_active"];
    if (hasValue(scheduleActive)) {
        if (scheduleActive.is<bool>()) {
            out.scheduleActive = scheduleActive.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.schedule_active");
        }
    }

    JsonVariantConst defaultManualDuration = watering["default_manual_duration"];
    if (hasValue(defaultManualDuration)) {
        if (defaultManualDuration.is<long>() &&
            defaultManualDuration.as<long>() >= board::MANUAL_DURATION_MIN_MINUTES &&
            defaultManualDuration.as<long>() <= board::MANUAL_DURATION_MAX_MINUTES) {
            out.defaultManualDuration = static_cast<uint8_t>(defaultManualDuration.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.default_manual_duration");
        }
    }

    // auto_adjustment_enabled merges like any other bool field here; it's device-owned the same
    // way schedule_active is (see comment above) and gets the same post-merge override treatment
    // in main.ino.
    JsonVariantConst autoAdjustmentEnabled = watering["auto_adjustment_enabled"];
    if (hasValue(autoAdjustmentEnabled)) {
        if (autoAdjustmentEnabled.is<bool>()) {
            out.autoAdjustmentEnabled = autoAdjustmentEnabled.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.auto_adjustment_enabled");
        }
    }

    // Current_sensor.location_known/latitude/longitude/city, and valve/pump_current_base_level:
    // deliberately not read from `doc` at all -- see this function's doc comment in Settings.h.
    // `out` already carries `existing`'s values for them.

    // The four alert toggles are plain bools, merged individually like schedule_active above —
    // they're user-editable (not device-owned), so no post-merge override is needed in main.ino.
    JsonVariantConst alertPumpUc = currentSensor["alert_pump_uc"];
    if (hasValue(alertPumpUc)) {
        if (alertPumpUc.is<bool>()) {
            out.alertPumpUc = alertPumpUc.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Current_sensor.alert_pump_uc");
        }
    }
    JsonVariantConst alertPumpOc = currentSensor["alert_pump_oc"];
    if (hasValue(alertPumpOc)) {
        if (alertPumpOc.is<bool>()) {
            out.alertPumpOc = alertPumpOc.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Current_sensor.alert_pump_oc");
        }
    }
    JsonVariantConst alertValveUc = currentSensor["alert_valve_uc"];
    if (hasValue(alertValveUc)) {
        if (alertValveUc.is<bool>()) {
            out.alertValveUc = alertValveUc.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Current_sensor.alert_valve_uc");
        }
    }
    JsonVariantConst alertValveOc = currentSensor["alert_valve_oc"];
    if (hasValue(alertValveOc)) {
        if (alertValveOc.is<bool>()) {
            out.alertValveOc = alertValveOc.as<bool>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Current_sensor.alert_valve_oc");
        }
    }

    // upper_limit/lower_limit are cross-validated together (upper >= lower, matching
    // validateSettings()) instead of field-by-field, so a combination that only makes sense
    // together doesn't get half-applied; if the combination doesn't hold, whichever of the two
    // was actually present in this upload gets rejected (and alerted), not silently kept as the
    // old value under a new, now-inconsistent partner.
    JsonVariantConst upperLimit = watering["upper_limit"];
    JsonVariantConst lowerLimit = watering["lower_limit"];
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
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.upper_limit");
        }
        if (!lowerOk) {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.lower_limit");
        }
        if (upperOk && lowerOk) {
            if (newUpper >= newLower) {
                out.upperLimitPercent = newUpper;
                out.lowerLimitPercent = newLower;
            } else {
                if (hasValue(upperLimit)) {
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.upper_limit");
                }
                if (hasValue(lowerLimit)) {
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.lower_limit");
                }
            }
        }
    }

    JsonVariantConst rainSkipProbability = watering["rain_skip_probability_percent"];
    if (hasValue(rainSkipProbability)) {
        if (rainSkipProbability.is<long>() && rainSkipProbability.as<long>() >= 0 &&
            rainSkipProbability.as<long>() <= 100) {
            out.rainSkipProbabilityPercent = static_cast<int16_t>(rainSkipProbability.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.rain_skip_probability_percent");
        }
    }

    JsonVariantConst rainSkipAmount = watering["rain_skip_amount_mm"];
    if (hasValue(rainSkipAmount)) {
        if (rainSkipAmount.is<long>() && rainSkipAmount.as<long>() >= 0) {
            out.rainSkipAmountMm = static_cast<int16_t>(rainSkipAmount.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.rain_skip_amount_mm");
        }
    }

    // The three correction-formula coefficients. is<float>() accepts JSON integers too, so
    // "temp_modifier_pct_per_degree": 16 is as valid as 16.0 - HA's input_number templates emit
    // whichever the step size happens to produce. Bounds mirror validateSettings()'s.
    struct FloatField {
        const char* key;
        const char* rejectLabel;
        float minValue;
        float maxValue;
        float* target;
    };
    const FloatField floatFields[] = {
        {"base_temp_c", "Watering.base_temp_c", -50.0f, 60.0f, &out.baseTempC},
        {"temp_modifier_pct_per_degree", "Watering.temp_modifier_pct_per_degree", 0.0f, 100.0f,
         &out.tempModifierPctPerDegree},
        {"rain_modifier_pct_per_mm", "Watering.rain_modifier_pct_per_mm", 0.0f, 100.0f,
         &out.rainModifierPctPerMm},
    };
    for (const FloatField& field : floatFields) {
        JsonVariantConst value = watering[field.key];
        if (!hasValue(value)) {
            continue;
        }
        if (value.is<float>() && value.as<float>() >= field.minValue && value.as<float>() <= field.maxValue) {
            *field.target = value.as<float>();
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, field.rejectLabel);
        }
    }

    JsonVariantConst weatherApiKey = watering["weatherapi_key"];
    if (hasValue(weatherApiKey)) {
        if (weatherApiKey.is<const char*>() &&
            strlen(weatherApiKey.as<const char*>()) < sizeof(out.weatherApiKey)) {
            snprintf(out.weatherApiKey, sizeof(out.weatherApiKey), "%s", weatherApiKey.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "Watering.weatherapi_key");
        }
    }

    // Zones is merged per zone index rather than as a single field, so an upload naming
    // only e.g. zone 3 doesn't wipe the other zones' names/enabled state the way replacing the
    // whole sub-object would.
    JsonVariantConst zones = doc["Zones"];
    if (!zones.isNull()) {
        for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
            char key[4];
            snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
            JsonVariantConst entry = zones[key];
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
                        snprintf(fieldLabel, sizeof(fieldLabel), "Zones.%u",
                                 static_cast<unsigned>(i + 1));
                        appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                    }
                }
                continue;
            }

            if (!entry.is<JsonObjectConst>()) {
                snprintf(fieldLabel, sizeof(fieldLabel), "Zones.%u", static_cast<unsigned>(i + 1));
                appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                continue;
            }

            JsonVariantConst name = entry["name"];
            if (hasValue(name)) {
                if (name.is<const char*>() && strlen(name.as<const char*>()) < sizeof(out.zoneNames[i])) {
                    snprintf(out.zoneNames[i], sizeof(out.zoneNames[i]), "%s", name.as<const char*>());
                } else {
                    snprintf(fieldLabel, sizeof(fieldLabel), "Zones.%u.name",
                             static_cast<unsigned>(i + 1));
                    appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, fieldLabel);
                }
            }

            JsonVariantConst enabled = entry["enabled"];
            if (hasValue(enabled)) {
                if (enabled.is<bool>()) {
                    out.zoneEnabled[i] = enabled.as<bool>();
                } else {
                    snprintf(fieldLabel, sizeof(fieldLabel), "Zones.%u.enabled",
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

    JsonObject watering = doc["Watering"].to<JsonObject>();
    watering["watering_percentage"] = settings.wateringPercentage;
    watering["schedule_active"] = settings.scheduleActive;
    watering["default_manual_duration"] = settings.defaultManualDuration;
    watering["auto_adjustment_enabled"] = settings.autoAdjustmentEnabled;
    watering["upper_limit"] = settings.upperLimitPercent;
    watering["lower_limit"] = settings.lowerLimitPercent;
    watering["rain_skip_probability_percent"] = settings.rainSkipProbabilityPercent;
    watering["rain_skip_amount_mm"] = settings.rainSkipAmountMm;
    watering["base_temp_c"] = settings.baseTempC;
    watering["temp_modifier_pct_per_degree"] = settings.tempModifierPctPerDegree;
    watering["rain_modifier_pct_per_mm"] = settings.rainModifierPctPerMm;
    watering["weatherapi_key"] = settings.weatherApiKey;

    JsonObject location = doc["Location"].to<JsonObject>();
    location["location_known"] = settings.locationKnown;
    location["latitude"] = settings.latitude;
    location["longitude"] = settings.longitude;
    location["city"] = settings.city;

    JsonObject currentSensor = doc["Current_sensor"].to<JsonObject>();
    currentSensor["valve_current_base_level"] = settings.valveCurrentBaseLevel;
    currentSensor["pump_current_base_level"] = settings.pumpCurrentBaseLevel;
    currentSensor["alert_pump_uc"] = settings.alertPumpUc;
    currentSensor["alert_pump_oc"] = settings.alertPumpOc;
    currentSensor["alert_valve_uc"] = settings.alertValveUc;
    currentSensor["alert_valve_oc"] = settings.alertValveOc;

    JsonObject zones = doc["Zones"].to<JsonObject>();
    for (uint8_t i = 0; i < board::ZONE_COUNT; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i + 1));
        JsonObject entry = zones[key].to<JsonObject>();
        entry["name"] = settings.zoneNames[i];
        entry["enabled"] = settings.zoneEnabled[i];
    }

    return serializeJson(doc, buffer, bufferSize);
}

}  // namespace irrigator::domain
