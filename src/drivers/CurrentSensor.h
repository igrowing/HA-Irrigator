#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

class CurrentSensor {
public:
    // No zero-offset/slope calibration here — see sampleRmsMv()'s comment for why neither is
    // needed. Readings are only ever compared against a per-sensor "base" captured under real
    // load (MQTT cmd/calibrate), never converted to a real mA value.
    explicit CurrentSensor(uint8_t adcPin) : adcPin_(adcPin) {}
    void begin();
    uint16_t readRawMilliVolts() const;

    // Blocking true-RMS burst: takes durationMs worth of raw ADC samples spaced sampleIntervalMs
    // apart (1ms/1kHz spans several full 50/60Hz mains cycles) into rawMvOut (must be non-null,
    // sized for at least durationMs/sampleIntervalMs entries; sampleCountOut reports how many were
    // actually written). The zero-line is then derived from THIS burst's own (min+max)/2 rather
    // than a fixed/assumed board constant — a periodic, roughly-symmetric AC waveform's own
    // min/max midpoint approximates its true DC center, so this self-adjusts for bias drift
    // between sensor units or with temperature instead of relying on a calibrated offset. Returns
    // sqrt(mean((raw_mV - center_mV)^2)) — a virtual reference level in raw millivolts, not a real
    // current value. This blocks for ~durationMs, so it must only be called from a background
    // task, never from loop() or anything loop() calls.
    uint16_t sampleRmsMv(uint16_t durationMs,
                          uint16_t sampleIntervalMs,
                          uint16_t* rawMvOut,
                          uint16_t maxRawSamples,
                          uint16_t& sampleCountOut) const;

private:
    uint8_t adcPin_;
};

}  // namespace irrigator::drivers
