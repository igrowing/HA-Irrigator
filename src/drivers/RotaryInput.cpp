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
    button_.setLongClickHandler([this](Button2&) {
        Log.info("[ROTARY] button: long click");
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
