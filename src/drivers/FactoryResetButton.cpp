#include "FactoryResetButton.h"

#include "Logger.h"

namespace irrigator::drivers {

void FactoryResetButton::begin() {
    button_.begin(board::FACTORY_RESET_BUTTON_PIN);
    button_.setLongClickTime(board::FACTORY_RESET_HOLD_MS);
    button_.setLongClickDetectedHandler([this](Button2&) {
        Log.info("[FACTORY-RESET] 10s hold detected");
        pendingTrigger_ = true;
    });
}

bool FactoryResetButton::poll() {
    button_.loop();
    if (pendingTrigger_) {
        pendingTrigger_ = false;
        return true;
    }
    return false;
}

}  // namespace irrigator::drivers
