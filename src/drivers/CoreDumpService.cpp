#include "CoreDumpService.h"

#include <esp_core_dump.h>
#include <esp_partition.h>
#include <string.h>

#include <algorithm>

#include "Logger.h"

namespace irrigator::drivers {

namespace {

// The panic handler always writes the image to the start of the `coredump`-subtype partition, but
// esp_core_dump_image_get() reports its *absolute* flash address while esp_partition_read() wants
// a partition-relative offset. Look the partition up rather than assuming a hardcoded 0x3F0000 -
// partitions.csv is free to move it, and board_config.h is this project's only place for
// hardware constants anyway (agents.md section 1), which a flash offset owned by the linker
// script isn't.
const esp_partition_t* findCoreDumpPartition() {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
}

}  // namespace

bool CoreDumpService::available() const {
    const esp_err_t err = esp_core_dump_image_check();
    if (err == ESP_OK) {
        return true;
    }
    // ESP_ERR_NOT_FOUND is the normal "this device has never crashed" case and isn't worth a log
    // line on every boot; anything else means a dump is there but unusable, which is worth saying
    // out loud rather than silently reporting "no dump" (agents.md section 3).
    if (err != ESP_ERR_NOT_FOUND) {
        Log.printf("[WARN] stored core dump is present but failed its integrity check (err=%d)\n",
                   static_cast<int>(err));
    }
    return false;
}

size_t CoreDumpService::totalBytes() const {
    size_t imageAddr = 0;
    size_t imageSize = 0;
    if (esp_core_dump_image_get(&imageAddr, &imageSize) != ESP_OK) {
        return 0;
    }
    return imageSize;
}

bool CoreDumpService::readSummary(CoreDumpSummary& out) const {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (!available()) {
        return false;
    }

    // esp_core_dump_get_summary() parses the stored ELF and allocates internally while doing it.
    // This runs from the MQTT command path, i.e. right alongside a 25.6KB PubSubClient buffer and
    // whatever a download response is holding, so on a fragmented heap it is a plausible place to
    // die - and dying here would panic the device while it is trying to tell you about the LAST
    // panic, overwriting that dump with a far less interesting one. Refuse rather than risk it:
    // a missing summary costs a diagnostic field, a crash costs the crash evidence itself.
    constexpr size_t kSummaryHeapFloorBytes = 24576;
    const size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < kSummaryHeapFloorBytes) {
        Log.printf("[WARN] skipping core dump summary: only %u B heap free (need %u B)\n",
                   static_cast<unsigned>(freeHeap),
                   static_cast<unsigned>(kSummaryHeapFloorBytes));
        return false;
    }

    // ~220 bytes. On the stack deliberately, and deliberately in this function rather than the
    // caller's: main.ino's handleMqttCommand() already carries the widest frame in the firmware
    // and the build runs -Wstack-usage=2048 over everything, so the summary buffers stay down
    // here in a leaf frame that nothing else shares.
    esp_core_dump_summary_t summary = {};
    const esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err != ESP_OK) {
        Log.printf("[WARN] core dump present but its summary could not be read (err=%d)\n",
                   static_cast<int>(err));
        return false;
    }

    snprintf(out.taskName, sizeof(out.taskName), "%.*s",
             static_cast<int>(sizeof(summary.exc_task)), summary.exc_task);
    out.programCounter = summary.exc_pc;
    out.exceptionCause = summary.ex_info.exc_cause;
    out.faultAddress = summary.ex_info.exc_vaddr;

    out.backtraceCorrupted = summary.exc_bt_info.corrupted;
    out.backtraceDepth = static_cast<uint8_t>(
        std::min<uint32_t>(summary.exc_bt_info.depth, CoreDumpSummary::kMaxBacktraceDepth));
    memcpy(out.backtrace, summary.exc_bt_info.bt, out.backtraceDepth * sizeof(uint32_t));

    // app_elf_sha256 is documented as "SHA256 sum as a string", but it's declared uint8_t[] and
    // nothing guarantees the terminator survived a corrupt image - bound the copy on both sides.
    snprintf(out.appElfSha256, sizeof(out.appElfSha256), "%.*s",
             static_cast<int>(sizeof(summary.app_elf_sha256)),
             reinterpret_cast<const char*>(summary.app_elf_sha256));

    // Best-effort: a dump can be perfectly valid without a recorded reason string (ESP_ERR_NOT_FOUND
    // here just means the panic didn't carry one), so a failure leaves panicReason empty rather
    // than failing the whole summary.
    if (esp_core_dump_get_panic_reason(out.panicReason, sizeof(out.panicReason)) != ESP_OK) {
        out.panicReason[0] = '\0';
    }
    return true;
#else
    (void)out;
    Log.warn("core dump summaries need CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH + _DATA_FORMAT_ELF");
    return false;
#endif
}

size_t CoreDumpService::readChunk(size_t offset, uint8_t* buffer, size_t length) const {
    if (buffer == nullptr || length == 0) {
        return 0;
    }

    size_t imageAddr = 0;
    size_t imageSize = 0;
    if (esp_core_dump_image_get(&imageAddr, &imageSize) != ESP_OK || offset >= imageSize) {
        return 0;
    }

    const esp_partition_t* partition = findCoreDumpPartition();
    if (partition == nullptr || imageAddr < partition->address) {
        return 0;
    }

    const size_t imageStart = imageAddr - partition->address;
    const size_t toRead = std::min(length, imageSize - offset);
    if (imageStart + offset + toRead > partition->size) {
        return 0;  // Would run past the partition - a corrupt length field, not a readable dump.
    }

    const esp_err_t err = esp_partition_read(partition, imageStart + offset, buffer, toRead);
    if (err != ESP_OK) {
        Log.printf("[ERROR] core dump flash read failed at offset %u (err=%d)\n",
                   static_cast<unsigned>(offset), static_cast<int>(err));
        return 0;
    }
    return toRead;
}

bool CoreDumpService::clear() const {
    const esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        Log.printf("[ERROR] failed to erase stored core dump (err=%d)\n", static_cast<int>(err));
        return false;
    }
    Log.info("Stored core dump erased");
    return true;
}

}  // namespace irrigator::drivers
