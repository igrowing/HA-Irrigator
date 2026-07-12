#include "RelayBank.h"

#include "Logger.h"

namespace irrigator::drivers {

bool RelayBank::begin() {
    for (uint8_t zone = 1; zone <= board::ZONE_COUNT; ++zone) {
        pinMode(board::GPIO_ZONE_PINS[zone - 1U], OUTPUT);
    }
    pinMode(board::PUMP_RELAY_PIN, OUTPUT);
    allOff();
    return true;
}

bool RelayBank::setZone(uint8_t zone, bool on) {
    if (zone < 1 || zone > board::ZONE_COUNT) {
        Log.printf("[ERROR] Invalid zone %u\n", static_cast<unsigned>(zone));
        return false;
    }

    digitalWrite(board::GPIO_ZONE_PINS[zone - 1U], on ? LOW : HIGH);
    zoneOn_[zone - 1U] = on;
    return true;
}

bool RelayBank::setPump(bool on) {
    digitalWrite(board::PUMP_RELAY_PIN, on ? LOW : HIGH);
    pumpOn_ = on;
    return true;
}

bool RelayBank::isZoneOn(uint8_t zone) const {
    if (zone < 1 || zone > board::ZONE_COUNT) {
        return false;
    }
    return zoneOn_[zone - 1U];
}

void RelayBank::allOff() {
    for (uint8_t zone = 1; zone <= board::ZONE_COUNT; ++zone) {
        digitalWrite(board::GPIO_ZONE_PINS[zone - 1U], HIGH);
        zoneOn_[zone - 1U] = false;
    }
    digitalWrite(board::PUMP_RELAY_PIN, HIGH);
    pumpOn_ = false;
}

}  // namespace irrigator::drivers
