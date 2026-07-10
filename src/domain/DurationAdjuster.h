#pragma once

#include <stdint.h>

#include "IrrigationTypes.h"
#include "board_config.h"

namespace irrigator::domain {

uint16_t adjustDuration(uint16_t current,
                        RotationDirection direction,
                        uint16_t minValue,
                        uint16_t maxValue);

uint16_t adjustManualDuration(uint16_t current, RotationDirection direction);
uint16_t adjustScheduledDuration(uint16_t current, RotationDirection direction);
uint16_t applyWateringPercentage(uint16_t durationMin, int16_t wateringPercentage);

}  // namespace irrigator::domain