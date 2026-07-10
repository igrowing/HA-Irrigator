#include "FileStore.h"

#include <SPIFFS.h>

#include "Logger.h"

namespace irrigator::drivers {

bool FileStore::begin() {
    const bool ok = SPIFFS.begin(true);
    if (!ok) {
        Log.error("SPIFFS mount failed");
    }
    return ok;
}

bool FileStore::exists(const char* path) {
    return SPIFFS.exists(path);
}

bool FileStore::readText(const char* path, String& out) {
    File file = SPIFFS.open(path, "r");
    if (!file) {
        Log.printf("[ERROR] Cannot open %s for reading\n", path);
        return false;
    }
    out = file.readString();
    file.close();
    return true;
}

bool FileStore::writeText(const char* path, const String& content) {
    File file = SPIFFS.open(path, "w");
    if (!file) {
        Log.printf("[ERROR] Cannot open %s for writing\n", path);
        return false;
    }
    const size_t written = file.print(content);
    file.close();
    if (written != content.length()) {
        Log.printf("[ERROR] Short write to %s\n", path);
        return false;
    }
    return true;
}

bool FileStore::remove(const char* path) {
    if (!SPIFFS.exists(path)) {
        return true;
    }
    const bool ok = SPIFFS.remove(path);
    if (!ok) {
        Log.printf("[ERROR] Cannot remove %s\n", path);
    }
    return ok;
}

size_t FileStore::freeBytes() const {
    return SPIFFS.totalBytes() - SPIFFS.usedBytes();
}

}  // namespace irrigator::drivers