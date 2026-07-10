#include "ConfigStore.h"

#include <ArduinoJson.h>
#include <stdio.h>

#include "board_config.h"

namespace irrigator::domain {

namespace {

void copyField(JsonDocument& doc, const char* key, char* dest, size_t destSize) {
    const char* value = doc[key] | "";
    snprintf(dest, destSize, "%s", value);
}

}  // namespace

bool validateConfig(const DeviceConfig& config) {
    if (config.ssid[0] == '\0') {
        return false;
    }
    if (config.mqttBroker[0] == '\0') {
        return false;
    }
    if (config.mqttPort == 0) {
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
