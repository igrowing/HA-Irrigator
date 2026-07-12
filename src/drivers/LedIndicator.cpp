#include "LedIndicator.h"

namespace irrigator::drivers {

void LedIndicator::begin() {
    pinMode(pin_, OUTPUT);
    writeOutput(false);
}

void LedIndicator::setMode(LedMode mode) {
    if (mode_ == mode) {
        return;
    }
    previousMode_ = mode_;
    mode_ = mode;
    modeStartedAtMs_ = millis();
}

void LedIndicator::update(uint32_t nowMs) {
    switch (mode_) {
        case LedMode::SolidOn:
            writeOutput(true);
            break;
        case LedMode::CaptivePortal:
            writeOutput((nowMs - modeStartedAtMs_) % 1000UL < 300UL);
            break;
        case LedMode::Alert: {
            // Two short blinks within the first second, then a 2s dark pause (3s total cycle).
            const uint32_t phase = (nowMs - modeStartedAtMs_) % 3000UL;
            const bool on = phase < 1000UL &&
                            (phase < 150UL || (phase >= 400UL && phase < 550UL));
            writeOutput(on);
            break;
        }
        case LedMode::Ota:
            writeOutput((nowMs - modeStartedAtMs_) % 333UL < 166UL);
            break;
        case LedMode::BreathOnce:
            writeOutput((nowMs - modeStartedAtMs_) < 2000UL);
            if (nowMs - modeStartedAtMs_ >= 2000UL) {
                setMode(previousMode_ == LedMode::BreathOnce ? LedMode::Off : previousMode_);
            }
            break;
        case LedMode::Off:
        default:
            writeOutput(false);
            break;
    }
}

void LedIndicator::writeOutput(bool on) {
    digitalWrite(pin_, on ? HIGH : LOW);
}

}  // namespace irrigator::drivers