#pragma once

#include <Arduino.h>
#include <Button2.h>

#include "board_config.h"

namespace irrigator::drivers {

// The board's BOOT button (GPIO0), repurposed as a factory-reset trigger. Poll-only, same
// rationale as RotaryInput — see its header comment on why decode/debounce logic must not run
// from an ISR on this project.
class FactoryResetButton {
public:
    void begin();

    // Returns true exactly once, the moment the button has been held continuously for
    // board::FACTORY_RESET_HOLD_MS (fires while still held, not on release).
    bool poll();

private:
    Button2 button_{board::FACTORY_RESET_BUTTON_PIN};
    bool pendingTrigger_ = false;
};

}  // namespace irrigator::drivers
