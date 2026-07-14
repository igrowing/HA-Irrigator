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
    RotaryEncoder encoder_{board::ROTARY_CLK_PIN, board::ROTARY_DAT_PIN, RotaryEncoder::LatchMode::TWO03};
    Button2 button_{board::ROTARY_BUTTON_PIN};
    int position_ = 0;
    RotaryEvent pendingButtonEvent_ = RotaryEvent::None;

    // Legal quadrature-transition filter, applied in front of the library (see poll()): a real
    // encoder detent only ever changes one signal at a time (Gray-code sequence 00-01-11-10-00
    // or its reverse). A reading where both CLK and DAT appear to have flipped at once is
    // physically impossible for a genuine rotation — it means noise/bounce, and is discarded
    // before the library ever sees it, rather than letting it corrupt the library's own
    // internal "last state" baseline.
    uint8_t lastQuadratureState_ = 0;
};

}  // namespace irrigator::drivers
