#include "RotaryInput.h"

#include "Logger.h"

namespace irrigator::drivers {

void RotaryInput::begin() {
    encoder_.setPosition(0);
    position_ = 0;

    button_.begin(board::ROTARY_BUTTON_PIN);
    button_.setLongClickTime(board::ROTARY_LONG_CLICK_MS);
    button_.setClickHandler([this](Button2&) {
        Log.info("[ROTARY] button: click");
        pendingButtonEvent_ = RotaryEvent::Click;
    });
    // "Detected" (fires the instant the hold crosses ROTARY_LONG_CLICK_MS, while still held) not
    // setLongClickHandler (fires only on release) — menu entry reacts the moment the hold
    // threshold is reached, and the eventual release is left with no handler at all (neither
    // long_cb nor click_cb fire for it, per Button2's _reportClicks()), so it's silently ignored
    // rather than emitting a stray Click.
    button_.setLongClickDetectedHandler([this](Button2&) {
        Log.info("[ROTARY] button: long click (detected on hold)");
        pendingButtonEvent_ = RotaryEvent::LongClick;
    });
}

RotaryEvent RotaryInput::poll() {
    encoder_.tick();
    button_.loop();

    if (pendingButtonEvent_ != RotaryEvent::None) {
        const RotaryEvent event = pendingButtonEvent_;
        pendingButtonEvent_ = RotaryEvent::None;
        return event;
    }

    const int newPosition = encoder_.getPosition();
    const int delta = newPosition - position_;
    if (delta > 0) {
        position_ += 1;
        Log.printf("[ROTARY] CW (position=%d)\n", position_);
        return RotaryEvent::Clockwise;
    }
    if (delta < 0) {
        position_ -= 1;
        Log.printf("[ROTARY] CCW (position=%d)\n", position_);
        return RotaryEvent::CounterClockwise;
    }
    return RotaryEvent::None;
}

}  // namespace irrigator::drivers
