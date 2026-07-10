#pragma once

#include <stdint.h>

#include "board_config.h"

namespace irrigator::domain {

struct Settings {
    int16_t wateringPercentage = board::DEFAULT_WATERING_PERCENTAGE;
    char zoneNames[board::ZONE_COUNT][16] = {};
    bool scheduleActive = true;
    uint8_t defaultManualDuration = board::DEFAULT_MANUAL_DURATION_MINUTES;
};

Settings makeDefaultSettings();
bool validateSettings(const Settings& settings);

}  // namespace irrigator::domain