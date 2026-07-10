#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

class CurrentSensor {
public:
    explicit CurrentSensor(uint8_t adcPin) : adcPin_(adcPin) {}
    void begin();
    uint16_t readMilliAmps() const;
    uint16_t readRaw() const;

private:
    uint8_t adcPin_;
};

}  // namespace irrigator::drivers