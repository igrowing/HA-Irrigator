#include <Arduino.h>
#include <ArduinoJson.h>
#include <Network.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include "board_config.h"
#include "domain/CalendarMath.h"
#include "domain/ConfigStore.h"
#include "domain/CurrentFaultMonitor.h"
#include "domain/IrrigationStateMachine.h"
#include "domain/RainSkip.h"
#include "domain/Schedule.h"
#include "domain/ScheduleEngine.h"
#include "domain/Settings.h"
#include "domain/WateringQueue.h"
#include "drivers/Clock.h"
#include "drivers/CurrentSensor.h"
#include "drivers/FactoryResetButton.h"
#include "drivers/FileStore.h"
#include "drivers/LedIndicator.h"
#include "drivers/Logger.h"
#include "drivers/RelayBank.h"
#include "network/CaptivePortalService.h"
#include "network/MqttService.h"
#include "network/OtaService.h"
#include "ui/IrrigatorUi.h"

using irrigator::board::PUMP_CURRENT_ADC_PIN;
using irrigator::board::STATUS_LED_PIN;
using irrigator::board::VALVE_CURRENT_ADC_PIN;
using irrigator::domain::RunReason;
using irrigator::domain::UiIntentType;
using irrigator::domain::WateringRequest;

namespace {

enum class AppMode { Booting, CaptivePortal, Operational };

AppMode appMode = AppMode::Booting;

irrigator::drivers::FileStore fileStore;
irrigator::drivers::RelayBank relayBank;
irrigator::drivers::CurrentSensor valveCurrentSensor(VALVE_CURRENT_ADC_PIN,
                                                     irrigator::board::VALVE_CURRENT_SENSOR_ZERO_OFFSET_MV,
                                                     irrigator::board::VALVE_CURRENT_SENSOR_MV_PER_MA);
irrigator::drivers::CurrentSensor pumpCurrentSensor(PUMP_CURRENT_ADC_PIN,
                                                    irrigator::board::PUMP_CURRENT_SENSOR_ZERO_OFFSET_MV,
                                                    irrigator::board::PUMP_CURRENT_SENSOR_MV_PER_MA);
irrigator::drivers::LedIndicator led(STATUS_LED_PIN);
irrigator::drivers::Clock clockService;
irrigator::drivers::FactoryResetButton factoryResetButton;

irrigator::domain::DeviceConfig deviceConfig;
irrigator::domain::Settings settings;
irrigator::domain::ScheduleList scheduleList;
irrigator::domain::WateringQueue<irrigator::board::WATERING_QUEUE_DEPTH> wateringQueue;
irrigator::domain::RainSkip rainSkip;
irrigator::domain::CurrentFaultMonitor valveFaultMonitor({irrigator::board::VALVE_CURRENT_MIN_MA,
                                                          irrigator::board::VALVE_CURRENT_MAX_MA});
irrigator::domain::CurrentFaultMonitor pumpFaultMonitor({irrigator::board::PUMP_CURRENT_MIN_MA,
                                                         irrigator::board::PUMP_CURRENT_MAX_MA});
irrigator::domain::IrrigationStateMachine irrigation;
irrigator::domain::ScheduleEngine scheduleEngine;

irrigator::network::CaptivePortalService captivePortal;
irrigator::network::MqttService mqtt;
irrigator::network::OtaService ota;
irrigator::ui::IrrigatorUi ui;

WateringRequest activeRequestForReporting;
bool reportedActive = false;
bool mqttBreathPlayed = false;
uint32_t lastWifiRetryMs = 0;
irrigator::domain::IrrigationState previousIrrigationState = irrigator::domain::IrrigationState::Idle;

bool valveWindowOpen = false;
uint32_t valveWindowCloseAtMs = 0;
bool valveFaultActive = false;
bool pumpWindowOpen = false;
uint32_t pumpWindowCloseAtMs = 0;
bool pumpFaultActive = false;

String makeDevicePrefix() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char suffix[3];
    snprintf(suffix, sizeof(suffix), "%02X", mac[5]);
    return "irrigator" + String(suffix);
}

const char* zoneName(uint8_t zone) {
    if (zone == 0) {
        return "pump";
    }
    if (zone >= 1 && zone <= irrigator::board::ZONE_COUNT) {
        return settings.zoneNames[zone - 1];
    }
    return "unknown";
}

// Pump-only requests (zone 0) aren't gated by this — "enabled" is a per-zone concept.
bool isZoneEnabled(uint8_t zone) {
    if (zone == 0) {
        return true;
    }
    if (zone >= 1 && zone <= irrigator::board::ZONE_COUNT) {
        return settings.zoneEnabled[zone - 1];
    }
    return false;
}

// Minimal ISO8601 "YYYY-MM-DDTHH:MM:SS" (UTC) -> epoch seconds. Ignores any trailing
// offset/'Z' suffix; HA is expected to publish a plain UTC timestamp for cmd/skip_rain_until.
int64_t parseIso8601Utc(const char* value) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) !=
        6) {
        return -1;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return -1;
    }

    return irrigator::domain::epochFromUtc(year,
                                           static_cast<unsigned>(month),
                                           static_cast<unsigned>(day),
                                           static_cast<unsigned>(hour),
                                           static_cast<unsigned>(minute),
                                           static_cast<unsigned>(second));
}

const char* bootReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:
            return "task_watchdog";
        case ESP_RST_WDT:
            return "other_watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "unknown";
    }
}

String buildStatusJson(uint32_t nowMs) {
    JsonDocument doc;
    doc["firmware_version"] = irrigator::board::FIRMWARE_VERSION;
    doc["boot_reason"] = bootReasonString(esp_reset_reason());
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime_s"] = nowMs / 1000UL;
    doc["free_heap_bytes"] = ESP.getFreeHeap();
    doc["free_storage_bytes"] = static_cast<uint32_t>(fileStore.freeBytes());
    doc["cpu_temp_c"] = temperatureRead();

    JsonObject outputs = doc["outputs"].to<JsonObject>();
    if (relayBank.isPumpOn()) {
        JsonObject pumpObj = outputs["pump"].to<JsonObject>();
        pumpObj["on"] = true;
        pumpObj["current_ma"] = pumpCurrentSensor.readMilliAmps();
    }
    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        if (relayBank.isZoneOn(zone)) {
            char key[8];
            snprintf(key, sizeof(key), "zone%u", static_cast<unsigned>(zone));
            JsonObject zoneObj = outputs[key].to<JsonObject>();
            zoneObj["on"] = true;
            zoneObj["current_ma"] = valveCurrentSensor.readMilliAmps();
        }
    }

    String payload;
    serializeJson(doc, payload);
    return payload;
}

void reportStop(RunReason reason) {
    if (!reportedActive) {
        return;
    }
    mqtt.publishState(activeRequestForReporting.zone,
                      zoneName(activeRequestForReporting.zone),
                      false,
                      false,
                      reason,
                      settings.scheduleActive,
                      0);
    reportedActive = false;
}

// The valve closes at the end of the requested duration, but the pump lingers for a further
// PUMP_GRACEFUL_SHUTDOWN_MS afterwards (protects it from rapid-cycle wear) — `isActive()` stays
// true for that whole tail, so the edge-triggered stop report in loop() (gated on !isActive())
// would otherwise fire ~a minute late relative to the requested duration. This fires the instant
// the valve itself closes, independent of the pump's graceful-shutdown tail.
void reportValveClosed() {
    if (!reportedActive) {
        return;
    }
    mqtt.publishState(activeRequestForReporting.zone,
                      zoneName(activeRequestForReporting.zone),
                      false,
                      true,
                      activeRequestForReporting.reason,
                      settings.scheduleActive,
                      0);
}

bool startIrrigation(const WateringRequest& request, uint32_t nowMs) {
    if (!irrigation.start(request, nowMs)) {
        return false;
    }
    activeRequestForReporting = request;
    reportedActive = true;
    mqtt.publishState(request.zone,
                      zoneName(request.zone),
                      request.zone != 0,
                      true,
                      request.reason,
                      settings.scheduleActive,
                      request.durationMin);
    return true;
}

void stopIrrigationExplicit(uint32_t nowMs) {
    if (!irrigation.isActive()) {
        return;
    }
    // Overwritten before abort() so every report from here on — the valve-closed edge below,
    // and the later final-stop report in loop() once the pump's graceful tail actually ends —
    // is tagged "abort" instead of silently reverting to the run's original start reason.
    activeRequestForReporting.reason = RunReason::Abort;
    irrigation.abort(nowMs);
    if (irrigation.isActive()) {
        // Zone+pump run: abort() gave the pump the same graceful-shutdown tail a natural
        // end-of-duration completion gets (see IrrigationStateMachine::abort()), so report the
        // valve-closed edge now — same shape/timing as reportValveClosed()'s natural-end use in
        // loop() — and let the later reportedActive && !isActive() check there fire the final
        // stop report once the pump actually finishes winding down.
        reportValveClosed();
    } else {
        // PumpOnly, or an abort landing mid-graceful-shutdown-tail: nothing left to protect,
        // stops immediately — report the full stop right now.
        reportStop(RunReason::Abort);
    }
}

bool persistSettings() {
    char buffer[irrigator::board::SETTINGS_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSettings(settings, buffer, sizeof(buffer));
    if (written == 0) {
        return false;
    }
    return fileStore.writeTextWithBackup(irrigator::board::SETTINGS_FILE_PATH, String(buffer));
}

void applyCurrentCalibration() {
    valveCurrentSensor.setCalibration(settings.valveCurrentZeroOffsetMv, settings.valveCurrentMvPerMa);
    pumpCurrentSensor.setCalibration(settings.pumpCurrentZeroOffsetMv, settings.pumpCurrentMvPerMa);
}

// Every MQTT command/topic failure (malformed payload, out-of-range value, unknown command,
// missing file, etc.) goes through here: one consistent, structured alert plus a matching log
// line, rather than each call site deciding independently whether/how to report a problem.
void reportMqttError(const char* context, const String& detail, const char* reason) {
    char alertMsg[192];
    snprintf(alertMsg, sizeof(alertMsg), R"({"%s":"%s","error":"%s"})", context, detail.c_str(), reason);
    mqtt.publishAlert(alertMsg);
    irrigator::drivers::Log.printf("[WARN] %s %s: %s\n", context, detail.c_str(), reason);
}

// `cmd/download/<filename>` handler: filename comes from the topic (payload is unused, "" per
// spec), response goes to `<prefix>/download/<filename>`.
void handleMqttDownload(const String& filename) {
    if (filename.length() == 0) {
        reportMqttError("download", filename, "empty filename");
        return;
    }

    String path = filename;
    path.trim();
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    if (!fileStore.exists(path.c_str())) {
        reportMqttError("download", filename, "not found");
        return;
    }
    String content;
    if (!fileStore.readText(path.c_str(), content)) {
        reportMqttError("download", filename, "failed to read");
        return;
    }

    mqtt.publishDownload(filename, content);
}

// `cmd/upload/<filename>` handler: payload is the new file content. config.json/settings.json/
// schedule.json are parsed and validated (reject-whole-file-on-invalid, same as loading them
// from flash at boot) before being written and applied live; any other filename is written
// as-is since there's no schema to validate it against. A successful config.json upload
// restarts the device (new Wi-Fi/MQTT credentials only take effect from enterOperationalMode()
// at boot).
void handleMqttUpload(const String& filename, const uint8_t* payload, size_t length) {
    if (filename.length() == 0) {
        reportMqttError("upload", filename, "empty filename");
        return;
    }

    String path = filename;
    path.trim();
    if (!path.startsWith("/")) {
        path = "/" + path;
    }
    const String content(reinterpret_cast<const char*>(payload), length);

    if (path == irrigator::board::CONFIG_FILE_PATH) {
        irrigator::domain::DeviceConfig parsed;
        if (!irrigator::domain::parseConfig(content.c_str(), content.length(), parsed)) {
            reportMqttError("upload", filename, "invalid config.json content");
            return;
        }
        if (!fileStore.writeTextWithBackup(path.c_str(), content)) {
            reportMqttError("upload", filename, "failed to write");
            return;
        }
        irrigator::drivers::Log.info("config.json updated via MQTT upload; restarting");
        mqtt.publishAvailability(false);
        delay(200);
        ESP.restart();
        return;
    }

    if (path == irrigator::board::SETTINGS_FILE_PATH) {
        irrigator::domain::Settings parsed;
        if (!irrigator::domain::parseSettings(content.c_str(), content.length(), parsed)) {
            reportMqttError("upload", filename, "invalid settings.json content");
            return;
        }
        if (!fileStore.writeTextWithBackup(path.c_str(), content)) {
            reportMqttError("upload", filename, "failed to write");
            return;
        }
        settings = parsed;
        applyCurrentCalibration();
        return;
    }

    if (path == irrigator::board::SCHEDULE_FILE_PATH) {
        irrigator::domain::ScheduleList parsed;
        if (!irrigator::domain::parseSchedule(content.c_str(), content.length(), parsed)) {
            reportMqttError("upload", filename, "invalid schedule.json content");
            return;
        }
        if (!fileStore.writeTextWithBackup(path.c_str(), content)) {
            reportMqttError("upload", filename, "failed to write");
            return;
        }
        scheduleList = parsed;
        mqtt.publishSchedule(content);
        return;
    }

    // Unrecognized filename: raw passthrough, there's no schema to validate it against.
    if (!fileStore.writeTextWithBackup(path.c_str(), content)) {
        reportMqttError("upload", filename, "failed to write");
    }
}

// `cmd/state` handler: publishes the current watering/pump status on demand, independent of
// the edge-triggered publishes that fire on every start/stop.
void publishCurrentState() {
    const auto snapshot = irrigation.snapshot(millis());
    const bool valveOn = snapshot.state == irrigator::domain::IrrigationState::Watering;
    const RunReason reason = reportedActive ? activeRequestForReporting.reason : RunReason::Manual;
    const uint16_t remainingMin = static_cast<uint16_t>(snapshot.remainingMs / 60000UL);
    mqtt.publishState(snapshot.activeZone,
                      zoneName(snapshot.activeZone),
                      valveOn,
                      snapshot.pumpOn,
                      reason,
                      settings.scheduleActive,
                      remainingMin);
}

void persistScheduleAndReport() {
    char buffer[irrigator::board::SCHEDULE_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSchedule(scheduleList, buffer, sizeof(buffer));
    if (written == 0) {
        return;
    }
    fileStore.writeTextWithBackup(irrigator::board::SCHEDULE_FILE_PATH, String(buffer));
    mqtt.publishSchedule(buffer);
}

// Nearest future occurrence across the week, for Screen 1's "next zone to be active" display —
// a UI-only nicety, not part of the actual scheduling decision (that's ScheduleEngine).
uint8_t findNextScheduledZone(const struct tm& nowLocal) {
    if (scheduleList.count == 0) {
        return 0;
    }
    const uint8_t nowIsoDay = nowLocal.tm_wday == 0 ? 7 : static_cast<uint8_t>(nowLocal.tm_wday);
    const int nowMinutes = nowLocal.tm_hour * 60 + nowLocal.tm_min;

    int bestDelta = -1;
    uint8_t bestZone = 0;
    for (uint8_t i = 0; i < scheduleList.count; ++i) {
        const auto& record = scheduleList.records[i];
        if (!record.active) {
            continue;
        }
        const int hour = (record.startTime[0] - '0') * 10 + (record.startTime[1] - '0');
        const int minute = (record.startTime[3] - '0') * 10 + (record.startTime[4] - '0');
        int dayDelta = record.dayOfWeek - nowIsoDay;
        if (dayDelta < 0) {
            dayDelta += 7;
        }
        int totalDelta = dayDelta * 1440 + ((hour * 60 + minute) - nowMinutes);
        if (totalDelta < 0) {
            totalDelta += 7 * 1440;
        }
        if (bestDelta < 0 || totalDelta < bestDelta) {
            bestDelta = totalDelta;
            bestZone = record.zone;
        }
    }
    return bestZone;
}

// `cmd/queue` handler and change notifications: publishes the current FIFO contents to
// `/queue`, called on demand and on every push/pop so subscribers stay in sync.
void publishQueueState() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < wateringQueue.size(); ++i) {
        const auto& item = wateringQueue.at(i);
        JsonObject obj = arr.add<JsonObject>();
        obj["zone"] = item.zone;
        obj["duration"] = item.durationMin;
    }
    String payload;
    serializeJson(doc, payload);
    mqtt.publishQueue(payload);
}

// Single choke point for advancing the queue: pops and starts the next request iff the
// irrigation state machine can currently accept one (Idle, or PumpGracePeriod for a seamless
// hand-off — see IrrigationStateMachine::canStartImmediately()). A no-op otherwise, so every
// call site below can call this unconditionally after anything that might have freed up room —
// a push, an abort, or a natural state transition — instead of each duplicating the
// pop-report-start sequence itself.
void pumpQueue(uint32_t nowMs) {
    if (!irrigation.canStartImmediately()) {
        return;
    }
    WateringRequest next;
    if (wateringQueue.pop(next)) {
        publishQueueState();
        startIrrigation(next, nowMs);
    }
}

// Every watering/pumping request — manual, scheduled, or MQTT — funnels through the queue
// first, even when it can start immediately: push, report, then (if idle) pop it straight back
// off and start it. That guarantees a push report and a pull report for every request, not just
// the ones that have to wait behind an active run.
void enqueueOrStart(const WateringRequest& request, uint32_t nowMs) {
    if (!isZoneEnabled(request.zone)) {
        char alertMsg[64];
        snprintf(alertMsg,
                sizeof(alertMsg),
                "Zone %u (%s) is disabled. Enable it in settings first.",
                static_cast<unsigned>(request.zone),
                zoneName(request.zone));
        mqtt.publishAlert(alertMsg);
        return;
    }
    if (!wateringQueue.push(request)) {
        char alertMsg[112];
        snprintf(alertMsg,
                sizeof(alertMsg),
                "Watering queue is full. Dropping watering: zone=%u duration=%u",
                static_cast<unsigned>(request.zone),
                static_cast<unsigned>(request.durationMin));
        mqtt.publishAlert(alertMsg);
        return;
    }
    publishQueueState();
    pumpQueue(nowMs);
}

void handleUiIntent(const irrigator::domain::UiIntent& intent, uint32_t nowMs) {
    switch (intent.type) {
        case UiIntentType::StartWatering: {
            WateringRequest request;
            request.zone = intent.zone;
            request.durationMin = intent.duration;
            request.reason = RunReason::Manual;
            enqueueOrStart(request, nowMs);
            break;
        }
        case UiIntentType::AbortWatering:
            stopIrrigationExplicit(nowMs);
            // Same as a plain (non-"all") MQTT cmd/stop: aborting one run hands off to whatever's
            // next in the queue rather than leaving it stranded — "Stop all" (StopAllWatering,
            // below) is the dedicated action for wiping the queue instead.
            pumpQueue(nowMs);
            break;
        case UiIntentType::StopAllWatering:
            // Unlike AbortWatering above, this deliberately does not hand off to the queue
            // afterward — the queue is empty by the time stopIrrigationExplicit() reports the
            // stop, so there's nothing left to pump.
            stopIrrigationExplicit(nowMs);
            wateringQueue.clear();
            publishQueueState();
            break;
        case UiIntentType::ToggleScheduleActive:
            settings.scheduleActive = intent.boolValue;
            persistSettings();
            mqtt.publishScheduled(settings.scheduleActive);
            break;
        case UiIntentType::SetWateringPercentage: {
            irrigator::domain::Settings updated = settings;
            updated.wateringPercentage = intent.percentage;
            if (irrigator::domain::validateSettings(updated)) {
                settings = updated;
                persistSettings();
            }
            break;
        }
        case UiIntentType::SetScheduleZone:
            if (intent.scheduleRecordIndex < scheduleList.count) {
                irrigator::domain::ScheduleRecord updated =
                    scheduleList.records[intent.scheduleRecordIndex];
                updated.zone = intent.zone;
                if (irrigator::domain::validateScheduleRecord(updated)) {
                    scheduleList.records[intent.scheduleRecordIndex] = updated;
                    persistScheduleAndReport();
                }
            }
            break;
        case UiIntentType::SetScheduleDay:
            if (intent.scheduleRecordIndex < scheduleList.count) {
                irrigator::domain::ScheduleRecord updated =
                    scheduleList.records[intent.scheduleRecordIndex];
                updated.dayOfWeek = intent.dayOfWeek;
                if (irrigator::domain::validateScheduleRecord(updated)) {
                    scheduleList.records[intent.scheduleRecordIndex] = updated;
                    persistScheduleAndReport();
                }
            }
            break;
        case UiIntentType::SetScheduleTime:
            if (intent.scheduleRecordIndex < scheduleList.count) {
                irrigator::domain::ScheduleRecord updated =
                    scheduleList.records[intent.scheduleRecordIndex];
                snprintf(updated.startTime,
                        sizeof(updated.startTime),
                        "%02u:%02u",
                        static_cast<unsigned>(intent.hour),
                        static_cast<unsigned>(intent.minute));
                if (irrigator::domain::validateScheduleRecord(updated)) {
                    scheduleList.records[intent.scheduleRecordIndex] = updated;
                    persistScheduleAndReport();
                }
            }
            break;
        case UiIntentType::SetScheduleDuration:
            if (intent.scheduleRecordIndex < scheduleList.count) {
                scheduleList.records[intent.scheduleRecordIndex].durationMin = intent.duration;
                persistScheduleAndReport();
            }
            break;
        case UiIntentType::SetScheduleActive:
            if (intent.scheduleRecordIndex < scheduleList.count) {
                scheduleList.records[intent.scheduleRecordIndex].active = intent.boolValue;
                persistScheduleAndReport();
            }
            break;
        case UiIntentType::RunAllZonesTest:
            for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
                if (!settings.zoneEnabled[zone - 1]) {
                    continue;
                }
                WateringRequest request;
                request.zone = zone;
                request.durationMin = intent.duration;
                request.reason = RunReason::Manual;
                enqueueOrStart(request, nowMs);
            }
            break;
        case UiIntentType::FactoryReset: {
            irrigator::drivers::Log.warn("Factory reset requested (10s BOOT-button hold, confirmed)");
            const bool configRemoved = fileStore.remove(irrigator::board::CONFIG_FILE_PATH);
            const bool settingsRemoved = fileStore.remove(irrigator::board::SETTINGS_FILE_PATH);
            const bool scheduleRemoved = fileStore.remove(irrigator::board::SCHEDULE_FILE_PATH);
            // Best-effort: a stray .bak left behind by writeTextWithBackup() doesn't affect the
            // success/failure verdict below, which only tracks the primary files.
            fileStore.remove((String(irrigator::board::CONFIG_FILE_PATH) + ".bak").c_str());
            fileStore.remove((String(irrigator::board::SETTINGS_FILE_PATH) + ".bak").c_str());
            fileStore.remove((String(irrigator::board::SCHEDULE_FILE_PATH) + ".bak").c_str());

            const bool success = configRemoved && settingsRemoved && scheduleRemoved;
            ui.showFactoryResetResult(success, nowMs);
            mqtt.publishAvailability(false);
            delay(irrigator::board::FACTORY_RESET_RESULT_DISPLAY_MS);
            ESP.restart();
            break;
        }
        case UiIntentType::None:
        default:
            break;
    }
}

void handleMqttCommand(const String& commandName, const uint8_t* payload, size_t length, uint32_t nowMs) {
    const String body(reinterpret_cast<const char*>(payload), length);

    if (commandName == "state") {
        publishCurrentState();
        return;
    }

    if (commandName == "queue") {
        publishQueueState();
        return;
    }

    if (commandName == "status") {
        mqtt.publishStatus(buildStatusJson(nowMs));
        return;
    }

    if (commandName.startsWith("download/")) {
        handleMqttDownload(commandName.substring(String("download/").length()));
        return;
    }

    if (commandName.startsWith("upload/")) {
        handleMqttUpload(commandName.substring(String("upload/").length()), payload, length);
        return;
    }

    if (commandName == "scheduled") {
        String trimmed = body;
        trimmed.trim();
        bool newValue;
        if (trimmed == "true" || trimmed == "1") {
            newValue = true;
        } else if (trimmed == "false" || trimmed == "0") {
            newValue = false;
        } else {
            reportMqttError("command", commandName, "invalid bool payload, expected true/false");
            return;
        }
        if (settings.scheduleActive != newValue) {
            settings.scheduleActive = newValue;
            persistSettings();
        }
        mqtt.publishScheduled(settings.scheduleActive);
        return;
    }

    if (commandName == "watering_percentage") {
        if (body.length() == 0) {
            reportMqttError("command", commandName, "empty payload, expected an integer");
            return;
        }
        irrigator::domain::Settings updated = settings;
        updated.wateringPercentage = static_cast<int16_t>(body.toInt());
        if (!irrigator::domain::validateSettings(updated)) {
            reportMqttError("command", commandName, "percentage out of range (0-300)");
            return;
        }
        settings = updated;
        persistSettings();
        mqtt.publishWateringPercentage(settings.wateringPercentage);
        return;
    }

    if (commandName == "run") {
        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            reportMqttError("command", commandName, "invalid JSON payload");
            return;
        }
        WateringRequest request;
        request.zone = doc["zone"] | 0;
        request.durationMin = doc["duration"] | 0;
        request.reason = RunReason::Mqtt;
        if (request.zone > irrigator::board::ZONE_COUNT) {
            reportMqttError("command", commandName, "zone out of range (0-11)");
            return;
        }
        if (request.durationMin == 0 || request.durationMin > irrigator::board::MANUAL_DURATION_MAX_MINUTES) {
            reportMqttError("command", commandName, "duration out of range (1-120)");
            return;
        }

        enqueueOrStart(request, nowMs);
        return;
    }

    if (commandName == "stop") {
        String trimmed = body;
        trimmed.trim();
        stopIrrigationExplicit(nowMs);
        if (trimmed == "all") {
            wateringQueue.clear();
            publishQueueState();
        } else {
            pumpQueue(nowMs);
        }
        return;
    }

    if (commandName == "skip_rain_until") {
        String trimmed = body;
        trimmed.trim();
        const int64_t epoch = parseIso8601Utc(trimmed.c_str());
        if (epoch <= 0) {
            reportMqttError("command", commandName, "invalid or missing timestamp");
            return;
        }
        rainSkip.setSkipUntil(epoch);
        mqtt.publishSkipRainUntil(trimmed);
        return;
    }

    if (commandName == "restart") {
        mqtt.publishAvailability(false);
        delay(200);
        ESP.restart();
        return;
    }

    if (commandName == "ota") {
        if (body.length() == 0) {
            reportMqttError("command", commandName, "empty payload, expected a firmware URL");
            return;
        }
        ota.beginUpdateFromUrl(body.c_str());
        return;
    }

    if (commandName == "set_calibration") {
        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            reportMqttError("command", commandName, "invalid JSON payload");
            return;
        }
        irrigator::domain::Settings updated = settings;
        updated.valveCurrentZeroOffsetMv = doc["valve_zero_mv"] | settings.valveCurrentZeroOffsetMv;
        updated.valveCurrentMvPerMa = doc["valve_mv_per_ma"] | settings.valveCurrentMvPerMa;
        updated.pumpCurrentZeroOffsetMv = doc["pump_zero_mv"] | settings.pumpCurrentZeroOffsetMv;
        updated.pumpCurrentMvPerMa = doc["pump_mv_per_ma"] | settings.pumpCurrentMvPerMa;
        if (!irrigator::domain::validateSettings(updated)) {
            reportMqttError("command", commandName, "calibration value out of range");
            return;
        }
        settings = updated;
        persistSettings();
        applyCurrentCalibration();
        return;
    }

    reportMqttError("command", commandName, "unknown command");
}

bool loadOperationalConfig() {
    String raw;
    if (!fileStore.readText(irrigator::board::CONFIG_FILE_PATH, raw)) {
        return false;
    }
    return irrigator::domain::parseConfig(raw.c_str(), raw.length(), deviceConfig);
}

void loadOrCreateSettings() {
    String raw;
    if (fileStore.exists(irrigator::board::SETTINGS_FILE_PATH) &&
        fileStore.readText(irrigator::board::SETTINGS_FILE_PATH, raw) &&
        irrigator::domain::parseSettings(raw.c_str(), raw.length(), settings)) {
        return;
    }

    irrigator::drivers::Log.warn("settings.json missing or invalid; writing defaults");
    settings = irrigator::domain::makeDefaultZones();
    persistSettings();
}

void loadSchedule() {
    if (!fileStore.exists(irrigator::board::SCHEDULE_FILE_PATH)) {
        scheduleList = {};
        return;
    }

    String raw;
    if (!fileStore.readText(irrigator::board::SCHEDULE_FILE_PATH, raw) ||
        !irrigator::domain::parseSchedule(raw.c_str(), raw.length(), scheduleList)) {
        irrigator::drivers::Log.error("schedule.json invalid; marking schedule inactive");
        scheduleList = {};
        settings.scheduleActive = false;
    }
}

void connectWifiOnce() {
    WiFi.mode(WIFI_STA);
    // Disable Wi-Fi modem sleep: costs more average current, but avoids the periodic
    // beacon-interval current spikes that were showing up as a bursty load on the 3.3V rail.
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.begin(deviceConfig.ssid, deviceConfig.password);
    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000UL) {
        delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
        irrigator::drivers::Log.warn("Initial Wi-Fi connect failed; retrying in background");
    }
}

void enterCaptivePortalMode() {
    appMode = AppMode::CaptivePortal;
    const String apSsid = makeDevicePrefix();
    captivePortal.begin(apSsid, fileStore);
    led.setMode(irrigator::drivers::LedMode::CaptivePortal);
    ui.showCaptivePortal(apSsid, IPAddress(192, 168, 4, 1));
}

void enterOperationalMode() {
    appMode = AppMode::Operational;

    loadOrCreateSettings();
    applyCurrentCalibration();
    loadSchedule();
    connectWifiOnce();

    led.setMode(irrigator::drivers::LedMode::Off);
    clockService.begin(deviceConfig.timezoneOffsetHours);
    clockService.restoreFromRtc();  // Fallback baseline in case NTP takes a while (or never syncs).
    clockService.syncFromNtp();     // Bounded wait; writes the result back to the DS3231 on success.

    irrigator::network::MqttConfig mqttConfig;
    mqttConfig.broker = deviceConfig.mqttBroker;
    mqttConfig.port = deviceConfig.mqttPort;
    mqttConfig.user = deviceConfig.mqttUser;
    mqttConfig.password = deviceConfig.mqttPassword;
    mqttConfig.prefix = makeDevicePrefix();
    mqtt.setCommandHandler(handleMqttCommand);
    mqtt.setNowEpochProvider([]() { return static_cast<int64_t>(clockService.epoch()); });
    mqtt.begin(mqttConfig);

    ota.setResultHandler([](bool success, const char* message) {
        char alertMsg[128];
        snprintf(alertMsg, sizeof(alertMsg), R"({"ota":"%s","message":"%s"})",
                success ? "ok" : "failed", message);
        mqtt.publishAlert(alertMsg);
        if (success) {
            char iso[24];
            irrigator::domain::formatIso8601Utc(clockService.epoch(), iso, sizeof(iso));
            mqtt.publishOtaCompleted(iso);
        }
    });

    // Armed last, with a fresh timestamp: connectWifiOnce()/syncFromNtp() above block for up to
    // ~15-20s with loop() (and so ui.poll()) not running yet. Arming the 20s active-interaction
    // timeout before that blocking work — as this used to — could burn most or all of the window
    // before the user ever saw the screen, so the first real long-click after boot would land
    // exactly when the timeout silently expired, get swallowed as a mere "wake from display-off"
    // instead of opening the menu, and only the next attempt would actually work.
    ui.wake(millis());
}

void runScheduleEngine(uint32_t nowMs) {
    struct tm timeinfo;
    if (!clockService.now(timeinfo)) {
        return;  // Clock not synced yet; nothing to evaluate against.
    }

    irrigator::domain::WallClock wallClock;
    wallClock.isoDayOfWeek = timeinfo.tm_wday == 0 ? 7 : static_cast<uint8_t>(timeinfo.tm_wday);
    wallClock.hour = static_cast<uint8_t>(timeinfo.tm_hour);
    wallClock.minute = static_cast<uint8_t>(timeinfo.tm_min);

    WateringRequest dueRequests[irrigator::board::SCHEDULE_MAX_RECORDS];
    const uint8_t dueCount = scheduleEngine.tick(wallClock,
                                                 clockService.epoch(),
                                                 scheduleList,
                                                 settings.scheduleActive,
                                                 rainSkip,
                                                 settings.wateringPercentage,
                                                 dueRequests,
                                                 irrigator::board::SCHEDULE_MAX_RECORDS);
    for (uint8_t i = 0; i < dueCount; ++i) {
        enqueueOrStart(dueRequests[i], nowMs);
    }
}

// Measurement windows per spec: sampling for a given output starts the moment it's commanded on
// and keeps running until board::CURRENT_MEASURE_POST_OFF_MS after it turns off. Fault alerting
// (via CurrentFaultMonitor's begin/endActiveCycle bracket) tracks the same window, which is also
// what gives the "only once per cycle, until the alert clears" de-duplication for free.
void updateCurrentMonitoring(uint32_t nowMs) {
    const auto snapshot = irrigation.snapshot(nowMs);
    const bool valveOn = snapshot.state == irrigator::domain::IrrigationState::Watering;
    const bool pumpOn = snapshot.pumpOn;

    if (valveOn) {
        if (!valveWindowOpen) {
            valveWindowOpen = true;
            valveFaultMonitor.beginActiveCycle();
        }
        valveWindowCloseAtMs = nowMs + irrigator::board::CURRENT_MEASURE_POST_OFF_MS;

        const uint16_t currentMa = valveCurrentSensor.readMilliAmps();
        if (valveFaultMonitor.checkAndConsumeAlert(currentMa)) {
            valveFaultActive = true;
            char alertMsg[80];
            snprintf(alertMsg,
                    sizeof(alertMsg),
                    R"({"valve":%u,"current":%u,"units":"mA"})",
                    static_cast<unsigned>(snapshot.activeZone),
                    static_cast<unsigned>(currentMa));
            mqtt.publishAlert(alertMsg);
        }
    } else if (valveWindowOpen && nowMs >= valveWindowCloseAtMs) {
        valveWindowOpen = false;
        valveFaultActive = false;
        valveFaultMonitor.endActiveCycle();
    }

    if (pumpOn) {
        if (!pumpWindowOpen) {
            pumpWindowOpen = true;
            pumpFaultMonitor.beginActiveCycle();
        }
        pumpWindowCloseAtMs = nowMs + irrigator::board::CURRENT_MEASURE_POST_OFF_MS;

        const uint16_t currentMa = pumpCurrentSensor.readMilliAmps();
        if (pumpFaultMonitor.checkAndConsumeAlert(currentMa)) {
            pumpFaultActive = true;
            char alertMsg[64];
            snprintf(alertMsg,
                    sizeof(alertMsg),
                    R"({"pump":0,"current":%u,"units":"mA"})",
                    static_cast<unsigned>(currentMa));
            mqtt.publishAlert(alertMsg);
        }
    } else if (pumpWindowOpen && nowMs >= pumpWindowCloseAtMs) {
        pumpWindowOpen = false;
        pumpFaultActive = false;
        pumpFaultMonitor.endActiveCycle();
    }
}

void updateOutputsFromDomain() {
    const auto snapshot = irrigation.snapshot(millis());
    relayBank.setPump(snapshot.pumpOn);

    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        const bool zoneOn = snapshot.state == irrigator::domain::IrrigationState::Watering &&
                            snapshot.activeZone == zone;
        relayBank.setZone(zone, zoneOn);
    }

    if (valveFaultActive || pumpFaultActive) {
        led.setMode(irrigator::drivers::LedMode::Alert);
    } else if (snapshot.pumpOn) {
        led.setMode(irrigator::drivers::LedMode::SolidOn);
    } else if (led.mode() == irrigator::drivers::LedMode::SolidOn ||
              led.mode() == irrigator::drivers::LedMode::Alert) {
        led.setMode(irrigator::drivers::LedMode::Off);
    }
}

}  // namespace

void setup() {
    irrigator::drivers::Log.begin(115200);
    irrigator::drivers::Log.printf("Irrigator firmware starting v.%s\n", APP_VERSION);

    Wire.begin(irrigator::board::I2C_SDA_PIN, irrigator::board::I2C_SCL_PIN);
    Wire.setClock(irrigator::board::I2C_CLOCK_HZ);

    analogReadResolution(irrigator::board::ADC_RESOLUTION_BITS);
    analogSetAttenuation(ADC_11db);

    fileStore.begin();
    relayBank.begin();
    valveCurrentSensor.begin();
    pumpCurrentSensor.begin();
    led.begin();
    ui.begin();
    factoryResetButton.begin();

    if (!fileStore.exists(irrigator::board::CONFIG_FILE_PATH) || !loadOperationalConfig()) {
        enterCaptivePortalMode();
        return;
    }

    enterOperationalMode();
}

void loop() {
    const uint32_t nowMs = millis();
    led.update(nowMs);

    if (ota.inProgress()) {
        led.setMode(irrigator::drivers::LedMode::Ota);
        return;
    }

    switch (appMode) {
        case AppMode::CaptivePortal:
            captivePortal.loop();
            break;
        case AppMode::Operational:
            if (WiFi.status() != WL_CONNECTED) {
                if (nowMs - lastWifiRetryMs > 30000UL) {
                    lastWifiRetryMs = nowMs;
                    irrigator::drivers::Log.warn("Wi-Fi disconnected; attempting reconnect");
                    WiFi.reconnect();
                }
            } else if (!mqttBreathPlayed && mqtt.connected()) {
                mqttBreathPlayed = true;
                led.setMode(irrigator::drivers::LedMode::BreathOnce);
            }

            mqtt.loop(nowMs);
            if (mqtt.consumeBootStatusDue(nowMs)) {
                mqtt.publishStatus(buildStatusJson(nowMs));
            }

            if (factoryResetButton.poll()) {
                ui.confirmFactoryReset(nowMs);
            }

            runScheduleEngine(nowMs);

            irrigation.tick(nowMs);
            if (previousIrrigationState == irrigator::domain::IrrigationState::Watering &&
                irrigation.snapshot(nowMs).state == irrigator::domain::IrrigationState::PumpGracePeriod) {
                reportValveClosed();
                // Seamless hand-off: the pump is still running its graceful-shutdown tail, so a
                // queued item can start right now instead of waiting for that tail to finish —
                // this is the whole point of the pump lingering rather than shutting off.
                pumpQueue(nowMs);
            }
            previousIrrigationState = irrigation.snapshot(nowMs).state;

            if (reportedActive && !irrigation.isActive()) {
                reportStop(activeRequestForReporting.reason);
                // Pump-only runs have no graceful-shutdown tail (no valve to protect), so this
                // is their only hand-off point; it also acts as a safety net for the zone-chain
                // case above if nothing was queued in time.
                pumpQueue(nowMs);
            }
            // updateCurrentMonitoring(nowMs);  // Disabled for now, until measurement circuit is tested in HW and calibrated..
            updateOutputsFromDomain();

            {
                irrigator::ui::RenderData renderData;
                renderData.snapshot = irrigation.snapshot(nowMs);
                renderData.settings = &settings;
                renderData.schedule = &scheduleList;
                renderData.scheduleActive = settings.scheduleActive;
                renderData.deviceIp = WiFi.localIP();
                renderData.mqttBrokerText = deviceConfig.mqttBroker;
                renderData.mqttTopicText = mqtt.prefix();

                struct tm wallTm;
                renderData.hasWallClock = clockService.now(wallTm);
                if (renderData.hasWallClock) {
                    renderData.wallClockLocal = wallTm;
                    renderData.nextScheduledZone = findNextScheduledZone(wallTm);
                    snprintf(renderData.nextScheduledZoneName,
                            sizeof(renderData.nextScheduledZoneName),
                            "%u.%s", 
                            renderData.nextScheduledZone,
                            zoneName(renderData.nextScheduledZone));
                }

                const irrigator::domain::UiIntent uiIntent = ui.poll(nowMs, renderData);
                handleUiIntent(uiIntent, nowMs);
            }
            break;
        case AppMode::Booting:
        default:
            break;
    }
}