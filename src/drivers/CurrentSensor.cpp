#include "CurrentSensor.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdint.h>

#include "board_config.h"

namespace irrigator::drivers {

void CurrentSensor::begin() {
    pinMode(adcPin_, INPUT);
}

uint16_t CurrentSensor::readRawMilliVolts() const {
    const int milliVolts = analogReadMilliVolts(adcPin_);
    return milliVolts < 0 ? 0 : static_cast<uint16_t>(milliVolts);
}

uint16_t CurrentSensor::sampleRmsMv(uint16_t durationMs,
                                    uint16_t sampleIntervalMs,
                                    uint16_t* rawMvOut,
                                    uint16_t maxRawSamples,
                                    uint16_t& sampleCountOut) const {
    sampleCountOut = 0;
    if (sampleIntervalMs == 0 || rawMvOut == nullptr) {
        return 0;
    }
    const uint32_t sampleCount = durationMs / sampleIntervalMs;
    if (sampleCount == 0) {
        return 0;
    }

    uint16_t minMv = UINT16_MAX;
    uint16_t maxMv = 0;
    for (uint32_t i = 0; i < sampleCount && sampleCountOut < maxRawSamples; ++i) {
        const uint16_t rawMv = readRawMilliVolts();
        rawMvOut[sampleCountOut++] = rawMv;
        if (rawMv < minMv) minMv = rawMv;
        if (rawMv > maxMv) maxMv = rawMv;
        vTaskDelay(pdMS_TO_TICKS(sampleIntervalMs));
    }
    if (sampleCountOut == 0) {
        return 0;
    }

    // See sampleRmsMv()'s header comment: the burst's own min/max midpoint stands in for a
    // calibrated zero-offset.
    const int32_t centerMv = (static_cast<int32_t>(minMv) + static_cast<int32_t>(maxMv)) / 2;
    uint64_t sumSquares = 0;
    for (uint16_t i = 0; i < sampleCountOut; ++i) {
        const int32_t deviationMv = static_cast<int32_t>(rawMvOut[i]) - centerMv;
        sumSquares += static_cast<uint64_t>(deviationMv) * static_cast<uint64_t>(deviationMv);
    }
    return static_cast<uint16_t>(sqrtf(static_cast<float>(sumSquares) / static_cast<float>(sampleCountOut)));
}

}  // namespace irrigator::drivers
