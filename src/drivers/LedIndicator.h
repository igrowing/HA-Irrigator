#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

enum class LedMode : uint8_t { Off, SolidOn, CaptivePortal, Alert, Ota, BreathOnce };

class LedIndicator {
public:
    explicit LedIndicator(uint8_t pin) : pin_(pin) {}
    void begin();
    void setMode(LedMode mode);
    LedMode mode() const { return mode_; }
    void update(uint32_t nowMs);

private:
    void writeActiveLow(bool on);

    uint8_t pin_;
    LedMode mode_ = LedMode::Off;
    LedMode previousMode_ = LedMode::Off;
    uint32_t modeStartedAtMs_ = 0;
};

}  // namespace irrigator::drivers