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
    // Plain standard-time offset, UTC-13..+13, no DST baked in (drivers::Clock adds that on top —
    // see its begin()). Device-derived, not user-entered: the captive portal has no timezone field
    // anymore, and main.ino's tryGeolocateIfDue writes this from ip-api.com's `offset` the first
    // time geolocation succeeds. 0/UTC is the placeholder it holds until then. mergeConfig() below
    // still accepts an uploaded `timezone`, deliberately — that upload is the manual override for
    // the cases geolocation can't get right (a non-EU DST rule, a sub-hour zone; see
    // domain::standardUtcOffsetHoursFromCurrent), and it sticks because geolocation only ever
    // resolves once per device (Settings::locationKnown).
    int8_t timezoneOffsetHours = 0;
};

bool validateConfig(const DeviceConfig& config);

// Rejects (returns false, leaves `out` untouched) on malformed JSON or a config that fails
// validateConfig() — never substitutes a placeholder for a missing/invalid required field.
bool parseConfig(const char* json, size_t length, DeviceConfig& out);

// Returns the number of bytes written, or 0 if `buffer` was too small.
size_t serializeConfig(const DeviceConfig& config, char* buffer, size_t bufferSize);

// Merges `json` onto `existing` field by field: a field present in `json` with a non-null,
// non-empty value is validated on its own and applied to `out` only if it individually passes;
// a field that's missing, null, an empty string, or fails its own check instead keeps
// `existing`'s value, and (only for the fails-its-own-check case) its name is appended,
// comma-separated, into `rejectedFieldsOut` for the caller to alert on. Returns false only when
// `json` doesn't parse as a JSON object at all — there's nothing to merge from then, and `out`/
// `rejectedFieldsOut` are left untouched.
bool mergeConfig(const DeviceConfig& existing,
                  const char* json,
                  size_t length,
                  DeviceConfig& out,
                  char* rejectedFieldsOut,
                  size_t rejectedFieldsOutSize);

}  // namespace irrigator::domain
