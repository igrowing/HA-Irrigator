#include "LedIndicator.h"

namespace irrigator::drivers {

void LedIndicator::begin() {
    pinMode(pin_, OUTPUT);
    writeActiveLow(false);
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
            writeActiveLow(true);
            break;
        case LedMode::CaptivePortal:
            writeActiveLow((nowMs - modeStartedAtMs_) % 1000UL < 300UL);
            break;
        case LedMode::Alert:
            writeActiveLow((nowMs - modeStartedAtMs_) % 2000UL < 1000UL);
            break;
        case LedMode::Ota:
            writeActiveLow((nowMs - modeStartedAtMs_) % 333UL < 166UL);
            break;
        case LedMode::BreathOnce:
            writeActiveLow((nowMs - modeStartedAtMs_) < 2000UL);
            if (nowMs - modeStartedAtMs_ >= 2000UL) {
                setMode(previousMode_ == LedMode::BreathOnce ? LedMode::Off : previousMode_);
            }
            break;
        case LedMode::Off:
        default:
            writeActiveLow(false);
            break;
    }
}

void LedIndicator::writeActiveLow(bool on) {
    digitalWrite(pin_, on ? LOW : HIGH);
}

}  // namespace irrigator::drivers