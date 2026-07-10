#include "Settings.h"

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

}  // namespace irrigator::domain