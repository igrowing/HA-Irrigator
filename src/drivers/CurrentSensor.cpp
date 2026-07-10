#include "CurrentSensor.h"

#include "board_config.h"

namespace irrigator::drivers {

void CurrentSensor::begin() {
    pinMode(adcPin_, INPUT);
}

uint16_t CurrentSensor::readRaw() const {
    const int raw = analogRead(adcPin_);
    return raw < 0 ? 0 : static_cast<uint16_t>(raw);
}

uint16_t CurrentSensor::readMilliAmps() const {
    // TODO: replace with calibrated conversion for the installed current transformer/module.
    return readRaw();
}

}  // namespace irrigator::drivers