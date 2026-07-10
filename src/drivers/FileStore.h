#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

class FileStore {
public:
    bool begin();
    bool exists(const char* path);
    bool readText(const char* path, String& out);
    bool writeText(const char* path, const String& content);
    bool remove(const char* path);
    size_t freeBytes() const;
};

}  // namespace irrigator::drivers