#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

class CurrentSensor {
public:
    // zeroOffsetMv: sensor output (in mV, via analogReadMilliVolts) at 0A — the DC bias most
    // AC current-sensor breakout boards center their swing on. milliVoltsPerMilliAmp: sensor
    // sensitivity. Both are hardware-specific and MUST be calibrated against the actual
    // installed sensor module — the defaults in board_config.h are placeholders, not a
    // real calibration (see agents.md Step 5: drivers/ needs hardware-in-the-loop verification).
    CurrentSensor(uint8_t adcPin, uint16_t zeroOffsetMv, float milliVoltsPerMilliAmp)
        : adcPin_(adcPin), zeroOffsetMv_(zeroOffsetMv), milliVoltsPerMilliAmp_(milliVoltsPerMilliAmp) {}
    void begin();
    uint16_t readMilliAmps() const;
    uint16_t readRawMilliVolts() const;

    // Overrides the constructor's calibration at runtime — used to apply a saved calibration
    // result from settings.json (see domain::Settings) once it's loaded, or a fresh one pushed
    // over MQTT (cmd/set_calibration).
    void setCalibration(uint16_t zeroOffsetMv, float milliVoltsPerMilliAmp) {
        zeroOffsetMv_ = zeroOffsetMv;
        milliVoltsPerMilliAmp_ = milliVoltsPerMilliAmp;
    }

private:
    uint8_t adcPin_;
    uint16_t zeroOffsetMv_;
    float milliVoltsPerMilliAmp_;
};

}  // namespace irrigator::drivers
