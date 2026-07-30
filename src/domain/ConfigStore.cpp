#include "ConfigStore.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include "board_config.h"

namespace irrigator::domain {

namespace {

void copyField(JsonDocument& doc, const char* key, char* dest, size_t destSize) {
    const char* value = doc[key] | "";
    snprintf(dest, destSize, "%s", value);
}

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

bool validateConfig(const DeviceConfig& config) {
    if (config.ssid[0] == '\0') {
        return false;
    }
    // MQTT is optional: an empty broker means "MQTT disabled", so there's nothing to validate
    // about port/user/password in that case. A broker was given, though, its port must be usable.
    if (config.mqttBroker[0] != '\0' && config.mqttPort == 0) {
        return false;
    }
    if (config.timezoneOffsetHours < board::TIMEZONE_OFFSET_MIN_HOURS ||
        config.timezoneOffsetHours > board::TIMEZONE_OFFSET_MAX_HOURS) {
        return false;
    }
    return true;
}

bool parseConfig(const char* json, size_t length, DeviceConfig& out) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, length);
    if (error) {
        return false;
    }

    DeviceConfig parsed;
    copyField(doc, "ssid", parsed.ssid, sizeof(parsed.ssid));
    copyField(doc, "password", parsed.password, sizeof(parsed.password));
    copyField(doc, "mqtt_broker", parsed.mqttBroker, sizeof(parsed.mqttBroker));
    copyField(doc, "mqtt_user", parsed.mqttUser, sizeof(parsed.mqttUser));
    copyField(doc, "mqtt_password", parsed.mqttPassword, sizeof(parsed.mqttPassword));
    parsed.mqttPort = doc["mqtt_port"] | static_cast<uint16_t>(board::DEFAULT_MQTT_PORT);
    parsed.timezoneOffsetHours = static_cast<int8_t>(doc["timezone"] | 0);

    if (!validateConfig(parsed)) {
        return false;
    }

    out = parsed;
    return true;
}

bool mergeConfig(const DeviceConfig& existing,
                  const char* json,
                  size_t length,
                  DeviceConfig& out,
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

    JsonVariantConst ssid = doc["ssid"];
    if (hasValue(ssid)) {
        if (ssid.is<const char*>() && strlen(ssid.as<const char*>()) < sizeof(out.ssid)) {
            snprintf(out.ssid, sizeof(out.ssid), "%s", ssid.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "ssid");
        }
    }

    JsonVariantConst password = doc["password"];
    if (hasValue(password)) {
        if (password.is<const char*>() && strlen(password.as<const char*>()) < sizeof(out.password)) {
            snprintf(out.password, sizeof(out.password), "%s", password.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "password");
        }
    }

    JsonVariantConst mqttBroker = doc["mqtt_broker"];
    if (hasValue(mqttBroker)) {
        if (mqttBroker.is<const char*>() &&
            strlen(mqttBroker.as<const char*>()) < sizeof(out.mqttBroker)) {
            snprintf(out.mqttBroker, sizeof(out.mqttBroker), "%s", mqttBroker.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "mqtt_broker");
        }
    }

    JsonVariantConst mqttUser = doc["mqtt_user"];
    if (hasValue(mqttUser)) {
        if (mqttUser.is<const char*>() && strlen(mqttUser.as<const char*>()) < sizeof(out.mqttUser)) {
            snprintf(out.mqttUser, sizeof(out.mqttUser), "%s", mqttUser.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "mqtt_user");
        }
    }

    JsonVariantConst mqttPassword = doc["mqtt_password"];
    if (hasValue(mqttPassword)) {
        if (mqttPassword.is<const char*>() &&
            strlen(mqttPassword.as<const char*>()) < sizeof(out.mqttPassword)) {
            snprintf(out.mqttPassword, sizeof(out.mqttPassword), "%s", mqttPassword.as<const char*>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "mqtt_password");
        }
    }

    JsonVariantConst mqttPort = doc["mqtt_port"];
    if (hasValue(mqttPort)) {
        if (mqttPort.is<long>() && mqttPort.as<long>() >= 1 && mqttPort.as<long>() <= 65535) {
            out.mqttPort = static_cast<uint16_t>(mqttPort.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "mqtt_port");
        }
    }

    JsonVariantConst timezone = doc["timezone"];
    if (hasValue(timezone)) {
        if (timezone.is<long>() && timezone.as<long>() >= board::TIMEZONE_OFFSET_MIN_HOURS &&
            timezone.as<long>() <= board::TIMEZONE_OFFSET_MAX_HOURS) {
            out.timezoneOffsetHours = static_cast<int8_t>(timezone.as<long>());
        } else {
            appendRejected(rejectedFieldsOut, rejectedFieldsOutSize, "timezone");
        }
    }

    return true;
}

size_t serializeConfig(const DeviceConfig& config, char* buffer, size_t bufferSize) {
    JsonDocument doc;
    doc["ssid"] = config.ssid;
    doc["password"] = config.password;
    doc["mqtt_broker"] = config.mqttBroker;
    doc["mqtt_port"] = config.mqttPort;
    doc["mqtt_user"] = config.mqttUser;
    doc["mqtt_password"] = config.mqttPassword;
    doc["timezone"] = config.timezoneOffsetHours;
    return serializeJson(doc, buffer, bufferSize);
}

}  // namespace irrigator::domain
