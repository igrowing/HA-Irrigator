#include "CurrentSensor.h"

#include "board_config.h"

namespace irrigator::drivers {

void CurrentSensor::begin() {
    pinMode(adcPin_, INPUT);
}

uint16_t CurrentSensor::readRawMilliVolts() const {
    const int milliVolts = analogReadMilliVolts(adcPin_);
    return milliVolts < 0 ? 0 : static_cast<uint16_t>(milliVolts);
}

uint16_t CurrentSensor::readMilliAmps() const {
    const uint16_t milliVolts = readRawMilliVolts();
    if (milliVolts <= zeroOffsetMv_ || milliVoltsPerMilliAmp_ <= 0.0f) {
        return 0;
    }
    const float currentMa = static_cast<float>(milliVolts - zeroOffsetMv_) / milliVoltsPerMilliAmp_;
    return currentMa < 0.0f ? 0 : static_cast<uint16_t>(currentMa);
}

}  // namespace irrigator::drivers
