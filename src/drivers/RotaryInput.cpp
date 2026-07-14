#include "RotaryInput.h"

#include "Logger.h"

namespace irrigator::drivers {

void RotaryInput::begin() {
    encoder_.setPosition(0);
    position_ = 0;
    lastQuadratureState_ = static_cast<uint8_t>(digitalRead(board::ROTARY_CLK_PIN) |
                                                (digitalRead(board::ROTARY_DAT_PIN) << 1));

    button_.begin(board::ROTARY_BUTTON_PIN);
    button_.setLongClickTime(board::ROTARY_LONG_CLICK_MS);
    button_.setClickHandler([this](Button2&) {
        Log.info("[ROTARY] button: click");
        pendingButtonEvent_ = RotaryEvent::Click;
    });
    button_.setLongClickHandler([this](Button2&) {
        Log.info("[ROTARY] button: long click");
        pendingButtonEvent_ = RotaryEvent::LongClick;
    });
}

RotaryEvent RotaryInput::poll() {
    // Read the pins ourselves (rather than the library's parameterless tick(), which does its
    // own digitalRead internally) so we can filter the reading before the library ever sees it.
    const int sig1 = digitalRead(board::ROTARY_CLK_PIN);
    const int sig2 = digitalRead(board::ROTARY_DAT_PIN);
    const uint8_t thisState = static_cast<uint8_t>(sig1 | (sig2 << 1));

    // Legal-transition filter: reject the two diagonal jumps (00<->11, 01<->10 — both bits
    // differ, XOR == 0b11) as noise; everything else (no change, or either single-bit Gray-code
    // step) is fed through.
    if ((thisState ^ lastQuadratureState_) != 0b11) {
        encoder_.tick(sig1, sig2);
        lastQuadratureState_ = thisState;
    } else {
        Log.printf("[ROTARY] discarded illegal transition %u->%u\n",
                  static_cast<unsigned>(lastQuadratureState_),
                  static_cast<unsigned>(thisState));
    }

    button_.loop();

    if (pendingButtonEvent_ != RotaryEvent::None) {
        const RotaryEvent event = pendingButtonEvent_;
        pendingButtonEvent_ = RotaryEvent::None;
        return event;
    }

    // Adopted from the InputManager approach used successfully in another project: catch up to
    // the encoder's full position in one step (rather than draining a multi-tick backlog one
    // step per poll() call across several loop() iterations) and report a single event for
    // this poll. A fast spin nets one CW/CCW event per call either way; this just resolves it
    // immediately instead of spreading it out.
    const int newPosition = encoder_.getPosition();
    // TODO: Optimise: position_ = newPosition - position_; if (position_ > 0)...
    const int delta = newPosition - position_;
    if (delta > 0) {
        position_ = newPosition;
        Log.printf("[ROTARY] CW (position=%d)\n", position_);
        return RotaryEvent::Clockwise;
    }
    if (delta < 0) {
        position_ = newPosition;
        Log.printf("[ROTARY] CCW (position=%d)\n", position_);
        return RotaryEvent::CounterClockwise;
    }
    return RotaryEvent::None;
}

}  // namespace irrigator::drivers
