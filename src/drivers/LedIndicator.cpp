#include "LedIndicator.h"

#include <math.h>

namespace irrigator::drivers {

namespace {
// Fixed PWM config for the status LED. 5kHz is well above flicker fusion and inaudible; 8-bit
// duty resolution is plenty for a breathing effect and keeps the math cheap.
constexpr uint32_t kPwmFreqHz = 5000;
constexpr uint8_t kPwmResolutionBits = 8;
constexpr uint32_t kPwmMaxDuty = (1UL << kPwmResolutionBits) - 1;

constexpr uint32_t kBreathPeriodMs = 4000;  // One full dim->bright->dim cycle.
constexpr uint32_t kBreathMinDuty = kPwmMaxDuty / 10;  // 10%
constexpr uint32_t kBreathMaxDuty = kPwmMaxDuty;       // 100%
}  // namespace

void LedIndicator::begin() {
    // Once attached, the pin is routed through the LEDC peripheral for the rest of its life —
    // writeOutput() below drives it with ledcWrite() (full/zero duty) rather than digitalWrite(),
    // so every mode shares one output path and Breathe can hand it a variable duty cycle.
    ledcAttach(pin_, kPwmFreqHz, kPwmResolutionBits);
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
        case LedMode::Breathe: {
            // Raised-cosine ramp between the min/max duty rather than a linear triangle wave —
            // it eases in/out at the dim and bright ends, which reads as an actual "breath"
            // instead of a robotic ramp-and-snap.
            const uint32_t phase = (nowMs - modeStartedAtMs_) % kBreathPeriodMs;
            const float t = static_cast<float>(phase) / static_cast<float>(kBreathPeriodMs);
            const float wave = (1.0f - cosf(2.0f * static_cast<float>(PI) * t)) / 2.0f;
            const uint32_t duty =
                kBreathMinDuty + static_cast<uint32_t>(wave * static_cast<float>(kBreathMaxDuty - kBreathMinDuty));
            ledcWrite(pin_, duty);
            break;
        }
        case LedMode::Off:
        default:
            writeOutput(false);
            break;
    }
}

void LedIndicator::writeOutput(bool on) {
    ledcWrite(pin_, on ? kPwmMaxDuty : 0);
}

}  // namespace irrigator::drivers