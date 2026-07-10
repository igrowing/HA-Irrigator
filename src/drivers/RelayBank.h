#pragma once

#include <Arduino.h>

#include "board_config.h"

namespace irrigator::drivers {

class IRelayBank {
public:
    virtual ~IRelayBank() = default;
    virtual bool begin() = 0;
    virtual bool setZone(uint8_t zone, bool on) = 0;
    virtual bool setPump(bool on) = 0;
    virtual bool isZoneOn(uint8_t zone) const = 0;
    virtual bool isPumpOn() const = 0;
    virtual void allOff() = 0;
};

class RelayBank : public IRelayBank {
public:
    bool begin() override;
    bool setZone(uint8_t zone, bool on) override;
    bool setPump(bool on) override;
    bool isZoneOn(uint8_t zone) const override;
    bool isPumpOn() const override { return pumpOn_; }
    void allOff() override;

private:
    bool writePcf8574();
    static uint8_t gpioPinForZone(uint8_t zone);

    uint8_t pcfState_ = 0xFF;  // PCF8574 active-low outputs: 1 = off, 0 = on.
    bool gpioZoneOn_[3] = {false, false, false};
    bool pumpOn_ = false;
};

}  // namespace irrigator::drivers