#pragma once

#include <Arduino.h>
#include <Button2.h>
#include <RotaryEncoder.h>

#include "board_config.h"

namespace irrigator::drivers {

enum class RotaryEvent : uint8_t { None, Clockwise, CounterClockwise, Click, LongClick };

// Poll-only: encoder decode and button debounce both run from poll(), called every loop()
// iteration — never from an ISR. This project has already hit a real freeze from a
// quadrature-decode state machine running inside a GPIO interrupt handler (see agents.md);
// don't reintroduce that by wiring encoder_.tick() to attachInterrupt().
class RotaryInput {
public:
    void begin();
    RotaryEvent poll();

private:
    // TWO03 assumes the encoder detents at both the 00 and 11 states (2 external counts per
    // click); FOUR0/FOUR3 assume one full 4-state Gray cycle per click (1 count). Most cheap
    // incremental encoders are the latter — if this still reports 2 per click, try FOUR3
    // instead (mirrors FOUR0 for the opposite CLK/DAT phase wiring).
    RotaryEncoder encoder_{board::ROTARY_CLK_PIN, board::ROTARY_DAT_PIN, RotaryEncoder::LatchMode::FOUR0};
    Button2 button_{board::ROTARY_BUTTON_PIN};
    int position_ = 0;
    RotaryEvent pendingButtonEvent_ = RotaryEvent::None;
};

}  // namespace irrigator::drivers
