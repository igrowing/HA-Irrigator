#pragma once

#include <Arduino.h>

#include "../domain/IrrigationStateMachine.h"

namespace irrigator::ui {

class IrrigatorUi {
public:
    void begin();
    void showCaptivePortal(const String& apSsid, const IPAddress& ip);
    void showNormal(const ::irrigator::domain::IrrigationSnapshot& snapshot);
    void displayOff();
};

}  // namespace irrigator::ui