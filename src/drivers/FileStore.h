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
    bool rename(const char* fromPath, const char* toPath);

    // Best-effort "atomic" overwrite: content is only replaced if a prior copy is safely
    // backed up first, and the backup is restored if the write fails partway through.
    bool writeTextWithBackup(const char* path, const String& content);

    size_t freeBytes() const;
};

}  // namespace irrigator::drivers