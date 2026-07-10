#pragma once

#include <stddef.h>
#include <stdint.h>

namespace irrigator::domain {

struct DeviceConfig {
    char ssid[33] = "";
    char password[64] = "";
    char mqttBroker[64] = "";
    uint16_t mqttPort = 1883;
    char mqttUser[33] = "";
    char mqttPassword[64] = "";
    int8_t timezoneOffsetHours = 0;  // UTC-13..+13
};

bool validateConfig(const DeviceConfig& config);

// Rejects (returns false, leaves `out` untouched) on malformed JSON or a config that fails
// validateConfig() — never substitutes a placeholder for a missing/invalid required field.
bool parseConfig(const char* json, size_t length, DeviceConfig& out);

// Returns the number of bytes written, or 0 if `buffer` was too small.
size_t serializeConfig(const DeviceConfig& config, char* buffer, size_t bufferSize);

}  // namespace irrigator::domain
