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

bool FileStore::rename(const char* fromPath, const char* toPath) {
    const bool ok = SPIFFS.rename(fromPath, toPath);
    if (!ok) {
        Log.printf("[ERROR] Cannot rename %s to %s\n", fromPath, toPath);
    }
    return ok;
}

bool FileStore::writeTextWithBackup(const char* path, const String& content) {
    const String bakPath = String(path) + ".bak";
    const bool hadExisting = exists(path);

    if (hadExisting && !rename(path, bakPath.c_str())) {
        Log.printf("[ERROR] Could not back up %s before write; leaving original untouched\n", path);
        return false;
    }

    if (writeText(path, content)) {
        if (hadExisting) {
            remove(bakPath.c_str());
        }
        return true;
    }

    Log.printf("[ERROR] Write to %s failed; restoring previous copy\n", path);
    remove(path);
    if (hadExisting) {
        rename(bakPath.c_str(), path);
    }
    return false;
}

size_t FileStore::freeBytes() const {
    return SPIFFS.totalBytes() - SPIFFS.usedBytes();
}

}  // namespace irrigator::drivers