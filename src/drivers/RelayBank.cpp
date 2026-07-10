#include "RelayBank.h"

#include <Wire.h>

#include "Logger.h"

namespace irrigator::drivers {

bool RelayBank::begin() {
    pinMode(board::GPIO_ZONE_9_PIN, OUTPUT);
    pinMode(board::GPIO_ZONE_10_PIN, OUTPUT);
    pinMode(board::GPIO_ZONE_11_PIN, OUTPUT);
    pinMode(board::PUMP_RELAY_PIN, OUTPUT);
    allOff();
    return writePcf8574();
}

bool RelayBank::setZone(uint8_t zone, bool on) {
    if (zone < 1 || zone > board::ZONE_COUNT) {
        Log.printf("[ERROR] Invalid zone %u\n", static_cast<unsigned>(zone));
        return false;
    }

    if (zone <= board::PCF8574_ZONE_COUNT) {
        const uint8_t bit = static_cast<uint8_t>(zone - 1U);
        if (on) {
            pcfState_ &= static_cast<uint8_t>(~(1U << bit));
        } else {
            pcfState_ |= static_cast<uint8_t>(1U << bit);
        }
        return writePcf8574();
    }

    const uint8_t pin = gpioPinForZone(zone);
    digitalWrite(pin, on ? LOW : HIGH);
    gpioZoneOn_[zone - 9U] = on;
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
    if (zone <= board::PCF8574_ZONE_COUNT) {
        const uint8_t bit = static_cast<uint8_t>(zone - 1U);
        return (pcfState_ & (1U << bit)) == 0;
    }
    return gpioZoneOn_[zone - 9U];
}

void RelayBank::allOff() {
    pcfState_ = 0xFF;
    writePcf8574();
    digitalWrite(board::GPIO_ZONE_9_PIN, HIGH);
    digitalWrite(board::GPIO_ZONE_10_PIN, HIGH);
    digitalWrite(board::GPIO_ZONE_11_PIN, HIGH);
    digitalWrite(board::PUMP_RELAY_PIN, HIGH);
    gpioZoneOn_[0] = false;
    gpioZoneOn_[1] = false;
    gpioZoneOn_[2] = false;
    pumpOn_ = false;
}

bool RelayBank::writePcf8574() {
    Wire.beginTransmission(board::PCF8574_ADDRESS);
    Wire.write(pcfState_);
    const uint8_t error = Wire.endTransmission();
    if (error != 0) {
        Log.printf("[ERROR] PCF8574 write failed: %u\n", static_cast<unsigned>(error));
        return false;
    }
    return true;
}

uint8_t RelayBank::gpioPinForZone(uint8_t zone) {
    switch (zone) {
        case 9:
            return board::GPIO_ZONE_9_PIN;
        case 10:
            return board::GPIO_ZONE_10_PIN;
        case 11:
            return board::GPIO_ZONE_11_PIN;
        default:
            return board::GPIO_ZONE_9_PIN;
    }
}

}  // namespace irrigator::drivers