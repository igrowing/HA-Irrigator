#pragma once

#include <Arduino.h>

namespace irrigator::drivers {

// Compact, human/tool-decodable extract of an ELF-format core dump - enough to identify what
// crashed without transferring the whole (up to 64KB) image. programCounter/backtrace are raw
// code addresses; decode them against the exact firmware.elf that was running when it crashed
// (see docs/coredump.md) with e.g. `xtensa-esp32-elf-addr2line -pfiaC -e firmware.elf <address>`.
struct CoreDumpSummary {
    char taskName[32] = "";  // esp_core_dump_summary_t::exc_task is char[16]; copied bounded.
    uint32_t programCounter = 0;
    uint32_t exceptionCause = 0;
    uint32_t faultAddress = 0;
    static constexpr uint8_t kMaxBacktraceDepth = 16;  // Matches esp_core_dump_bt_info_t::bt[16].
    uint32_t backtrace[kMaxBacktraceDepth] = {};
    uint8_t backtraceDepth = 0;
    bool backtraceCorrupted = false;
    char panicReason[200] = "";
    // Truncated SHA256 of the app ELF that was running at crash time (APP_ELF_SHA256_SZ, currently
    // 9 chars + NUL from CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9). Every address above is meaningless
    // against the wrong build, and this device is updated over the air - so this is the only way,
    // after the fact, to tell which archived firmware.elf to decode against. Sized generously so a
    // future CONFIG_APP_RETRIEVE_LEN_ELF_SHA bump can't overflow it.
    char appElfSha256[24] = "";
};

// Thin wrapper around ESP-IDF's espcoredump component. CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH and
// _DATA_FORMAT_ELF are already on by default in this project's prebuilt Arduino core (checked
// against the framework's esp32/sdkconfig) - the panic handler writes here automatically on any
// crash. The only thing that was missing is a `coredump`-subtype partition to write into (see
// partitions.csv); with none present every call here simply reports "not found", which is exactly
// what every earlier boot on this device did. Requires no begin()/init - the underlying APIs are
// documented as "always available, even when core dump is disabled in menuconfig".
class CoreDumpService {
public:
    // True if a valid (checksum-verified) core dump is currently stored in flash. NOT cheap: the
    // underlying esp_core_dump_image_check() re-reads and re-CRC32s the whole stored image on
    // every call. Call it at boot and at the start of a transfer, never from loop() - main.ino
    // caches the boot-time answer in `coreDumpPresent` for exactly this reason.
    bool available() const;

    // Populates `out` from the stored dump. Returns false (leaving `out` untouched) if there is
    // no valid dump, or the underlying summary API fails.
    bool readSummary(CoreDumpSummary& out) const;

    // Total bytes of the stored dump image (including its checksum trailer), for sizing a chunked
    // transfer - 0 if there is no valid dump.
    size_t totalBytes() const;

    // Reads up to `length` bytes of the raw ELF dump starting at byte `offset`, for chunked
    // transfer over MQTT (see main.ino's serviceCoreDumpTransfer). Returns bytes actually read
    // - 0 at/past the end of the image, or on any flash error.
    size_t readChunk(size_t offset, uint8_t* buffer, size_t length) const;

    // Erases the stored dump so a stale one can't be confused with a future crash. Call only
    // after the operator has independently confirmed a downloaded copy is complete and valid
    // (cmd/coredump_clear) - never automatically after sending it. Every MQTT publish in this
    // codebase is fire-and-forget (QoS 0, no delivery acknowledgement anywhere), so an
    // auto-erase-on-send could destroy the only evidence of a crash the instant one chunk drops
    // in transit.
    bool clear() const;
};

}  // namespace irrigator::drivers
