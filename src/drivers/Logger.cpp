#include "Logger.h"

#include <stdarg.h>

namespace irrigator::drivers {

Logger Log;

void Logger::begin(uint32_t baud) {
    Serial.begin(baud);
}

void Logger::info(const __FlashStringHelper* message) {
    Serial.print(F("[INFO] "));
    Serial.println(message);
}

void Logger::info(const char* message) {
    Serial.print(F("[INFO] "));
    Serial.println(message);
}

void Logger::warn(const char* message) {
    Serial.print(F("[WARN] "));
    Serial.println(message);
}

void Logger::error(const char* message) {
    Serial.print(F("[ERROR] "));
    Serial.println(message);
}

void Logger::printf(const char* format, ...) {
    char buffer[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.print(buffer);
}

}  // namespace irrigator::drivers