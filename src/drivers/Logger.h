#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

class Logger {
public:
    void begin(uint32_t baud = 115200);
    void info(const __FlashStringHelper* message);
    void info(const char* message);
    void warn(const char* message);
    void error(const char* message);
    void printf(const char* format, ...);
};

extern Logger Log;

}  // namespace irrigator::drivers