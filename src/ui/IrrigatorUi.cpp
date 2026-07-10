#include "IrrigatorUi.h"

#include "../drivers/Logger.h"

namespace irrigator::ui {

void IrrigatorUi::begin() {
    ::irrigator::drivers::Log.info("UI skeleton initialized");
    // TODO: initialize SSD1309 U8g2 driver and render welcome screen.
}

void IrrigatorUi::showCaptivePortal(const String& apSsid, const IPAddress& ip) {
    ::irrigator::drivers::Log.printf("[INFO] Captive portal display: %s %s\n",
                                     apSsid.c_str(),
                                     ip.toString().c_str());
}

void IrrigatorUi::showNormal(const ::irrigator::domain::IrrigationSnapshot& snapshot) {
    ::irrigator::drivers::Log.printf("[INFO] UI normal state=%u zone=%u pump=%d\n",
                                     static_cast<unsigned>(snapshot.state),
                                     static_cast<unsigned>(snapshot.activeZone),
                                     snapshot.pumpOn);
}

void IrrigatorUi::displayOff() {
    ::irrigator::drivers::Log.info("Display off");
}

}  // namespace irrigator::ui