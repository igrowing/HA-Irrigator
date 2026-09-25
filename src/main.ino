#include <Arduino.h>
#include <ArduinoJson.h>
#include <Network.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_rom_crc.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include <memory>
#include <new>
#include <string.h>

#include "board_config.h"
#include "domain/AutoAdjustment.h"
#include "domain/CalendarMath.h"
#include "domain/ConfigStore.h"
#include "domain/DstRules.h"
#include "domain/FlowConsumptionTracker.h"
#include "domain/FlowPublishGate.h"
#include "domain/IrrigationStateMachine.h"
#include "domain/RainSkip.h"
#include "domain/Schedule.h"
#include "domain/ScheduleEngine.h"
#include "domain/Settings.h"
#include "domain/WateringQueue.h"
#include "domain/WifiQuality.h"
#include "drivers/Clock.h"
#include "drivers/CoreDumpService.h"
#include "drivers/CurrentSensor.h"
#include "drivers/FactoryResetButton.h"
#include "drivers/FileStore.h"
#include "drivers/LedIndicator.h"
#include "drivers/Logger.h"
#include "drivers/RelayBank.h"
#include "drivers/Tuf2000mFlowMeter.h"
#include "network/CaptivePortalService.h"
#include "network/MqttService.h"
#include "network/OtaService.h"
#include "network/WeatherService.h"
#include "ui/IrrigatorUi.h"

// Overrides the ESP32 Arduino core's weak getArduinoLoopTaskStackSize() (default 8192, see the
// framework's cores/esp32/main.cpp). setup(), loop(), and everything reachable from them — the UI,
// the whole MQTT command dispatch, file I/O, and critically the blocking OTA flash — all share this
// one stack. 8KB was measurably too tight: handleMqttCommand() alone reserves ~1.4KB (was ~4KB
// before the ScheduleList/settings-buffer fixes below), loop() ~1.5KB, and OTA then piles
// HTTPClient + Update + the IDF flash-write path (which disables the flash cache and whose depth
// isn't ours to control) on top of that — the suspected cause of the ESP_RST_PANIC on every OTA
// attempt that no amount of promiscuous-mode fixing cured. Costs 8KB of heap once at boot, against
// ~180KB free; a stack-canary panic mid-flash-write costs a site visit with a laptop.
SET_LOOP_TASK_STACK_SIZE(16384);

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
irrigator::drivers::CurrentSensor valveCurrentSensor(VALVE_CURRENT_ADC_PIN);
irrigator::drivers::CurrentSensor pumpCurrentSensor(PUMP_CURRENT_ADC_PIN);
// TUF-2000M flow meter over Modbus RTU/RS-485 - see src/drivers/Tuf2000mFlowMeter.h and
// board_config.h's FLOW_METER_* constants (pins, baud, register map). Serial2 is remapped onto
// FLOW_METER_RX_PIN/FLOW_METER_TX_PIN by the driver's begin() - its default pins (16/17) are
// already claimed by zones (see GPIO_ZONE_PINS).
irrigator::drivers::Tuf2000mFlowMeter flowMeter;
irrigator::drivers::LedIndicator led(STATUS_LED_PIN);
irrigator::drivers::Clock clockService;
irrigator::drivers::FactoryResetButton factoryResetButton;
irrigator::drivers::CoreDumpService coreDumpService;

// Snapshot taken once in setup(). A stored dump can only appear via a crash + reboot, so this
// stays true for the whole session — the one thing that can change it mid-run is cmd/coredump_clear
// erasing the dump, which updates it. Cached because CoreDumpService::available() re-reads and
// re-CRC32s the entire (up to 64KB) image on every call, which has no business happening on the
// buildStatusJson()/loop() path.
bool coreDumpPresent = false;

irrigator::domain::DeviceConfig deviceConfig;
irrigator::domain::Settings settings;
irrigator::domain::ScheduleList scheduleList;
irrigator::domain::WateringQueue<irrigator::board::WATERING_QUEUE_DEPTH> wateringQueue;
irrigator::domain::RainSkip rainSkip;
irrigator::domain::IrrigationStateMachine irrigation;
irrigator::domain::ScheduleEngine scheduleEngine;

irrigator::network::CaptivePortalService captivePortal;
irrigator::network::MqttService mqtt;
irrigator::network::OtaService ota;
irrigator::network::WeatherService weatherService;
irrigator::ui::IrrigatorUi ui;

// Weather-based auto-adjustment retry pacing — in-RAM only, a reboot just
// means "allow an immediate retry" rather than waiting out whatever interval was left.
uint32_t lastGeoAttemptMs = 0;
bool geoAttempted = false;
uint32_t lastWeatherAttemptMs = 0;
bool weatherAttempted = false;
// Deliberately in-RAM only, not settings.json — a power cycle should re-run today's cycle rather
// than remember it already happened (see the comment on Settings.h's field list for why).
char lastWeatherUpdateDate[11] = {};  // "YYYY-MM-DD", empty until this session's first success.
int64_t lastWeatherUpdateEpoch = 0;  // Exact moment of the above; 0 until this session's first success.
// Separate from lastWeatherUpdateDate: this one tracks the last day a failure alert went out, so
// the uncapped 5-min retry loop (see runAutoAdjustmentIfDue) doesn't spam /alert on every attempt.
char lastWeatherFailureAlertDate[11] = {};
// Same idea, for tryGeolocateIfDue's retry loop — geolocation gates the whole weather pipeline
// (see settings.locationKnown), so a stuck lookup deserves its own once-a-day heads-up too.
char lastGeoFailureAlertDate[11] = {};
bool weatherTaskInFlight = false;  // Only ever touched from loop() / the functions it calls.
// Set by cmd/weather (a manual, on-demand test trigger); consumeWeatherAutoAdjustResult() checks
// it once the triggered fetch lands and publishes /status back regardless of outcome, so the
// trigger is directly observable over MQTT instead of only inferable from settings.json changing.
bool weatherManualTriggerRequested = false;

WateringRequest activeRequestForReporting;
bool reportedActive = false;
bool mqttBreathPlayed = false;
uint32_t lastWifiRetryMs = 0;

// Sampled at most every kWifiRssiSampleIntervalMs rather than read live from the WiFi driver on
// every loop() iteration/UI render — RSSI jitters several dB reading-to-reading from multipath
// fading even on a stationary device, so a live read would just flicker the on-screen quality
// indicator. -127dBm is below the "miserable" floor, so the local UI reads as no-signal until
// the first real sample lands rather than falsely showing "excellent" from an unsampled 0.
constexpr uint32_t kWifiRssiSampleIntervalMs = 5000;
int32_t cachedWifiRssi = -127;
uint32_t lastWifiRssiSampleMs = 0;
bool wifiRssiSampled = false;

// ---------------------------------------------------------------------------------------------
// DISABLED 2026-08-09: the entire noise-floor/SNR feature is commented out while chasing an OTA
// panic (ESP_RST_PANIC on every OTA attempt, no serial access on site to decode the backtrace).
// Promiscuous mode was the prime suspect: it's the only custom code here that runs in the WiFi
// driver's task context, so it's the only thing that could be invoked while the flash cache is
// disabled mid OTA-write. Marking the callback IRAM_ATTR and force-stopping sampling when OTA
// starts did NOT fix the crash, so the feature is now removed from the build entirely — both to
// rule it out completely and to reclaim its share of RAM/stack. Do not re-enable until an OTA
// has been confirmed working without it. Everything below stays as comments (not deleted) so the
// original design notes survive for whoever restores it.
//
// SNR (RSSI relative to the local RF noise floor) needs the noise floor, which ESP32's normal
// STA API never exposes — wifi_ap_record_t only carries rssi. The only place noise_floor exists
// is the per-packet metadata handed to a *promiscuous*-mode RX callback (and only on original
// ESP32 — S2/S3/C3 don't populate it in this field), so getting it means briefly switching the
// radio into promiscuous mode alongside the normal STA connection, averaging the noise_floor of
// whatever management frames arrive during a short window, then switching promiscuous mode back
// off. Kept short (kNoiseFloorSampleWindowMs) and infrequent (kNoiseFloorSampleIntervalMs)
// deliberately: promiscuous mode adds a callback invocation for every captured frame on the
// channel, and this device's real job is irrigation control, not continuous WiFi sniffing.
// constexpr uint32_t kNoiseFloorSampleIntervalMs = 30000;
// constexpr uint32_t kNoiseFloorSampleWindowMs = 300;
// int8_t cachedNoiseFloorDbm = 0;
// bool noiseFloorSampled = false;    // False until the first window ever produces a reading.
// bool noiseFloorAttempted = false;  // Mirrors wifiRssiSampled's "eager first attempt" pattern.
// bool noiseFloorSamplingActive = false;
// uint32_t lastNoiseFloorAttemptMs = 0;
// uint32_t noiseFloorWindowStartMs = 0;
// Written from the promiscuous RX callback (WiFi driver's own task context), read/reset from
// loop() — plain volatile ints rather than a mutex, since the only failure mode of a torn
// read/write here is one averaging window being off by a sample or two, not a correctness bug.
// volatile int32_t noiseFloorAccumulator = 0;
// volatile uint16_t noiseFloorSampleCount = 0;
// ---------------------------------------------------------------------------------------------

irrigator::domain::IrrigationState previousIrrigationState = irrigator::domain::IrrigationState::Idle;

// True-RMS current sampling (see currentSamplingTask/updateCurrentSampling below). currentWindowOpen
// and currentWindowCloseAtMs are main-loop-only state; currentSamplingWindowActive crosses into
// currentSamplingTask's own task context — written from one side, read from the other, plain
// volatile rather than a mutex, same tolerance as noiseFloorAccumulator above: worst case is one
// stale/extra burst around a window transition, not a correctness bug.
bool currentWindowOpen = false;
uint32_t currentWindowCloseAtMs = 0;
volatile bool currentSamplingWindowActive = false;
// Zone the valve sensor's reading should be attributed to for the *next* burst — 0 while no zone
// is open (pump-only run, or the post-valve-close pump-graceful-shutdown tail). Written every
// loop() tick by updateCurrentSampling(), read once per burst by currentSamplingTask; same
// single-word volatile tolerance as above.
volatile uint8_t currentSamplingActiveZone = 0;
TaskHandle_t currentSamplingTaskHandle = nullptr;

constexpr uint16_t kCurrentBurstMaxSamples =
    irrigator::board::CURRENT_RMS_BURST_DURATION_MS / irrigator::board::CURRENT_RMS_SAMPLE_INTERVAL_MS;

// One burst's result, handed from currentSamplingTask to loop() via currentBurstResultQueue —
// same producer/consumer shape as weatherResultQueue/weatherAutoAdjustTask below: the task only
// computes, loop() is the only place that touches settings/MQTT/persisted state.
struct CurrentBurstResult {
    uint16_t valveRmsMv;
    uint16_t pumpRmsMv;
    uint8_t activeZone;
    uint16_t valveRawMv[kCurrentBurstMaxSamples];
    uint16_t pumpRawMv[kCurrentBurstMaxSamples];
    uint16_t valveRawCount;
    uint16_t pumpRawCount;
};
QueueHandle_t currentBurstResultQueue = nullptr;

// Per-sensor ring of the most recent CURRENT_RMS_HISTORY_LEN burst RMS readings (~1 minute at the
// 5s burst cadence) — cmd/calibrate averages whatever's currently in here as the new base. Reset
// whenever a fresh on-window opens so a previous, unrelated run's readings don't bleed into a
// calibration a minute later. Main-loop-only state (only ever touched while draining
// currentBurstResultQueue), no cross-task synchronization needed.
struct CurrentRmsHistory {
    uint16_t samples[irrigator::board::CURRENT_RMS_HISTORY_LEN] = {};
    uint8_t count = 0;
    uint8_t nextIndex = 0;

    void push(uint16_t value) {
        samples[nextIndex] = value;
        nextIndex = (nextIndex + 1) % irrigator::board::CURRENT_RMS_HISTORY_LEN;
        if (count < irrigator::board::CURRENT_RMS_HISTORY_LEN) {
            count++;
        }
    }
    uint16_t average() const {
        if (count == 0) {
            return 0;
        }
        uint32_t sum = 0;
        for (uint8_t i = 0; i < count; ++i) {
            sum += samples[i];
        }
        return static_cast<uint16_t>(sum / count);
    }
    void reset() {
        count = 0;
        nextIndex = 0;
    }
};
CurrentRmsHistory valveRmsHistory;
CurrentRmsHistory pumpRmsHistory;

// Most recent burst's raw per-sample readings, for cmd/calibrate's saturation-check response.
// Main-loop-only, overwritten every time a burst result is drained.
uint16_t lastValveBurstRawMv[kCurrentBurstMaxSamples] = {};
uint16_t lastPumpBurstRawMv[kCurrentBurstMaxSamples] = {};
uint16_t lastValveBurstRawCount = 0;
uint16_t lastPumpBurstRawCount = 0;

// Once-a-day dedup for over/undercurrent alerts (see checkCurrentFaults below) — same pattern as
// lastWeatherFailureAlertDate/lastGeoFailureAlertDate above. Valve dedup is per-zone (the sensor
// is shared across all 11 zones, but a stuck zone 3 today shouldn't silence a separately failing
// zone 5 later the same day); the pump has no zone dimension.
char lastValveOvercurrentAlertDate[irrigator::board::ZONE_COUNT][11] = {};
char lastValveUndercurrentAlertDate[irrigator::board::ZONE_COUNT][11] = {};
char lastPumpOvercurrentAlertDate[11] = {};
char lastPumpUndercurrentAlertDate[11] = {};

// Once-a-day dedup for flow-meter poll failures (see updateFlowSampling() below) - same
// consumeDailyAlertGate() idiom as the current-sensor faults above. Single buffer, not per-zone:
// unlike the current sensor, there's only one flow meter/one Modbus bus for the whole device, so a
// failure isn't attributable to whichever zone happens to be running.
char lastFlowMeterFailureAlertDate[11] = {};

// Flow meter (TUF-2000M) MQTT publish cadence - see irrigator::domain::FlowPublishGate for the
// once-a-minute-while-active / threshold-while-idle rule this drives (pulled out into domain/ so
// it's unit-testable under `pio test -e native`). Main-loop-only (only ever touched while draining
// flowMeter's own responseReady()/takeReading() mailbox), no cross-task synchronization needed -
// unlike flowMeter itself, which handles that internally.
irrigator::domain::FlowPublishGate flowPublishGate;

// Latest totalizer reading drained from the flow meter (cubic meters, as the driver reports it) -
// the shared baseline both Stage 4 features below diff against. Updated once per valid poll in
// updateFlowSampling(), before either of them runs.
float lastKnownTotalizerM3 = 0.0f;
bool haveLastKnownTotalizerM3 = false;

// Latest whole valid poll (not just the totalizer), kept for cmd/meter's diagnostics reply so the
// command answers from cache instead of triggering a Modbus transaction of its own. Same
// main-loop-only ownership as the baseline above.
irrigator::drivers::Tuf2000mFlowMeter::Reading lastValidFlowReading;
bool haveValidFlowReading = false;

// Never-reset-while-running per-zone/"other" lifetime liters counters (see
// irrigator::domain::FlowLifetimeCounters) and the current run's own volume-so-far (see
// irrigator::domain::FlowRunVolumeTracker) - both pulled into domain/ so their attribution/
// capture-and-clear logic is unit-testable under `pio test -e native`. main.ino's job is just
// feeding them totalizer readings and irrigation-state snapshots at the right moments.
irrigator::domain::FlowLifetimeCounters flowLifetimeCounters;
irrigator::domain::FlowRunVolumeTracker flowRunVolumeTracker;

// /flow_totals publish cadence - simple once-a-minute timer (unlike /flow's threshold+cap gate,
// there's no urgency case to react to here: this is long-term consumption bookkeeping, not leak
// detection), so no dedicated domain/ class the way FlowPublishGate needed one.
uint32_t lastFlowTotalsPublishMs = 0;
bool flowTotalsPublishedOnce = false;
constexpr uint32_t kFlowTotalsPublishIntervalMs = 60000;

// Structured outcome of one handleCalibrateCommand() run, for callers that need more than the
// MQTT side-effects (currently just the Settings-menu "Calibrate CS" flow — see UiIntentType::
// CalibrateCurrentSensors in handleUiIntent — which mirrors it onto the display via
// IrrigatorUi::showCalibrateResult). preconditionFailed means neither sensor was even attempted
// (e.g. valve not open); otherwise each sensor's succeeded/baseLevel/message is independent — one
// can succeed while the other is rejected. message is only meaningful when succeeded is false, and
// deliberately short (unlike the verbose /alert text calibrateOneSensor() still publishes over
// MQTT regardless of this struct) so it fits the SSD1309's ~21-char line width.
struct CalibrateOutcome {
    bool preconditionFailed = false;
    bool valveSucceeded = false;
    uint16_t valveBaseLevel = 0;
    char valveMessage[16] = {};
    bool pumpSucceeded = false;
    uint16_t pumpBaseLevel = 0;
    char pumpMessage[16] = {};
};

// Explicit forward declaration: the default argument on this signature confuses the Arduino
// build's automatic prototype generation (it never emits a prototype for this one, leading to a
// "not declared in this scope" at handleUiIntent's earlier call site otherwise). The default is
// only ever specified once, here — see handleCalibrateCommand's actual definition below.
void handleCalibrateCommand(uint32_t nowMs, CalibrateOutcome* outcome = nullptr);

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

// Throttled to kWifiRssiSampleIntervalMs (see cachedWifiRssi above) — cheap to call every loop()
// iteration regardless, but there's no value in sampling faster than the UI needs.
void sampleWifiRssiIfDue(uint32_t nowMs) {
    if (wifiRssiSampled && nowMs - lastWifiRssiSampleMs < kWifiRssiSampleIntervalMs) {
        return;
    }
    wifiRssiSampled = true;
    lastWifiRssiSampleMs = nowMs;
    // WiFi.RSSI() returns 0 (not a negative dBm) when there's no active association — reading
    // that through the normal thresholds would misreport "no signal" as Excellent instead of
    // Miserable, so fall back to the no-signal sentinel explicitly rather than trusting it.
    cachedWifiRssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
}

// --- DISABLED 2026-08-09 (noise-floor/SNR feature — see the block near cachedNoiseFloorDbm) ---
// Runs in the WiFi driver's own task context, not loop() — keep it to simple integer
// bookkeeping only (no logging, no flash access, nothing that could block the WiFi task).
// The MGMT-only filter set in startNoiseFloorSampling() already restricts what reaches here,
// but the type check is a cheap belt-and-suspenders guard against a stale/misconfigured filter.
// IRAM_ATTR: the WiFi driver can still invoke this while flash cache is disabled (e.g. mid
// sector-erase/write during OTA, since beginUpdateFromUrl() blocks loop() for the whole
// download+flash and nothing else gets a chance to turn promiscuous mode back off — see
// abortNoiseFloorSamplingForOta()). A non-IRAM callback hit in that window is a "Cache disabled
// but cached memory region accessed" panic, not a graceful failure; this is belt-and-suspenders
// alongside abortNoiseFloorSamplingForOta() for any frame already in flight when OTA starts.
// IRAM_ATTR void onPromiscuousPacket(void* buf, wifi_promiscuous_pkt_type_t type) {
//     if (type != WIFI_PKT_MGMT) {
//         return;
//     }
//     const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
//     noiseFloorAccumulator += pkt->rx_ctrl.noise_floor;
//     noiseFloorSampleCount++;
// }
//
// void startNoiseFloorSampling() {
//     noiseFloorAccumulator = 0;
//     noiseFloorSampleCount = 0;
//     wifi_promiscuous_filter_t filter = {};
//     filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
//     esp_wifi_set_promiscuous_filter(&filter);
//     esp_wifi_set_promiscuous_rx_cb(&onPromiscuousPacket);
//     esp_wifi_set_promiscuous(true);
// }
//
// void stopNoiseFloorSamplingAndApply() {
//     esp_wifi_set_promiscuous(false);
//     // A quiet window (e.g. a channel with almost no management-frame traffic) can end with zero
//     // samples — keep whatever the last good reading was rather than clobbering it with garbage.
//     if (noiseFloorSampleCount > 0) {
//         cachedNoiseFloorDbm = static_cast<int8_t>(noiseFloorAccumulator / noiseFloorSampleCount);
//         noiseFloorSampled = true;
//     }
// }
//
// Mirrors sampleWifiRssiIfDue's throttling pattern (kNoiseFloorSampleIntervalMs / eager first
// attempt), plus the extra start/stop state machine promiscuous mode needs: a short
// kNoiseFloorSampleWindowMs burst rather than leaving the radio in promiscuous mode continuously.
// void sampleNoiseFloorIfDue(uint32_t nowMs) {
//     if (noiseFloorSamplingActive) {
//         if (nowMs - noiseFloorWindowStartMs >= kNoiseFloorSampleWindowMs) {
//             stopNoiseFloorSamplingAndApply();
//             noiseFloorSamplingActive = false;
//         }
//         return;
//     }
//     if (WiFi.status() != WL_CONNECTED) {
//         return;
//     }
//     if (noiseFloorAttempted && nowMs - lastNoiseFloorAttemptMs < kNoiseFloorSampleIntervalMs) {
//         return;
//     }
//     noiseFloorAttempted = true;
//     lastNoiseFloorAttemptMs = nowMs;
//     noiseFloorWindowStartMs = nowMs;
//     noiseFloorSamplingActive = true;
//     startNoiseFloorSampling();
// }
//
// Called the instant an OTA is accepted (OtaService's startedHandler, before the HTTP GET) —
// beginUpdateFromUrl() then blocks loop() for the entire download+flash, so if a noise-floor
// sampling window happened to be open at that moment, nothing would otherwise turn promiscuous
// mode back off until OTA finishes. Left on for that long turns a rare 300ms exposure into a
// multi-second one, during which a management frame arriving mid flash-write (cache disabled)
// can crash the device — see onPromiscuousPacket's IRAM_ATTR comment. Safe to call unconditionally
// even if no window is active (esp_wifi_set_promiscuous(false) on an already-off radio is a no-op).
// void abortNoiseFloorSamplingForOta() {
//     if (!noiseFloorSamplingActive) {
//         return;
//     }
//     stopNoiseFloorSamplingAndApply();
//     noiseFloorSamplingActive = false;
// }
// ---------------------------------------------------------------------------------------------

// Forward-declared: defined near runAutoAdjustmentIfDue, which it mirrors gate-for-gate — kept
// side-by-side there so the two can't drift apart, but reported here so the full reason the
// weather cycle hasn't run (no location, no clock, no schedule today, window not open yet, fetch
// in progress, or already done) is visible over MQTT without needing serial logs.
const char* weatherAutoAdjustStatus();

String buildStatusJson(uint32_t nowMs) {
    JsonDocument doc;
    doc["firmware_version"] = irrigator::board::FIRMWARE_VERSION;
    doc["boot_reason"] = bootReasonString(esp_reset_reason());
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime_s"] = nowMs / 1000UL;
    doc["rssi_db"] = cachedWifiRssi;
    doc["wifi_quality"] =
        irrigator::domain::toString(irrigator::domain::wifiQualityFromRssi(cachedWifiRssi));
    // BSSID/channel identify which physical AP (and radio channel) the device is actually
    // associated with right now — useful for spotting a sticky-client roam to a distant/overloaded
    // AP, or channel congestion specific to where the device sits, neither of which RSSI alone
    // reveals (a location can show strong RSSI to a bad AP, or weak RSSI to a clean one). Read
    // live (not cached like RSSI above) since this is only built on-demand/periodically, not on
    // every loop() iteration the way the UI's RSSI read is.
    doc["bssid"] = WiFi.status() == WL_CONNECTED ? WiFi.BSSIDstr() : "";
    doc["wifi_channel"] = WiFi.status() == WL_CONNECTED ? WiFi.channel() : 0;
    // DISABLED 2026-08-09 along with the rest of the noise-floor/SNR feature (see the block near
    // cachedNoiseFloorDbm). noise_floor_db/snr_db simply stop appearing in the status JSON; HA's
    // sensors for them go unavailable, which is the intended, visible signal that this is off.
    // Omitted entirely (rather than a fake sentinel) until the first promiscuous-mode sampling
    // window (see sampleNoiseFloorIfDue) actually lands a reading — see that function for why
    // this isn't available from the normal STA API the way RSSI/BSSID/channel are.
    // if (noiseFloorSampled) {
    //     doc["noise_floor_db"] = cachedNoiseFloorDbm;
    //     doc["snr_db"] = cachedWifiRssi - cachedNoiseFloorDbm;
    // }
    doc["free_heap_bytes"] = ESP.getFreeHeap();
    doc["free_storage_bytes"] = static_cast<uint32_t>(fileStore.freeBytes());
    // Should always read 25600. Anything less (typically 256, PubSubClient's default) means the
    // buffer resize at begin() lost a race with the heap, and every payload larger than that is
    // being dropped silently — including this very /status message, so seeing a small number here
    // at all means it has since self-healed. See MqttService::ensureBufferSize().
    doc["mqtt_buffer_bytes"] = mqtt.bufferSize();
    // Publish-path health. All four are cumulative since boot and normally read 0; watch them
    // against rssi_db rather than in isolation. publish_failures counts publishes that failed
    // every retry (each also forced a reconnect); publish_retried counts ones that failed at least
    // once then succeeded - the near misses, and the earliest sign the link is degrading;
    // publish_offline counts publishes dropped because MQTT was disconnected at the time, which
    // used to be entirely silent; mqtt_forced_reconnects counts sessions torn down by
    // forceReconnect() rather than by the keepalive.
    doc["publish_failures"] = mqtt.publishFailures();
    doc["publish_retried"] = mqtt.publishRetrySuccesses();
    doc["publish_offline"] = mqtt.publishSkippedOffline();
    doc["mqtt_forced_reconnects"] = mqtt.forcedReconnects();
    // Pairs with boot_reason above: "panic" tells you it crashed, this tells you whether there's
    // still something to *read* about it (a dump survives reboots until cmd/coredump_clear erases
    // it, so this stays true across later clean boots — see docs/coredump.md).
    doc["coredump_available"] = coreDumpPresent;
    doc["cpu_temp_c"] = temperatureRead();
    // Master on/off for the whole weather auto-adjustment pipeline (see runAutoAdjustmentIfDue) —
    // surfaced here so "why hasn't this run" is diagnosable remotely without a settings.json pull.
    doc["auto_adjustment_enabled"] = settings.autoAdjustmentEnabled;
    // location_known gates the entire weather auto-adjustment pipeline (see
    // runAutoAdjustmentIfDue) — surfaced here so "no last_weather_update" is diagnosable remotely
    // instead of silently stuck on a geolocation lookup that never succeeded.
    doc["location_known"] = settings.locationKnown;
    if (settings.locationKnown) {
        doc["city"] = settings.city;
    }
    doc["weather_adjust_status"] = weatherAutoAdjustStatus();
    // The percentage the schedule is running at right now (0 = today's cycle is skipped) — puts the
    // outcome of the weather cycle in the same payload cmd/weather reports back, next to the status.
    doc["watering_percentage"] = settings.wateringPercentage;
    // Omitted until this session's first successful weather fetch, same as noise_floor_db above.
    if (lastWeatherUpdateEpoch != 0) {
        char lastWeatherIso[24];
        irrigator::domain::formatIso8601Utc(lastWeatherUpdateEpoch, lastWeatherIso, sizeof(lastWeatherIso));
        doc["last_weather_update"] = lastWeatherIso;
    }

    // outputs (which zone/pump is on, and its current) deliberately omitted — that duplicates
    // /state (fired on every start/stop and on cmd/state) and current readings no longer have a
    // meaningful absolute unit to report outside of cmd/calibrate's response; see
    // docs/technical.md's True-RMS current sampling section.

    String payload;
    serializeJson(doc, payload);
    return payload;
}

void reportStop(RunReason reason) {
    if (!reportedActive) {
        return;
    }
    // For a zone+pump run this is 0 - reportValveClosed() below already reported the real number
    // when the valve closed, and takeVolumeLitersOnStop() only ever reports non-zero once per run.
    // For a pump-only run (no separate valve-closed event), this is the run's only stop event, so
    // it's where the volume actually gets reported.
    const float volumeLiters =
        flowRunVolumeTracker.takeVolumeLitersOnStop(lastKnownTotalizerM3, haveLastKnownTotalizerM3);
    mqtt.publishState(activeRequestForReporting.zone,
                      zoneName(activeRequestForReporting.zone),
                      false,
                      false,
                      reason,
                      settings.scheduleActive,
                      0,
                      volumeLiters);
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
    // The valve closing is the moment this zone's watering is actually done, even though the pump
    // may still be running its graceful-shutdown tail - so this is where a zone+pump run's real
    // volume gets reported (see FlowRunVolumeTracker's class comment).
    const float volumeLiters =
        flowRunVolumeTracker.takeVolumeLitersOnStop(lastKnownTotalizerM3, haveLastKnownTotalizerM3);
    mqtt.publishState(activeRequestForReporting.zone,
                      zoneName(activeRequestForReporting.zone),
                      false,
                      true,
                      activeRequestForReporting.reason,
                      settings.scheduleActive,
                      0,
                      volumeLiters);
}

bool startIrrigation(const WateringRequest& request, uint32_t nowMs) {
    if (!irrigation.start(request, nowMs)) {
        // Previously a bare `return false` - the request was popped off the queue and dropped with
        // no report on any channel, which is indistinguishable from "the device ignored me".
        char alertMsg[128];
        snprintf(alertMsg, sizeof(alertMsg),
                 R"({"error":"watering request dropped","zone":%u,"duration":%u,"state":%u})",
                 static_cast<unsigned>(request.zone),
                 static_cast<unsigned>(request.durationMin),
                 static_cast<unsigned>(irrigation.state()));
        mqtt.publishAlert(alertMsg);
        return false;
    }
    activeRequestForReporting = request;
    reportedActive = true;
    // Baseline the new run's volume tracking against whichever totalizer reading is currently
    // known - see FlowRunVolumeTracker's class comment for why this needs to happen exactly once,
    // right at the start of each run.
    flowRunVolumeTracker.beginRun(lastKnownTotalizerM3, haveLastKnownTotalizerM3);
    mqtt.publishState(request.zone,
                      zoneName(request.zone),
                      request.zone != 0,
                      true,
                      request.reason,
                      settings.scheduleActive,
                      request.durationMin,
                      0.0f);
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
    // static, not a plain local: SETTINGS_JSON_MAX_BYTES is 1536, which made this function's stack
    // frame 1600 bytes. persistSettings() is called from deep inside handleMqttCommand() (cmd/
    // scheduled, cmd/upload/settings.json, cmd/calibrate, ...), so that frame stacked on top of
    // that one's — 1600 + ~4KB — on the shared loop-task stack. Safe as static/shared scratch
    // because every caller runs on the loop task: the UI intents, all MQTT commands, and the
    // weather cycle's persist (the background weatherAutoAdjustTask only ever computes and posts
    // its result to weatherResultQueue; drainWeatherResult... calls this from loop()). Do NOT call
    // persistSettings() from another task without giving it its own buffer or a mutex.
    static char buffer[irrigator::board::SETTINGS_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSettings(settings, buffer, sizeof(buffer));
    const bool ok =
        written != 0 &&
        fileStore.writeTextWithBackup(irrigator::board::SETTINGS_FILE_PATH, String(buffer));
    if (!ok) {
        // Central, loud failure reporting for EVERY persist path (UI toggle, MQTT command,
        // auto-adjustment, boot-time default write). A silent failure here is exactly how RAM and
        // flash diverge: the in-RAM `settings` keep the new value — and get echoed over MQTT
        // (/scheduled, /watering_percentage, /status) as if saved — while /settings.json on flash
        // still holds the old one, so the next reboot loads the old value and silently reverts the
        // change (and cmd/download/settings.json, which reads the raw file, reports the old value).
        // publishAlert is a no-op when MQTT isn't connected yet (e.g. the boot default write), which
        // is fine — the serial log line still lands.
        irrigator::drivers::Log.error("persistSettings: flash write failed; RAM and flash now diverge");
        mqtt.publishAlert(
            R"({"error":"settings write to flash failed; change will not survive a reboot"})");
    }
    return ok;
}

// config.json's counterpart to persistSettings(), for the one field the device writes on its own
// after boot: the geolocation-derived timezone (tryGeolocateIfDue). Same divergence hazard, same
// loud reporting — the in-RAM deviceConfig would otherwise keep an offset flash doesn't have, and
// the next reboot would quietly go back to UTC and shift every scheduled watering by hours.
// Unlike a config.json MQTT upload this deliberately does NOT restart: nothing here touches the
// Wi-Fi/MQTT credentials that only take effect at boot, and clockService picks the new offset up
// live.
bool persistConfig() {
    // Plain local, not static like persistSettings()' buffer: CONFIG_JSON_MAX_BYTES is 768, and the
    // only caller (tryGeolocateIfDue, straight off loop()) has a shallow frame under it.
    char buffer[irrigator::board::CONFIG_JSON_MAX_BYTES];
    const size_t written =
        irrigator::domain::serializeConfig(deviceConfig, buffer, sizeof(buffer));
    const bool ok =
        written != 0 &&
        fileStore.writeTextWithBackup(irrigator::board::CONFIG_FILE_PATH, String(buffer));
    if (!ok) {
        irrigator::drivers::Log.error("persistConfig: flash write failed; RAM and flash now diverge");
        mqtt.publishAlert(
            R"({"error":"config write to flash failed; change will not survive a reboot"})");
    }
    return ok;
}

// schedule_active change tracer. The flag has been flipping to false across reboots for reasons
// that are impossible to see without a serial console, so every in-RAM change to it routes through
// here, which publishes an MQTT alert naming the cause. Watch sensor.irrigator_last_alert to see
// exactly what changed it and in what order — no USB needed. Only publishes on an actual change,
// and does NOT persist (callers decide when to write flash), so it's safe to call from anywhere.
void setScheduleActive(bool newValue, const char* cause) {
    if (settings.scheduleActive == newValue) {
        return;
    }
    settings.scheduleActive = newValue;
    irrigator::drivers::Log.printf("[INFO] schedule_active -> %s (%s)\n",
                                   newValue ? "true" : "false", cause);
    char msg[128];
    snprintf(msg, sizeof(msg), R"({"schedule_active_changed_to":%s,"cause":"%s"})",
             newValue ? "true" : "false", cause);
    mqtt.publishAlert(msg);
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

// Companion to reportMqttError() for a merge upload (see handleMqttUpload) that otherwise
// succeeded: `rejectedFields` names the field(s) whose new value failed its own validation and
// so kept the existing file's value instead — surfaced as an alert (not an error) since the rest
// of the upload still went through.
void reportMqttFieldsRejected(const String& filename, const char* rejectedFields) {
    if (rejectedFields[0] == '\0') {
        return;
    }
    char alertMsg[224];
    snprintf(alertMsg, sizeof(alertMsg),
             R"({"upload":"%s","alert":"invalid value(s) ignored, kept previous: %s"})",
             filename.c_str(), rejectedFields);
    mqtt.publishAlert(alertMsg);
    irrigator::drivers::Log.printf("[WARN] upload %s: invalid value(s) ignored: %s\n",
                                   filename.c_str(), rejectedFields);
}

// ---------------------------------------------------------------------------------------------
// Core dump retrieval. The panic handler writes an ELF image into the `coredump` partition (see
// partitions.csv) on every crash; these functions are the only way to get it off a unit that lives
// in a garage with no serial connection. Wire format and the decode workflow: docs/coredump.md.
// ---------------------------------------------------------------------------------------------

// Fills `target` with the decoded summary — shared by cmd/coredump, the boot-time report, and the
// header of a cmd/download/coredump transfer, so all three can't describe the same dump
// differently. Addresses go out as "0x...." strings: they are meant to be pasted straight into
// addr2line, and a JSON number would arrive as a decimal the operator has to convert back.
void fillCoreDumpSummaryJson(JsonObject target) {
    irrigator::drivers::CoreDumpSummary summary;
    if (!coreDumpService.readSummary(summary)) {
        target["coredump"] = "none";
        return;
    }

    char hex[12];
    target["coredump"] = "present";
    target["size_bytes"] = static_cast<uint32_t>(coreDumpService.totalBytes());
    target["task"] = summary.taskName;
    target["panic_reason"] = summary.panicReason;
    snprintf(hex, sizeof(hex), "0x%08lx", static_cast<unsigned long>(summary.programCounter));
    target["pc"] = hex;
    target["exc_cause"] = summary.exceptionCause;
    snprintf(hex, sizeof(hex), "0x%08lx", static_cast<unsigned long>(summary.faultAddress));
    target["fault_addr"] = hex;
    // The addresses above only mean anything against the build that produced them, and this device
    // is updated over the air — so name that build (see CoreDumpSummary::appElfSha256).
    target["elf_sha256"] = summary.appElfSha256;
    target["bt_corrupted"] = summary.backtraceCorrupted;
    JsonArray backtrace = target["backtrace"].to<JsonArray>();
    for (uint8_t i = 0; i < summary.backtraceDepth; ++i) {
        snprintf(hex, sizeof(hex), "0x%08lx", static_cast<unsigned long>(summary.backtrace[i]));
        backtrace.add(hex);
    }
}

void publishCoreDumpSummary() {
    JsonDocument doc;
    fillCoreDumpSummaryJson(doc.to<JsonObject>());
    String payload;
    serializeJson(doc, payload);
    mqtt.publishCoreDump(payload);
}

// State of an in-flight `cmd/download/coredump`. The image is up to 64KB of binary, so it goes out
// as base64 chunks, one per loop() iteration — not all at once inside the MQTT callback, which
// would stall loop() (and so irrigation.tick(), the UI, and the current-fault drain) for the whole
// transfer. This is agents.md's "set a flag in the handler, do the work in loop()" rule.
struct CoreDumpTransfer {
    bool active = false;
    size_t offset = 0;
    size_t totalBytes = 0;
    uint16_t nextSeq = 0;
    uint16_t chunkCount = 0;
    uint32_t crc = 0;
    uint8_t* raw = nullptr;
    char* encoded = nullptr;
};
CoreDumpTransfer coreDumpTransfer;

// 3072 raw bytes -> 4096 base64 chars, which leaves plenty of headroom inside MqttService's
// ~25KB PubSubClient buffer once the envelope and topic are added, and caps a full 64KB image at
// 22 chunks. Both buffers are heap-allocated for the duration of a transfer and freed at the end:
// one large, rare allocation, not the per-loop churn agents.md's heap-fragmentation rule targets,
// and far better than paying ~7KB of static RAM forever for something used a handful of times in
// a device's life.
constexpr size_t kCoreDumpChunkBytes = 3072;
constexpr size_t kCoreDumpEncodedBytes = 4 * ((kCoreDumpChunkBytes + 2) / 3) + 1;

void finishCoreDumpTransfer() {
    delete[] coreDumpTransfer.raw;
    delete[] coreDumpTransfer.encoded;
    coreDumpTransfer = CoreDumpTransfer{};
}

// `cmd/download/coredump` — arms the transfer serviced by serviceCoreDumpTransfer() below.
void startCoreDumpTransfer() {
    if (coreDumpTransfer.active) {
        reportMqttError("download", "coredump", "a transfer is already in progress");
        return;
    }
    if (!coreDumpService.available()) {
        reportMqttError("download", "coredump", "no valid core dump stored");
        return;
    }
    const size_t total = coreDumpService.totalBytes();
    if (total == 0) {
        reportMqttError("download", "coredump", "stored dump is empty");
        return;
    }

    coreDumpTransfer.raw = new (std::nothrow) uint8_t[kCoreDumpChunkBytes];
    coreDumpTransfer.encoded = new (std::nothrow) char[kCoreDumpEncodedBytes];
    if (coreDumpTransfer.raw == nullptr || coreDumpTransfer.encoded == nullptr) {
        finishCoreDumpTransfer();  // Frees whichever of the two did succeed.
        reportMqttError("download", "coredump", "out of memory for transfer buffers");
        return;
    }

    coreDumpTransfer.active = true;
    coreDumpTransfer.totalBytes = total;
    coreDumpTransfer.nextSeq = 1;
    coreDumpTransfer.chunkCount =
        static_cast<uint16_t>((total + kCoreDumpChunkBytes - 1) / kCoreDumpChunkBytes);

    // The header carries the summary too, so a single command answers both "what crashed" and
    // "give me the image" — the receiver has something useful even if the chunks then get cut off.
    JsonDocument doc;
    doc["coredump"] = "begin";
    doc["size"] = static_cast<uint32_t>(total);
    doc["chunks"] = coreDumpTransfer.chunkCount;
    doc["chunk_bytes"] = static_cast<uint32_t>(kCoreDumpChunkBytes);
    doc["encoding"] = "base64";
    doc["firmware_version"] = irrigator::board::FIRMWARE_VERSION;
    fillCoreDumpSummaryJson(doc["summary"].to<JsonObject>());
    String header;
    serializeJson(doc, header);
    mqtt.publishDownload("coredump", header);

    irrigator::drivers::Log.printf("[INFO] core dump transfer started: %u bytes in %u chunks\n",
                                   static_cast<unsigned>(total),
                                   static_cast<unsigned>(coreDumpTransfer.chunkCount));
}

// One chunk per loop() iteration. Deliberately not paced by a timer: the whole transfer is at most
// 22 messages, and since every one is a QoS-0 fire-and-forget publish, the sooner they are all out
// the smaller the window in which a Wi-Fi drop can tear a hole in the image.
void serviceCoreDumpTransfer() {
    if (!coreDumpTransfer.active) {
        return;
    }
    if (!mqtt.connected()) {
        // Publishing into a disconnected client would drop chunks silently, and the receiver
        // can't tell a gap like that from a lost packet — better to abort loudly and let the
        // operator re-request the whole thing. The dump itself is untouched (only
        // cmd/coredump_clear ever erases it), so nothing is lost by giving up here.
        irrigator::drivers::Log.warn("Core dump transfer aborted: MQTT disconnected");
        finishCoreDumpTransfer();
        return;
    }

    const size_t read =
        coreDumpService.readChunk(coreDumpTransfer.offset, coreDumpTransfer.raw, kCoreDumpChunkBytes);
    if (read == 0) {
        reportMqttError("download", "coredump", "flash read failed mid-transfer");
        finishCoreDumpTransfer();
        return;
    }

    size_t encodedLength = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(coreDumpTransfer.encoded),
                              kCoreDumpEncodedBytes,
                              &encodedLength,
                              coreDumpTransfer.raw,
                              read) != 0) {
        reportMqttError("download", "coredump", "base64 encode failed");
        finishCoreDumpTransfer();
        return;
    }
    coreDumpTransfer.encoded[encodedLength] = '\0';
    // Running CRC32 over the raw bytes, reported in the trailer. Sequence numbers already catch a
    // dropped chunk; this catches a chunk that arrived but is wrong, and distinguishes "the
    // transfer was corrupted" from "the stored dump itself is corrupt" (which esp-coredump would
    // report separately when decoding).
    coreDumpTransfer.crc = esp_rom_crc32_le(coreDumpTransfer.crc, coreDumpTransfer.raw, read);

    // Assembled by hand rather than through ArduinoJson: the envelope is fixed, and base64's
    // alphabet (A-Za-z0-9+/=) contains nothing JSON would need to escape, so serializing would
    // only buy a second 4KB copy of the payload on the heap.
    String payload("{\"seq\":");
    payload.reserve(encodedLength + 48);
    payload += coreDumpTransfer.nextSeq;
    payload += ",\"data\":\"";
    payload += coreDumpTransfer.encoded;
    payload += "\"}";
    mqtt.publishDownload("coredump", payload);

    coreDumpTransfer.offset += read;
    ++coreDumpTransfer.nextSeq;

    if (coreDumpTransfer.offset < coreDumpTransfer.totalBytes) {
        return;
    }

    char trailer[112];
    snprintf(trailer, sizeof(trailer),
             R"({"coredump":"end","chunks":%u,"size":%lu,"crc32":"0x%08lx"})",
             static_cast<unsigned>(coreDumpTransfer.chunkCount),
             static_cast<unsigned long>(coreDumpTransfer.totalBytes),
             static_cast<unsigned long>(coreDumpTransfer.crc));
    mqtt.publishDownload("coredump", trailer);
    irrigator::drivers::Log.printf("[INFO] core dump transfer complete: %u bytes in %u chunks\n",
                                   static_cast<unsigned>(coreDumpTransfer.totalBytes),
                                   static_cast<unsigned>(coreDumpTransfer.chunkCount));
    finishCoreDumpTransfer();
}

// One-shot, published alongside the boot /status. If the panic handler wrote a dump before the
// reboot we're now in, say so unprompted: nobody is watching the serial console on a deployed
// unit, and a crash dump nobody knows about is worth exactly as much as no crash dump at all.
void reportCoreDumpAtBoot() {
    if (!coreDumpPresent) {
        return;
    }
    publishCoreDumpSummary();
    mqtt.publishAlert(
        R"({"coredump":"available","alert":"a core dump from an earlier crash is stored; )"
        R"(retrieve it with cmd/download/coredump before it is overwritten"})");
}

// `cmd/download/<filename>` handler: filename comes from the topic (payload is unused, "" per
// spec), response goes to `<prefix>/download/<filename>`.
void handleMqttDownload(const String& filename) {
    if (filename.length() == 0) {
        reportMqttError("download", filename, "empty filename");
        return;
    }

    // The core dump is not a file on SPIFFS — it's a raw flash partition the panic handler writes
    // (see drivers/CoreDumpService.h) — so it can't go through the fileStore path below, and being
    // binary and up to 64KB it can't go out in one message either. Handled under cmd/download/
    // anyway rather than its own verb, so "cmd/download/<x>" stays the single answer to "how do I
    // pull <x> off the device".
    if (filename == "coredump") {
        startCoreDumpTransfer();
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

// `cmd/upload/<filename>` handler: payload is the new file content. config.json/settings.json
// are merged field-by-field onto the device's current in-RAM copy (deviceConfig/settings) rather
// than replacing the whole file: a field present in the upload with a non-null, non-empty value
// is validated on its own and applied only if it individually makes sense; a field that's
// missing, null, an empty string, or fails its own check instead keeps the existing value, with
// the latter case also raising an alert naming the field (see ConfigStore.h/Settings.h's
// mergeConfig/mergeSettings). schedule.json keeps the older reject-whole-file-on-invalid
// semantics unchanged (it's a JSON array of records, not a flat object of fields — see
// Schedule.h), though its record count can still grow/shrink freely. Any other filename is
// written as-is since there's no schema to validate it against. A successful config.json upload
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
        char rejectedFields[192];
        irrigator::domain::DeviceConfig merged;
        if (!irrigator::domain::mergeConfig(deviceConfig, content.c_str(), content.length(), merged,
                                            rejectedFields, sizeof(rejectedFields)) ||
            !irrigator::domain::validateConfig(merged)) {
            reportMqttError("upload", filename, "invalid config.json content");
            return;
        }
        char buffer[irrigator::board::CONFIG_JSON_MAX_BYTES];
        const size_t written = irrigator::domain::serializeConfig(merged, buffer, sizeof(buffer));
        if (written == 0 || !fileStore.writeTextWithBackup(path.c_str(), String(buffer))) {
            reportMqttError("upload", filename, "failed to write");
            return;
        }
        reportMqttFieldsRejected(filename, rejectedFields);
        irrigator::drivers::Log.info("config.json updated via MQTT upload; restarting");
        mqtt.publishAvailability(false);
        delay(200);
        ESP.restart();
        return;
    }

    if (path == irrigator::board::SETTINGS_FILE_PATH) {
        char rejectedFields[256];
        irrigator::domain::Settings merged;
        if (!irrigator::domain::mergeSettings(settings, content.c_str(), content.length(), merged,
                                              rejectedFields, sizeof(rejectedFields)) ||
            !irrigator::domain::validateSettings(merged)) {
            reportMqttError("upload", filename, "invalid settings.json content");
            return;
        }
        // schedule_active is device-owned: it is changed ONLY via cmd/scheduled (the switch), never
        // by a settings upload. HA's full-replace settings.json republish (fired by editing any
        // unrelated field — a zone name, a calibration value) carries a "passenger" schedule_active
        // that HA has no reliable, non-racy source for during a reconnect, and letting it through
        // was silently disabling the schedule after a reboot (RAM/echo stayed true, but the file
        // this upload wrote said false, so the NEXT reboot loaded false). Ignore the uploaded value
        // and keep the device's own. The diagnostic alert below fires exactly when an upload tried
        // to change it — it is the definitive proof of "HA uploaded false", and confirms this guard
        // caught it.
        if (merged.scheduleActive != settings.scheduleActive) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     R"({"info":"settings upload tried to set schedule_active=%s; ignored, kept device value %s"})",
                     merged.scheduleActive ? "true" : "false",
                     settings.scheduleActive ? "true" : "false");
            mqtt.publishAlert(msg);
        }
        merged.scheduleActive = settings.scheduleActive;
        // auto_adjustment_enabled is guarded the same way, and for the same reason: HA's
        // full-settings republish (script.irrigator_publish_settings) can only echo back whatever
        // sensor.irrigator_settings last saw, which races the same way schedule_active did — an
        // upload landing right after a toggle could carry the pre-toggle value and silently flip
        // it back on the next reboot. The dedicated cmd/auto_adjustment command (and its switch)
        // is the only path that's allowed to actually change this.
        if (merged.autoAdjustmentEnabled != settings.autoAdjustmentEnabled) {
            char msg[176];
            snprintf(msg, sizeof(msg),
                     R"({"info":"settings upload tried to set auto_adjustment_enabled=%s; ignored, kept device value %s"})",
                     merged.autoAdjustmentEnabled ? "true" : "false",
                     settings.autoAdjustmentEnabled ? "true" : "false");
            mqtt.publishAlert(msg);
        }
        merged.autoAdjustmentEnabled = settings.autoAdjustmentEnabled;
        // location_known/latitude/longitude/city are likewise device-owned: they're populated only
        // by the device's own geolocation resolution (see resolveGeolocationIfNeeded()), never by a
        // settings upload -- mergeSettings() doesn't even read them from the upload, so `merged`
        // already carries `settings`'s values here. Kept as an explicit assignment anyway so that
        // stays true even if mergeSettings' internals ever change.
        merged.locationKnown = settings.locationKnown;
        merged.latitude = settings.latitude;
        merged.longitude = settings.longitude;
        snprintf(merged.city, sizeof(merged.city), "%s", settings.city);
        // valve/pump_current_base_level are likewise device-owned: only cmd/calibrate ever sets
        // them (see handleMqttCommand's "calibrate" branch / handleCalibrateCommand). mergeSettings()
        // doesn't read them from the upload either, so this is belt-and-suspenders, same as the
        // location fields above.
        merged.valveCurrentBaseLevel = settings.valveCurrentBaseLevel;
        merged.pumpCurrentBaseLevel = settings.pumpCurrentBaseLevel;
        settings = merged;
        // Persist the MERGED result (serialized from `settings`), NOT the raw uploaded bytes — those
        // may omit fields entirely (partial upload) or still contain the ignored schedule_active/
        // auto_adjustment_enabled, and writing them would put stale/incomplete data back on flash.
        // persistSettings() surfaces any write failure via its own alert.
        if (!persistSettings()) {
            reportMqttError("upload", filename, "failed to write");
            return;
        }
        reportMqttFieldsRejected(filename, rejectedFields);
        return;
    }

    if (path == irrigator::board::SCHEDULE_FILE_PATH) {
        // static for the same reason as persistSettings()' buffer and runScheduleEngine()'s
        // dueRequests: sizeof(ScheduleList) is 2551 bytes at SCHEDULE_MAX_RECORDS=255, and this
        // sits in handleMqttCommand()'s frame. Still a staging area, not live state — the parse
        // must succeed AND the file must be written before it's committed to the global
        // scheduleList, so a rejected upload leaves the running schedule untouched. Loop-task only
        // (MQTT commands are dispatched from mqtt.loop() inside loop()).
        static irrigator::domain::ScheduleList parsed;
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

// `cmd/delete/<filename>` handler: filename comes from the topic, payload must be the literal
// string "true" (a deliberate confirmation, not a toggle/boolean like cmd/scheduled) or the
// request is rejected outright. Main use case: mergeSettings()/mergeConfig() (see
// handleMqttUpload) can only ever add or update a field, never remove one that's no longer
// wanted -- e.g. after a zone_names restructuring, or to shrink a file back down -- so deleting
// the file and re-uploading a clean one (or letting boot-time defaults regenerate it) is the
// escape hatch. Deleting config.json/settings.json/schedule.json only takes effect on the next
// boot (the in-RAM deviceConfig/settings/scheduleList stay live until then); deleting config.json
// specifically drops the device into captive portal mode on that next boot, since there's then no
// Wi-Fi config left to load (see loadOperationalConfig()).
void handleMqttDelete(const String& filename, const String& body) {
    if (filename.length() == 0) {
        reportMqttError("delete", filename, "empty filename");
        return;
    }

    String trimmedBody = body;
    trimmedBody.trim();
    if (trimmedBody != "true") {
        reportMqttError("delete", filename, "payload must be 'true' to confirm deletion");
        return;
    }

    String path = filename;
    path.trim();
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    if (!fileStore.exists(path.c_str())) {
        reportMqttError("delete", filename, "not found");
        return;
    }
    if (!fileStore.remove(path.c_str())) {
        reportMqttError("delete", filename, "failed to delete");
        return;
    }
    // Best-effort: a stray .bak left behind by writeTextWithBackup() shouldn't outlive the
    // primary file it was backing up. Not checked for success -- it may simply not exist.
    fileStore.remove((path + ".bak").c_str());

    irrigator::drivers::Log.printf("[INFO] %s deleted via MQTT cmd/delete\n", filename.c_str());
    char alertMsg[128];
    snprintf(alertMsg, sizeof(alertMsg), R"({"delete":"%s","info":"file deleted"})", filename.c_str());
    mqtt.publishAlert(alertMsg);
}

// `cmd/state` handler: publishes the current watering/pump status on demand, independent of
// the edge-triggered publishes that fire on every start/stop.
void publishCurrentState() {
    const auto snapshot = irrigation.snapshot(millis());
    const bool valveOn = snapshot.state == irrigator::domain::IrrigationState::Watering;
    const RunReason reason = reportedActive ? activeRequestForReporting.reason : RunReason::Manual;
    const uint16_t remainingMin = static_cast<uint16_t>(snapshot.remainingMs / 60000UL);
    // On-demand, not a stop event - never carries a completed run's volume (see
    // FlowRunVolumeTracker's class comment: that's a one-shot field on the specific /state message
    // where "on" transitions to false, not a persisted "current volume" to poll here).
    mqtt.publishState(snapshot.activeZone,
                      zoneName(snapshot.activeZone),
                      valveOn,
                      snapshot.pumpOn,
                      reason,
                      settings.scheduleActive,
                      remainingMin,
                      0.0f);
}

bool persistScheduleAndReport() {
    // Heap-allocated, not a stack array: at SCHEDULE_JSON_MAX_BYTES=24KB this would blow the
    // default ~8KB loop-task stack outright.
    auto buffer = std::make_unique<char[]>(irrigator::board::SCHEDULE_JSON_MAX_BYTES);
    const size_t written =
        irrigator::domain::serializeSchedule(scheduleList, buffer.get(), irrigator::board::SCHEDULE_JSON_MAX_BYTES);
    if (written == 0) {
        return false;
    }
    const bool ok = fileStore.writeTextWithBackup(irrigator::board::SCHEDULE_FILE_PATH, String(buffer.get()));
    mqtt.publishSchedule(buffer.get());
    return ok;
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
        // Not a fault, and deliberately unreported: a queued request correctly waits here for the
        // duration of whatever is already running. Worth knowing when reading MQTT, though - this
        // silent return is why a cmd/run can produce a `/queue` push echo and then nothing at all
        // for minutes, which looks exactly like a device that ignored the rest of the command.
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
// the ones that have to wait behind an active run. Returns whether the request was actually
// pushed (false = dropped: disabled zone or full queue) — the UI's queue-add confirmation
// (UiIntentType::StartWatering while watering is already active, see handleUiIntent below) uses
// this to show success/failure instead of assuming every push lands.
bool enqueueOrStart(const WateringRequest& request, uint32_t nowMs) {
    if (!isZoneEnabled(request.zone)) {
        char alertMsg[64];
        snprintf(alertMsg,
                sizeof(alertMsg),
                "Zone %u (%s) is disabled. Enable it in settings first.",
                static_cast<unsigned>(request.zone),
                zoneName(request.zone));
        mqtt.publishAlert(alertMsg);
        return false;
    }
    if (!wateringQueue.push(request)) {
        char alertMsg[112];
        snprintf(alertMsg,
                sizeof(alertMsg),
                "Watering queue is full. Dropping watering: zone=%u duration=%u",
                static_cast<unsigned>(request.zone),
                static_cast<unsigned>(request.durationMin));
        mqtt.publishAlert(alertMsg);
        return false;
    }
    publishQueueState();
    pumpQueue(nowMs);
    return true;
}

void handleUiIntent(const irrigator::domain::UiIntent& intent, uint32_t nowMs) {
    switch (intent.type) {
        case UiIntentType::StartWatering: {
            WateringRequest request;
            request.zone = intent.zone;
            request.durationMin = intent.duration;
            request.reason = RunReason::Manual;
            // intent.boolValue: set by UiStateMachine::handleDurationPicker when the request was
            // made while watering was already active — that's the queue-add path, so it needs the
            // "Added to queue"/"QUEUE FULL" confirmation screen. An immediate start (not queued
            // behind anything) skips it, same as before this existed.
            const bool queued = intent.boolValue;
            const bool pushed = enqueueOrStart(request, nowMs);
            if (queued) {
                char zoneLabel[24];
                if (intent.zone == 0) {
                    snprintf(zoneLabel, sizeof(zoneLabel), "PUMP");
                } else {
                    snprintf(zoneLabel, sizeof(zoneLabel), "%u.%s", static_cast<unsigned>(intent.zone), zoneName(intent.zone));
                }
                ui.showQueueAddResult(zoneLabel, intent.duration, pushed, nowMs);
            }
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
            setScheduleActive(intent.boolValue, "ui_toggle");
            persistSettings();
            mqtt.publishScheduled(settings.scheduleActive);
            break;
        case UiIntentType::ToggleAutoAdjustment:
            settings.autoAdjustmentEnabled = intent.boolValue;
            persistSettings();
            mqtt.publishAutoAdjustmentEnabled(settings.autoAdjustmentEnabled);
            break;
        case UiIntentType::ToggleAlertPumpUc:
            settings.alertPumpUc = intent.boolValue;
            persistSettings();
            break;
        case UiIntentType::ToggleAlertPumpOc:
            settings.alertPumpOc = intent.boolValue;
            persistSettings();
            break;
        case UiIntentType::ToggleAlertValveUc:
            settings.alertValveUc = intent.boolValue;
            persistSettings();
            break;
        case UiIntentType::ToggleAlertValveOc:
            settings.alertValveOc = intent.boolValue;
            persistSettings();
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
        case UiIntentType::SetUpperLimit: {
            irrigator::domain::Settings updated = settings;
            updated.upperLimitPercent = intent.percentage;
            if (irrigator::domain::validateSettings(updated)) {
                settings = updated;
                persistSettings();
            }
            break;
        }
        case UiIntentType::SetLowerLimit: {
            irrigator::domain::Settings updated = settings;
            updated.lowerLimitPercent = intent.percentage;
            if (irrigator::domain::validateSettings(updated)) {
                settings = updated;
                persistSettings();
            }
            break;
        }
        case UiIntentType::SetRainSkipProbability: {
            irrigator::domain::Settings updated = settings;
            updated.rainSkipProbabilityPercent = intent.percentage;
            if (irrigator::domain::validateSettings(updated)) {
                settings = updated;
                persistSettings();
            }
            break;
        }
        case UiIntentType::SetRainSkipAmount: {
            irrigator::domain::Settings updated = settings;
            updated.rainSkipAmountMm = intent.percentage;
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
        case UiIntentType::AddScheduleRecord: {
            bool success = false;
            if (scheduleList.count < irrigator::board::SCHEDULE_MAX_RECORDS) {
                irrigator::domain::ScheduleRecord record;
                record.zone = intent.zone;
                record.dayOfWeek = intent.dayOfWeek;
                snprintf(record.startTime,
                        sizeof(record.startTime),
                        "%02u:%02u",
                        static_cast<unsigned>(intent.hour),
                        static_cast<unsigned>(intent.minute));
                record.durationMin = static_cast<uint8_t>(intent.duration);
                record.active = true;
                if (irrigator::domain::validateScheduleRecord(record)) {
                    scheduleList.records[scheduleList.count++] = record;
                    success = persistScheduleAndReport();
                }
            }
            ui.showScheduleActionResult(success, nowMs);
            break;
        }
        case UiIntentType::DeleteScheduleRecord: {
            bool success = false;
            if (intent.scheduleRecordIndex < scheduleList.count) {
                for (uint8_t i = intent.scheduleRecordIndex; i + 1 < scheduleList.count; ++i) {
                    scheduleList.records[i] = scheduleList.records[i + 1];
                }
                scheduleList.count--;
                success = persistScheduleAndReport();
            }
            ui.showScheduleActionResult(success, nowMs);
            break;
        }
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
        case UiIntentType::RestartDevice:
            irrigator::drivers::Log.info("Restart requested from Settings menu (confirmed)");
            mqtt.publishAvailability(false);
            delay(200);
            ESP.restart();
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
        case UiIntentType::CalibrateCurrentSensors: {
            CalibrateOutcome outcome;
            handleCalibrateCommand(nowMs, &outcome);
            ui.showCalibrateResult(outcome.valveSucceeded, outcome.valveBaseLevel, outcome.valveMessage,
                                   outcome.pumpSucceeded, outcome.pumpBaseLevel, outcome.pumpMessage, nowMs);
            break;
        }
        case UiIntentType::None:
        default:
            break;
    }
}

// Captures one sensor's base_level from its calibration history (see CurrentRmsHistory above) and,
// if accepted, publishes the cmd/calibrate response and sets `updated` for the caller to persist.
// Rejects (leaving `baseLevel` untouched) an empty history or a too-low average — either way it
// reports why via /alert rather than silently saving a bad calibration (agents.md's fail-loud
// rule): a near-zero reading usually means the output wasn't actually drawing current (wiring
// fault, relay not energizing), not a genuinely low-draw device. succeededOut/messageOut mirror
// the outcome for handleCalibrateCommand's optional CalibrateOutcome (messageOut is a short,
// display-friendly reason — see CalibrateOutcome's doc — distinct from the verbose /alert text
// published below).
void calibrateOneSensor(const char* sensorName,
                        CurrentRmsHistory& history,
                        const uint16_t* rawMv,
                        uint16_t rawCount,
                        uint16_t& baseLevel,
                        bool& updated,
                        bool& succeededOut,
                        char* messageOut,
                        size_t messageOutSize) {
    succeededOut = false;
    if (history.count == 0) {
        char alertMsg[144];
        snprintf(alertMsg, sizeof(alertMsg),
                R"({"command":"calibrate","sensor":"%s","error":"no samples collected yet, try again in a few seconds"})",
                sensorName);
        mqtt.publishAlert(alertMsg);
        snprintf(messageOut, messageOutSize, "no data yet");
        return;
    }

    const uint16_t avg = history.average();
    if (avg < irrigator::board::CURRENT_CALIBRATION_MIN_BASE_LEVEL) {
        char alertMsg[192];
        snprintf(alertMsg, sizeof(alertMsg),
                R"({"command":"calibrate","sensor":"%s","error":"reading too low (%u); check wiring or that the relay is actually energizing"})",
                sensorName, static_cast<unsigned>(avg));
        mqtt.publishAlert(alertMsg);
        snprintf(messageOut, messageOutSize, "too low (%u)", static_cast<unsigned>(avg));
        return;
    }

    baseLevel = avg;
    updated = true;
    succeededOut = true;

    JsonDocument doc;
    doc["sensor"] = sensorName;
    doc["base_level"] = avg;
    doc["sample_count"] = history.count;
    JsonArray samples = doc["burst_samples_mv"].to<JsonArray>();
    for (uint16_t i = 0; i < rawCount; ++i) {
        samples.add(rawMv[i]);
    }
    String payload;
    serializeJson(doc, payload);
    mqtt.publishCalibrate(payload);
}

// MQTT cmd/calibrate AND the Settings-menu "Calibrate CS" item share this exact logic (see
// CalibrateOutcome's doc) — captures each sensor's base as the average of its last
// CURRENT_RMS_HISTORY_LEN burst readings, saved to settings (see Settings.h's doc on why these
// two fields are device-owned). Requires an actual watering run — not just the pump alone — so
// the captured base reflects real load rather than a dry/idle reading; a pump-only run still
// energizes the pump but proves nothing about typical loaded current. `outcome`, if non-null, is
// filled in for callers that need more than the MQTT side-effects this always performs.
void handleCalibrateCommand(uint32_t nowMs, CalibrateOutcome* outcome) {
    const auto snapshot = irrigation.snapshot(nowMs);
    if (snapshot.state != irrigator::domain::IrrigationState::Watering) {
        reportMqttError("command", "calibrate",
                        "Make sure the valve is open - better if the pump is running too.");
        if (outcome != nullptr) {
            outcome->preconditionFailed = true;
            snprintf(outcome->valveMessage, sizeof(outcome->valveMessage), "not open");
            snprintf(outcome->pumpMessage, sizeof(outcome->pumpMessage), "not running");
        }
        return;
    }

    bool updated = false;
    bool valveSucceeded = false;
    bool pumpSucceeded = false;
    char valveMessage[16] = {};
    char pumpMessage[16] = {};
    calibrateOneSensor("valve", valveRmsHistory, lastValveBurstRawMv, lastValveBurstRawCount,
                       settings.valveCurrentBaseLevel, updated, valveSucceeded, valveMessage, sizeof(valveMessage));
    calibrateOneSensor("pump", pumpRmsHistory, lastPumpBurstRawMv, lastPumpBurstRawCount,
                       settings.pumpCurrentBaseLevel, updated, pumpSucceeded, pumpMessage, sizeof(pumpMessage));
    if (outcome != nullptr) {
        outcome->valveSucceeded = valveSucceeded;
        outcome->valveBaseLevel = settings.valveCurrentBaseLevel;
        snprintf(outcome->valveMessage, sizeof(outcome->valveMessage), "%s", valveMessage);
        outcome->pumpSucceeded = pumpSucceeded;
        outcome->pumpBaseLevel = settings.pumpCurrentBaseLevel;
        snprintf(outcome->pumpMessage, sizeof(outcome->pumpMessage), "%s", pumpMessage);
    }

    if (updated) {
        persistSettings();
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
        mqtt.publishWateringPercentage(settings.wateringPercentage);
        return;
    }

    // Test-only escape hatch: runs the weather fetch + percentage/rain-skip adjustment right now,
    // bypassing the schedule-today / lead-time-window / retry-pacing gates that runAutoAdjustmentIfDue
    // normally waits on — same background task, same settings.json write, same /status report at
    // the end (success or failure) so the whole pipeline is exercisable on demand without waiting
    // for the daily window.
    if (commandName == "weather") {
        if (weatherTaskInFlight) {
            reportMqttError("command", commandName, "a weather fetch is already in progress");
            return;
        }
        if (!settings.locationKnown) {
            reportMqttError("command", commandName, "location not known yet; geolocation hasn't resolved");
            return;
        }
        struct tm timeinfo;
        if (!clockService.now(timeinfo)) {
            reportMqttError("command", commandName, "clock not synced yet");
            return;
        }
        char today[11];
        snprintf(today, sizeof(today), "%04d-%02d-%02d",
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        weatherManualTriggerRequested = true;
        if (!startWeatherFetch(today, clockService.epoch())) {
            weatherManualTriggerRequested = false;
            reportMqttError("command", commandName, "failed to start weather fetch task");
        }
        return;
    }

    if (commandName.startsWith("download/")) {
        handleMqttDownload(commandName.substring(String("download/").length()));
        return;
    }

    // Summary only — cheap, and usually all you need to know which commit to look at. The image
    // itself comes from cmd/download/coredump. See docs/coredump.md.
    if (commandName == "coredump") {
        publishCoreDumpSummary();
        return;
    }

    if (commandName == "coredump_clear") {
        // Confirmation-gated exactly like cmd/delete: this is the one irreversible thing you can
        // do to a crash dump, every publish that carried it out was QoS 0, and until it's erased
        // the dump survives any number of reboots. See CoreDumpService::clear()'s comment for why
        // this is never done automatically after a transfer.
        String trimmed = body;
        trimmed.trim();
        if (trimmed != "true") {
            reportMqttError("command", commandName, "payload must be 'true' to confirm erase");
            return;
        }
        if (coreDumpTransfer.active) {
            reportMqttError("command", commandName, "a transfer is in progress");
            return;
        }
        if (!coreDumpService.clear()) {
            reportMqttError("command", commandName, "erase failed");
            return;
        }
        coreDumpPresent = false;
        mqtt.publishAlert(R"({"coredump":"cleared","info":"stored core dump erased"})");
        return;
    }

    if (commandName.startsWith("upload/")) {
        handleMqttUpload(commandName.substring(String("upload/").length()), payload, length);
        return;
    }

    if (commandName.startsWith("delete/")) {
        handleMqttDelete(commandName.substring(String("delete/").length()), body);
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
        // No "only persist if the value changed" guard here, deliberately. If RAM and flash have
        // diverged (RAM already `true`, flash still `false` from an earlier failed/skipped write),
        // that guard would see RAM already matching `newValue` and skip the write entirely — so the
        // user setting it "true" again would do nothing, flash would stay `false`, and the schedule
        // would keep reverting on every reboot with no way to fix it from HA. Always writing makes a
        // re-toggle a reliable way to force flash back in sync; persistSettings() surfaces any write
        // failure via its own alert, so a failed save is no longer silent.
        setScheduleActive(newValue, "mqtt_cmd_scheduled");
        persistSettings();
        mqtt.publishScheduled(settings.scheduleActive);
        return;
    }

    if (commandName == "auto_adjustment") {
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
        settings.autoAdjustmentEnabled = newValue;
        persistSettings();
        mqtt.publishAutoAdjustmentEnabled(settings.autoAdjustmentEnabled);
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

    if (commandName == "calibrate") {
        handleCalibrateCommand(nowMs);
        return;
    }

    if (commandName == "meter") {
        publishMeterDiagnostics(nowMs);
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
        // Self-migrating: a settings.json written before the Zones/Location/Current_sensor/
        // Watering restructuring still parses fine (parseSettings() falls back to each field's
        // old flat top-level key — see Settings.h's mergeSettings/parseSettings doc), but the file
        // on flash is still the old flat shape. Detect that once here and rewrite it in the new
        // nested shape immediately, so the flat-fallback path is only ever exercised on this one
        // boot, not every boot thereafter.
        if (raw.indexOf("\"Watering\"") < 0) {
            irrigator::drivers::Log.info("settings.json predates nested groups; migrating to Zones/Location/Current_sensor/Watering");
            persistSettings();
        }
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
        irrigator::drivers::Log.error("schedule.json invalid; treating as empty (no scheduled runs)");
        scheduleList = {};
        // Deliberately does NOT force settings.scheduleActive=false. That used to mutate the in-RAM
        // flag WITHOUT persisting it, so RAM and flash diverged: a later persistSettings() (e.g. the
        // weather auto-adjustment's) would then write that false to flash, permanently disabling the
        // schedule even after schedule.json was fixed. An empty scheduleList already prevents any
        // scheduled run on its own (findNextScheduledZone returns 0), so the flag needn't be touched.
    }
}

void connectWifiOnce() {
    WiFi.mode(WIFI_STA);
    // Disable Wi-Fi modem sleep: costs more average current, but avoids the periodic
    // beacon-interval current spikes that were showing up as a bursty load on the 3.3V rail.
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.setTxPower(WIFI_POWER_17dBm);
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
    // Seed the state/echo topics HA displays from, on every (re)connect. HA no longer writes
    // schedule_active back to the device on sync (that was disabling schedules on reconnect), so
    // the device is now the sole publisher of /scheduled — without this seed a virgin deploy or an
    // HA restart while the device stays up would leave the switch blank. /scheduled is retained
    // (see publishScheduled()); /auto_adjustment and /watering_percentage deliberately are not, so
    // this seed is also the only way a subscriber that's already listening gets those two promptly
    // on reconnect, rather than waiting for the next actual change.
    mqtt.setConnectedHandler([]() {
        // Paced, not fired back-to-back: every publish failure this device has reported was the
        // last small write of this burst, going into a socket only milliseconds old. See
        // MqttService::kConnectSeedSpacingMs.
        constexpr uint32_t kSeedSpacingMs = irrigator::network::MqttService::kConnectSeedSpacingMs;
        mqtt.publishScheduled(settings.scheduleActive);
        delay(kSeedSpacingMs);
        mqtt.publishAutoAdjustmentEnabled(settings.autoAdjustmentEnabled);
        delay(kSeedSpacingMs);
        mqtt.publishWateringPercentage(settings.wateringPercentage);
        delay(kSeedSpacingMs);
        // One-shot per boot: report what schedule_active was actually loaded from flash, so the
        // change-tracer timeline in sensor.irrigator_last_alert has a known starting point. If this
        // says `false` right after a reboot that followed a confirmed `true`, the flip happened on
        // flash before this boot (an earlier session wrote it); if it says `true` and a later
        // "schedule_active_changed_to:false" alert follows, the flip is happening live this session.
        static bool bootValueReported = false;
        if (!bootValueReported) {
            bootValueReported = true;
            char msg[96];
            snprintf(msg, sizeof(msg), R"({"boot_loaded_schedule_active":%s})",
                     settings.scheduleActive ? "true" : "false");
            mqtt.publishAlert(msg);
        }
    });
    mqtt.begin(mqttConfig);

    ota.setStartedHandler([]() {
        // DISABLED 2026-08-09: abortNoiseFloorSamplingForOta() went away with the rest of the
        // noise-floor/SNR feature (see the block near cachedNoiseFloorDbm) — promiscuous mode is
        // no longer ever enabled, so there is nothing left to stop before the blocking OTA runs.
        mqtt.publishOtaStarted();
    });
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

// Earliest active schedule start (minutes since midnight) for the given ISO day-of-week, or -1 if
// nothing's scheduled that day — used to derive the auto-adjustment lead-time window's opening.
// Mirrors findNextScheduledZone's HH:MM digit-arithmetic (already validated at load time).
int earliestScheduleStartMinutesToday(uint8_t isoDayOfWeek) {
    int best = -1;
    for (uint8_t i = 0; i < scheduleList.count; ++i) {
        const auto& record = scheduleList.records[i];
        if (!record.active || record.dayOfWeek != isoDayOfWeek) {
            continue;
        }
        const int hour = (record.startTime[0] - '0') * 10 + (record.startTime[1] - '0');
        const int minute = (record.startTime[3] - '0') * 10 + (record.startTime[4] - '0');
        const int minutes = hour * 60 + minute;
        if (best < 0 || minutes < best) {
            best = minutes;
        }
    }
    return best;
}

// One-time (per boot, until it succeeds) ip-api.com lookup — Phase 1/2: weather calls stay
// disabled (see runAutoAdjustmentIfDue's settings.locationKnown guard) until this resolves. It
// also settles config.json's timezone, which is why the clock has to be running first (below).
void tryGeolocateIfDue(uint32_t nowMs) {
    if (settings.locationKnown || WiFi.status() != WL_CONNECTED) {
        return;
    }
    // Wait for a set clock (NTP or the DS3231 fallback) before spending the lookup. ip-api reports
    // the DST-inclusive offset for *now*, so converting it to the standard offset config.json
    // stores needs a trustworthy "now" to ask isEuDstActive about — and getting that wrong bakes a
    // permanent one-hour error into every schedule, since this only resolves once per device. The
    // retry timer below brings us back in 5 minutes; nothing downstream can run without a clock
    // anyway (see weatherAutoAdjustStatus's "no_clock").
    struct tm clockProbe;
    if (!clockService.now(clockProbe)) {
        return;
    }
    if (geoAttempted && nowMs - lastGeoAttemptMs < irrigator::board::AUTOADJUSTMENT_RETRY_INTERVAL_MS) {
        return;
    }
    geoAttempted = true;
    lastGeoAttemptMs = nowMs;

    const auto geo = weatherService.fetchGeoLocation();
    if (!geo.success) {
        irrigator::drivers::Log.warn("Geolocation attempt failed; will retry");
        // clockProbe, not a second clockService.now(): the guard above already established the
        // clock is set, and the once-a-day alert only needs today's date, which the intervening
        // HTTP attempt can't have moved off the day the probe read.
        char today[11];
        snprintf(today, sizeof(today), "%04d-%02d-%02d",
                clockProbe.tm_year + 1900, clockProbe.tm_mon + 1, clockProbe.tm_mday);
        if (strcmp(lastGeoFailureAlertDate, today) != 0) {
            snprintf(lastGeoFailureAlertDate, sizeof(lastGeoFailureAlertDate), "%s", today);
            // Sized for geo.errorReason's 200-byte capacity (WeatherService.h) plus the JSON
            // wrapper, so the diagnostic detail that buffer holds doesn't get cut off here.
            char alertMsg[256];
            snprintf(alertMsg, sizeof(alertMsg), R"({"geolocation":"failed","reason":"%s"})", geo.errorReason);
            mqtt.publishAlert(alertMsg);
        }
        return;
    }
    settings.locationKnown = true;
    settings.latitude = geo.latitude;
    settings.longitude = geo.longitude;
    snprintf(settings.city, sizeof(settings.city), "%s", geo.city);
    persistSettings();
    irrigator::drivers::Log.printf(
        "[INFO] Geolocation resolved: %s (%.4f, %.4f)\n", settings.city, settings.latitude, settings.longitude);

    // Same lookup settles the timezone the captive portal no longer asks for. Applied to the live
    // clock immediately (so today's schedule already runs on local wall-clock time) and persisted
    // so the next boot starts there instead of back at UTC.
    const int8_t resolvedOffsetHours = irrigator::domain::standardUtcOffsetHoursFromCurrent(
        geo.utcOffsetSeconds, static_cast<int64_t>(clockService.epoch()));
    if (resolvedOffsetHours != deviceConfig.timezoneOffsetHours) {
        irrigator::drivers::Log.printf("[INFO] Timezone from location: UTC%+d (was UTC%+d)\n",
                                       static_cast<int>(resolvedOffsetHours),
                                       static_cast<int>(deviceConfig.timezoneOffsetHours));
        deviceConfig.timezoneOffsetHours = resolvedOffsetHours;
        clockService.setUtcOffsetHours(resolvedOffsetHours);
        persistConfig();
    }
}

// Step 1's predictive rain-skip: cancels the rest of today's (and tomorrow morning's) scheduled
// runs via the existing RainSkip gate — manual/MQTT cmd/run remain unaffected, same scoping the
// device's own cmd/skip_rain_until already has.
void applyRainSkipDecision(int64_t nowEpoch) {
    const int64_t skipUntil = nowEpoch + irrigator::board::AUTOADJUSTMENT_RAIN_SKIP_DURATION_S;
    rainSkip.setSkipUntil(skipUntil);
    char iso[24];
    irrigator::domain::formatIso8601Utc(skipUntil, iso, sizeof(iso));
    mqtt.publishSkipRainUntil(iso);
    irrigator::drivers::Log.info("Auto-adjustment: rain forecast triggered a 24h scheduled-run skip");
}

// Fires at most once per calendar day, on the first failed attempt that day — the retry loop
// below keeps trying every AUTOADJUSTMENT_RETRY_INTERVAL_MS uncapped, which would otherwise spam
// /alert for as long as both providers stay down. force=true (cmd/weather's manual test trigger)
// bypasses the once-a-day cap, since an explicit on-demand test should always report back.
void reportWeatherFailure(const char* today, const char* openMeteoReason, const char* weatherApiReason, bool force) {
    if (!force) {
        if (strcmp(lastWeatherFailureAlertDate, today) == 0) {
            return;
        }
        snprintf(lastWeatherFailureAlertDate, sizeof(lastWeatherFailureAlertDate), "%s", today);
    }

    // Sized for two up-to-199-char reasons (WeatherFetchResult::errorReason's 200-byte capacity)
    // plus the JSON wrapper, so neither reason gets cut off here.
    char alertMsg[448];
    snprintf(alertMsg,
            sizeof(alertMsg),
            R"({"weather":"failed","open_meteo":"%s","weather_api":"%s"})",
            openMeteoReason,
            weatherApiReason);
    mqtt.publishAlert(alertMsg);
}

// The network fetch (HTTPS GET + JSON parse, potentially two providers in sequence) runs on its
// own FreeRTOS task instead of inline in loop() — that call chain can take several seconds on a
// weak/slow link, and blocking the main loop task for that long would stall valve/pump timing,
// the schedule engine, and the UI. Only pure, snapshotted inputs cross into the task (Params) and
// only a small POD result crosses back out (Result via queue); nothing shared (settings, mqtt,
// fileStore, rainSkip, clockService) is touched from the task, so there's nothing to lock.
struct WeatherAutoAdjustParams {
    float latitude;
    float longitude;
    int64_t nowEpoch;
    char today[11];
    char weatherApiKey[48];
    int16_t rainSkipProbabilityPercent;
    int16_t rainSkipAmountMm;
    int16_t lowerLimitPercent;
    int16_t upperLimitPercent;
};

struct WeatherAutoAdjustResult {
    bool success;
    bool rainSkip;
    int16_t newPercentage;
    int64_t nowEpoch;
    char today[11];
    // Matches WeatherFetchResult::errorReason's size (WeatherService.h) — these are straight
    // copies of it; a smaller buffer here would silently truncate the heap/content-length/timing
    // detail that buffer was specifically sized to hold.
    char openMeteoErrorReason[200];
    char weatherApiErrorReason[200];
};

QueueHandle_t weatherResultQueue = nullptr;

void weatherAutoAdjustTask(void* pvParameters) {
    // Heap-allocated by the caller (runAutoAdjustmentIfDue) and freed here — the task outlives
    // the stack frame that queued it.
    std::unique_ptr<WeatherAutoAdjustParams> params(static_cast<WeatherAutoAdjustParams*>(pvParameters));

    WeatherAutoAdjustResult result{};
    result.nowEpoch = params->nowEpoch;
    snprintf(result.today, sizeof(result.today), "%s", params->today);

    auto forecast = weatherService.fetchForecastOpenMeteo(params->latitude, params->longitude, params->nowEpoch);
    if (!forecast.success) {
        irrigator::drivers::Log.warn("Open-Meteo fetch failed; trying WeatherAPI fallback");
        snprintf(result.openMeteoErrorReason, sizeof(result.openMeteoErrorReason), "%s", forecast.errorReason);
        forecast = weatherService.fetchForecastWeatherApi(
            params->latitude, params->longitude, params->nowEpoch, params->weatherApiKey);
    }
    if (!forecast.success) {
        snprintf(result.weatherApiErrorReason, sizeof(result.weatherApiErrorReason), "%s", forecast.errorReason);
        result.success = false;
    } else {
        result.success = true;
        if (irrigator::domain::shouldSkipForRain(forecast.maxRainProbabilityPercent,
                                                 forecast.rainAmountNextWindowMm,
                                                 params->rainSkipProbabilityPercent,
                                                 params->rainSkipAmountMm)) {
            result.rainSkip = true;
        } else {
            result.rainSkip = false;
            result.newPercentage =
                irrigator::domain::computeAutoCorrectedPercentage(forecast.avgPastTempC,
                                                                   forecast.rainPastDaysMm,
                                                                   params->lowerLimitPercent,
                                                                   params->upperLimitPercent);
        }
    }

    xQueueSend(weatherResultQueue, &result, 0);
    vTaskDelete(nullptr);
}

// Drains the background task's result, if any, and applies its decision — the only place that
// touches settings/mqtt/rainSkip/fileStore for the auto-adjustment cycle, so it all stays on the
// single-threaded loop() side.
void consumeWeatherAutoAdjustResult(uint32_t nowMs) {
    if (!weatherTaskInFlight) {
        return;
    }
    WeatherAutoAdjustResult result;
    if (xQueueReceive(weatherResultQueue, &result, 0) != pdTRUE) {
        return;  // Still running.
    }
    weatherTaskInFlight = false;

    if (!result.success) {
        irrigator::drivers::Log.warn("Auto-adjustment: both weather providers failed; retrying in 5 min");
        reportWeatherFailure(result.today, result.openMeteoErrorReason, result.weatherApiErrorReason,
                            weatherManualTriggerRequested);
    } else {
        if (result.rainSkip) {
            applyRainSkipDecision(result.nowEpoch);
        } else if (result.newPercentage != settings.wateringPercentage) {
            settings.wateringPercentage = result.newPercentage;
            mqtt.publishWateringPercentage(settings.wateringPercentage);
        } else if (weatherManualTriggerRequested) {
            // Unchanged result: the automatic cycle stays quiet (nothing new to tell HA), but an
            // explicit cmd/weather should always report what it computed, or "no message" is
            // indistinguishable from "the fetch didn't produce a percentage".
            mqtt.publishWateringPercentage(settings.wateringPercentage);
        }

        snprintf(lastWeatherUpdateDate, sizeof(lastWeatherUpdateDate), "%s", result.today);
        lastWeatherUpdateEpoch = result.nowEpoch;
        persistSettings();
    }

    if (weatherManualTriggerRequested) {
        weatherManualTriggerRequested = false;
        mqtt.publishStatus(buildStatusJson(nowMs));
    }
}

// Shared by weatherAutoAdjustStatus (read-only reporting) and runAutoAdjustmentIfDue (which acts
// on it) so the two "when is a check due" gates can't drift apart. Two paths:
//  - A watering is scheduled today: due once within AUTOADJUSTMENT_LEAD_TIME_MINUTES of the day's
//    first start (checked earlier on cmd/weather's manual trigger, which bypasses this entirely).
//  - Nothing is scheduled today: due once AUTOADJUSTMENT_OFFDAY_CHECK_INTERVAL_S has passed since
//    the last successful check (or immediately if there's never been one), so the weather pipeline
//    and rain-skip still get exercised daily even through a stretch with an empty schedule.
// Returns nullptr when due "on schedule grounds"; callers still need their own
// weatherTaskInFlight/retry-timer gates on top of that.
const char* autoAdjustScheduleGateReason(const struct tm& timeinfo, int64_t nowEpoch) {
    const uint8_t isoDayOfWeek = timeinfo.tm_wday == 0 ? 7 : static_cast<uint8_t>(timeinfo.tm_wday);
    const int firstStartMinutes = earliestScheduleStartMinutesToday(isoDayOfWeek);
    if (firstStartMinutes >= 0) {
        const int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        if (nowMinutes < firstStartMinutes - irrigator::board::AUTOADJUSTMENT_LEAD_TIME_MINUTES) {
            return "waiting_for_window";
        }
        return nullptr;
    }
    if (lastWeatherUpdateEpoch != 0 &&
        nowEpoch - lastWeatherUpdateEpoch < irrigator::board::AUTOADJUSTMENT_OFFDAY_CHECK_INTERVAL_S) {
        return "no_schedule_waiting_recheck";
    }
    return nullptr;
}

// Mirrors runAutoAdjustmentIfDue's gates exactly, read-only — see that function for what each one
// means. Kept as a single source of truth for "why hasn't this run" rather than duplicating the
// reasoning independently and risking the two drifting apart.
const char* weatherAutoAdjustStatus() {
    if (!settings.autoAdjustmentEnabled) {
        return "disabled";
    }
    if (!settings.locationKnown) {
        return "no_location";
    }
    struct tm timeinfo;
    if (!clockService.now(timeinfo)) {
        return "no_clock";
    }
    char today[11];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    if (strcmp(lastWeatherUpdateDate, today) == 0) {
        return "done_today";
    }
    const char* gateReason = autoAdjustScheduleGateReason(timeinfo, clockService.epoch());
    if (gateReason != nullptr) {
        return gateReason;
    }
    if (weatherTaskInFlight) {
        return "fetch_in_progress";
    }
    return "waiting_retry";
}

// Snapshots the settings the fetch needs and hands them to weatherAutoAdjustTask. Shared by the
// daily automatic cycle (runAutoAdjustmentIfDue) and cmd/weather's on-demand manual trigger —
// callers are responsible for their own gating (locationKnown, weatherTaskInFlight, etc.).
bool startWeatherFetch(const char* today, int64_t nowEpoch) {
    if (weatherResultQueue == nullptr) {
        weatherResultQueue = xQueueCreate(1, sizeof(WeatherAutoAdjustResult));
        if (weatherResultQueue == nullptr) {
            irrigator::drivers::Log.error("Failed to create weather auto-adjustment queue");
            return false;
        }
    }

    auto params = std::make_unique<WeatherAutoAdjustParams>();
    params->latitude = settings.latitude;
    params->longitude = settings.longitude;
    params->nowEpoch = nowEpoch;
    snprintf(params->today, sizeof(params->today), "%s", today);
    snprintf(params->weatherApiKey, sizeof(params->weatherApiKey), "%s", settings.weatherApiKey);
    params->rainSkipProbabilityPercent = settings.rainSkipProbabilityPercent;
    params->rainSkipAmountMm = settings.rainSkipAmountMm;
    params->lowerLimitPercent = settings.lowerLimitPercent;
    params->upperLimitPercent = settings.upperLimitPercent;

    weatherTaskInFlight = true;
    // 12KB stack: comfortably covers the TLS handshake + ArduinoJson buffers this needs (the
    // default Arduino loop task stack this used to run inline on is smaller and shared with
    // everything else, which is exactly what moving this off-loop avoids).
    const BaseType_t created = xTaskCreate(
        weatherAutoAdjustTask, "weatherAdj", 12288, params.get(), tskIDLE_PRIORITY + 1, nullptr);
    if (created != pdPASS) {
        irrigator::drivers::Log.error("Failed to start weather auto-adjustment task");
        weatherTaskInFlight = false;
        return false;  // params freed automatically (unique_ptr).
    }
    params.release();  // Ownership now with the task; it frees this on exit.
    return true;
}

// Daily weather-based adjustment cycle. On a day with a watering scheduled, opens
// AUTOADJUSTMENT_LEAD_TIME_MINUTES before its first start (or immediately at boot if that window
// has already passed) and, once open, retries every AUTOADJUSTMENT_WEATHER_RETRY_INTERVAL_MS. On a
// day with nothing scheduled, instead waits for AUTOADJUSTMENT_OFFDAY_CHECK_INTERVAL_S to pass
// since the last successful check. Either way, retries are deliberately uncapped: a forecast that
// only arrives after the first watering has already run still helps later same-day runs or
// tomorrow's, or informs the rain-skip decision in time to save water.
void runAutoAdjustmentIfDue(uint32_t nowMs) {
    consumeWeatherAutoAdjustResult(nowMs);

    if (!settings.autoAdjustmentEnabled) {
        return;  // Master switch is off — scheduled runs keep using the manual watering
                 // percentage untouched, same as before this feature ever ran.
    }
    if (weatherTaskInFlight || !settings.locationKnown) {
        return;  // Fetch already in flight, or no location yet (scheduled runs keep using the
                 // manual watering percentage until geolocation resolves).
    }
    struct tm timeinfo;
    if (!clockService.now(timeinfo)) {
        return;
    }
    char today[11];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    if (strcmp(lastWeatherUpdateDate, today) == 0) {
        return;  // Already completed today's cycle.
    }

    const int64_t nowEpoch = clockService.epoch();
    if (autoAdjustScheduleGateReason(timeinfo, nowEpoch) != nullptr) {
        return;  // Neither the pre-watering window nor the off-day recheck interval is open yet.
    }

    if (weatherAttempted &&
        nowMs - lastWeatherAttemptMs < irrigator::board::AUTOADJUSTMENT_WEATHER_RETRY_INTERVAL_MS) {
        return;
    }
    weatherAttempted = true;
    lastWeatherAttemptMs = nowMs;

    startWeatherFetch(today, nowEpoch);  // Retry paced by lastWeatherAttemptMs above regardless of outcome.
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

    // static, not a stack array: runScheduleEngine() runs on every loop() iteration (the minute
    // gating is inside ScheduleEngine::tick(), not here), and at SCHEDULE_MAX_RECORDS=255 a stack
    // copy would push ~1.5KB onto the default ~8KB loop-task stack on every single tick — the same
    // class of bug persistScheduleAndReport()'s heap-allocated buffer (above) already had to avoid.
    static WateringRequest dueRequests[irrigator::board::SCHEDULE_MAX_RECORDS];
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

// Runs for the lifetime of the device (created once in setup(), never deleted). Blocks on its own
// notification while idle so it costs nothing between waterings (event-driven per agents.md,
// rather than a busy poll) — updateCurrentSampling() below wakes it exactly once per window-open
// transition. Once active it bursts both sensors back-to-back (~200ms of the ~5s period) and
// sleeps the remainder, re-checking currentSamplingWindowActive each cycle so it winds back down
// to blocking-on-notify as soon as the window (including its CURRENT_MEASURE_POST_OFF_MS tail)
// closes. Both burst calls block for real, which is exactly why this work lives in its own task
// instead of loop() or a Ticker callback (see agents.md's no-blocking-in-loop /
// Ticker-callback-discipline rules) — loop() only ever drains currentBurstResultQueue below;
// settings/MQTT/persisted state are never touched from here.
void currentSamplingTask(void*) {
    CurrentBurstResult result;
    for (;;) {
        if (!currentSamplingWindowActive) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const uint32_t burstStartMs = millis();
        result.activeZone = currentSamplingActiveZone;
        result.valveRmsMv = valveCurrentSensor.sampleRmsMv(irrigator::board::CURRENT_RMS_BURST_DURATION_MS,
                                                            irrigator::board::CURRENT_RMS_SAMPLE_INTERVAL_MS,
                                                            result.valveRawMv,
                                                            kCurrentBurstMaxSamples,
                                                            result.valveRawCount);
        result.pumpRmsMv = pumpCurrentSensor.sampleRmsMv(irrigator::board::CURRENT_RMS_BURST_DURATION_MS,
                                                          irrigator::board::CURRENT_RMS_SAMPLE_INTERVAL_MS,
                                                          result.pumpRawMv,
                                                          kCurrentBurstMaxSamples,
                                                          result.pumpRawCount);
        if (currentBurstResultQueue != nullptr) {
            // Non-blocking; if loop() hasn't drained the previous burst yet (queue depth > 1
            // covers normal jitter), drop rather than stall the sampling cadence on a slow loop().
            xQueueSend(currentBurstResultQueue, &result, 0);
        }

        const uint32_t elapsedMs = millis() - burstStartMs;
        const uint32_t remainingMs = elapsedMs >= irrigator::board::CURRENT_RMS_BURST_PERIOD_MS
                                          ? 0
                                          : irrigator::board::CURRENT_RMS_BURST_PERIOD_MS - elapsedMs;
        vTaskDelay(pdMS_TO_TICKS(remainingMs));
    }
}

// Combined valve-or-pump on-window (same CURRENT_MEASURE_POST_OFF_MS tail the old per-output
// fault-monitor windows used) driving currentSamplingTask's burst cadence for the duration of a
// run. currentSamplingActiveZone tracks which zone (0 = none — pump-only, or the
// pump-graceful-shutdown tail after the valve's already closed) the *next* burst's valve reading
// should be attributed to; see drainCurrentBurstResults() below for how that's used.
void updateCurrentSampling(uint32_t nowMs) {
    const auto snapshot = irrigation.snapshot(nowMs);
    const bool active = snapshot.state == irrigator::domain::IrrigationState::Watering || snapshot.pumpOn;
    currentSamplingActiveZone =
        snapshot.state == irrigator::domain::IrrigationState::Watering ? snapshot.activeZone : 0;

    if (active) {
        if (!currentWindowOpen) {
            currentWindowOpen = true;
            currentSamplingWindowActive = true;
            // Fresh window, fresh calibration history — don't let a previous, unrelated run's
            // readings bleed into this one (see CurrentRmsHistory's comment above).
            valveRmsHistory.reset();
            pumpRmsHistory.reset();
            if (currentSamplingTaskHandle != nullptr) {
                xTaskNotifyGive(currentSamplingTaskHandle);
            }
        }
        currentWindowCloseAtMs = nowMs + irrigator::board::CURRENT_MEASURE_POST_OFF_MS;
    } else if (currentWindowOpen && nowMs >= currentWindowCloseAtMs) {
        currentWindowOpen = false;
        currentSamplingWindowActive = false;
    }
}

// Sets `lastAlertDate` to `today` and returns true, but only the first time on a given calendar
// day — a repeat call on the same day returns false without touching `lastAlertDate` again. Same
// once-a-day-per-fault idiom as lastWeatherFailureAlertDate/lastGeoFailureAlertDate above.
bool consumeDailyAlertGate(char* lastAlertDate, size_t lastAlertDateSize, const char* today) {
    if (strcmp(lastAlertDate, today) == 0) {
        return false;
    }
    snprintf(lastAlertDate, lastAlertDateSize, "%s", today);
    return true;
}

// Drains currentBurstResultQueue — loop()-context only, the sole place settings/MQTT/persisted
// state get touched (see currentSamplingTask's comment above for why). Per burst: folds the
// reading into its sensor's calibration history (CurrentRmsHistory, for cmd/calibrate's average),
// keeps the raw samples around for cmd/calibrate's saturation check, and — while a base is
// calibrated (0 = not yet, skip) — checks the reading against base*CURRENT_OVERCURRENT_MULTIPLIER
// (both sensors) and base*CURRENT_VALVE_UNDERCURRENT_MULTIPLIER / base*CURRENT_PUMP_UNDERCURRENT_MULTIPLIER
// (valve/pump respectively), each gated to one /alert per calendar day (consumeDailyAlertGate) AND
// to its own Settings::alert* toggle (Current_sensor group in settings.json — see the Settings
// menu's "Alert pump/valve UC/OC" rows) being on. Valve dedup is per-zone — the sensor is shared
// across all 11 zones, but which zone is responsible for any given burst is always known (only one
// zone is ever open at a time), so a stuck zone 3 today doesn't silence a separately failing zone 5
// later the same day.
void drainCurrentBurstResults() {
    if (currentBurstResultQueue == nullptr) {
        return;
    }
    CurrentBurstResult result;
    while (xQueueReceive(currentBurstResultQueue, &result, 0) == pdTRUE) {
        pumpRmsHistory.push(result.pumpRmsMv);
        lastPumpBurstRawCount = result.pumpRawCount;
        memcpy(lastPumpBurstRawMv, result.pumpRawMv, result.pumpRawCount * sizeof(uint16_t));

        // Only a real zone's reading belongs in the valve's calibration history — excludes the
        // pump-graceful-shutdown tail (valve already closed, activeZone back to 0), so it can't
        // drag a fresh calibration average toward zero right as the window is ending.
        if (result.activeZone != 0) {
            valveRmsHistory.push(result.valveRmsMv);
        }
        lastValveBurstRawCount = result.valveRawCount;
        memcpy(lastValveBurstRawMv, result.valveRawMv, result.valveRawCount * sizeof(uint16_t));

        struct tm timeinfo;
        if (!clockService.now(timeinfo)) {
            continue;  // Can't safely dedup an alert without a valid calendar date.
        }
        char today[11];
        snprintf(today, sizeof(today), "%04d-%02d-%02d",
                timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

        if (result.activeZone != 0 && settings.valveCurrentBaseLevel != 0) {
            const uint8_t zoneIndex = result.activeZone - 1;
            const uint16_t overThreshold = static_cast<uint16_t>(
                settings.valveCurrentBaseLevel * irrigator::board::CURRENT_OVERCURRENT_MULTIPLIER);
            const uint16_t underThreshold = static_cast<uint16_t>(
                settings.valveCurrentBaseLevel * irrigator::board::CURRENT_VALVE_UNDERCURRENT_MULTIPLIER);
            if (settings.alertValveOc && result.valveRmsMv > overThreshold &&
                consumeDailyAlertGate(lastValveOvercurrentAlertDate[zoneIndex],
                                      sizeof(lastValveOvercurrentAlertDate[zoneIndex]), today)) {
                char alertMsg[128];
                snprintf(alertMsg, sizeof(alertMsg),
                        R"({"valve":%u,"fault":"overcurrent","level":%u,"base":%u})",
                        static_cast<unsigned>(result.activeZone),
                        static_cast<unsigned>(result.valveRmsMv),
                        static_cast<unsigned>(settings.valveCurrentBaseLevel));
                mqtt.publishAlert(alertMsg);
            } else if (settings.alertValveUc && result.valveRmsMv < underThreshold &&
                       consumeDailyAlertGate(lastValveUndercurrentAlertDate[zoneIndex],
                                             sizeof(lastValveUndercurrentAlertDate[zoneIndex]), today)) {
                char alertMsg[128];
                snprintf(alertMsg, sizeof(alertMsg),
                        R"({"valve":%u,"fault":"undercurrent","level":%u,"base":%u})",
                        static_cast<unsigned>(result.activeZone),
                        static_cast<unsigned>(result.valveRmsMv),
                        static_cast<unsigned>(settings.valveCurrentBaseLevel));
                mqtt.publishAlert(alertMsg);
            }
        }

        if (settings.pumpCurrentBaseLevel != 0) {
            const uint16_t overThreshold = static_cast<uint16_t>(
                settings.pumpCurrentBaseLevel * irrigator::board::CURRENT_OVERCURRENT_MULTIPLIER);
            const uint16_t underThreshold = static_cast<uint16_t>(
                settings.pumpCurrentBaseLevel * irrigator::board::CURRENT_PUMP_UNDERCURRENT_MULTIPLIER);
            if (settings.alertPumpOc && result.pumpRmsMv > overThreshold &&
                consumeDailyAlertGate(lastPumpOvercurrentAlertDate, sizeof(lastPumpOvercurrentAlertDate), today)) {
                char alertMsg[112];
                snprintf(alertMsg, sizeof(alertMsg),
                        R"({"pump":0,"fault":"overcurrent","level":%u,"base":%u})",
                        static_cast<unsigned>(result.pumpRmsMv),
                        static_cast<unsigned>(settings.pumpCurrentBaseLevel));
                mqtt.publishAlert(alertMsg);
            } else if (settings.alertPumpUc && result.pumpRmsMv < underThreshold &&
                       consumeDailyAlertGate(lastPumpUndercurrentAlertDate,
                                             sizeof(lastPumpUndercurrentAlertDate), today)) {
                char alertMsg[112];
                snprintf(alertMsg, sizeof(alertMsg),
                        R"({"pump":0,"fault":"undercurrent","level":%u,"base":%u})",
                        static_cast<unsigned>(result.pumpRmsMv),
                        static_cast<unsigned>(settings.pumpCurrentBaseLevel));
                mqtt.publishAlert(alertMsg);
            }
        }
    }
}

// Stage 4's lifetime-consumption bookkeeping: folds the delta since the last poll's totalizer
// reading into whichever bucket irrigation.snapshot(nowMs) indicates (see
// irrigator::domain::FlowLifetimeCounters' class comment for the exact zone/"other"/discarded
// rule), then publishes all buckets to /flow_totals - throttled to kFlowTotalsPublishIntervalMs,
// not on every poll.
void updateFlowLifetimeTotals(uint32_t nowMs, const irrigator::drivers::Tuf2000mFlowMeter::Reading& reading) {
    if (haveLastKnownTotalizerM3) {
        const float deltaM3 = reading.totalizerM3 - lastKnownTotalizerM3;
        if (deltaM3 > 0.0f) {
            const auto snapshot = irrigation.snapshot(nowMs);
            const uint8_t activeZone =
                snapshot.state == irrigator::domain::IrrigationState::Watering ? snapshot.activeZone : 0;
            flowLifetimeCounters.addDelta(activeZone, snapshot.pumpOn, deltaM3 * 1000.0f);
        }
    }
    lastKnownTotalizerM3 = reading.totalizerM3;
    haveLastKnownTotalizerM3 = true;

    if (flowTotalsPublishedOnce && nowMs - lastFlowTotalsPublishMs < kFlowTotalsPublishIntervalMs) {
        return;
    }
    flowTotalsPublishedOnce = true;
    lastFlowTotalsPublishMs = nowMs;

    JsonDocument doc;
    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        if (!isZoneEnabled(zone)) {
            continue;
        }
        char key[8];
        snprintf(key, sizeof(key), "zone_%u", static_cast<unsigned>(zone));
        doc[key] = flowLifetimeCounters.zoneLiters(zone);
    }
    doc["other"] = flowLifetimeCounters.otherLiters();
    String payload;
    serializeJson(doc, payload);
    mqtt.publishFlowTotals(payload);
}

// Stage 2 of the flow-meter integration (see docs/technical.md's "Water flow sensing" section):
// gates the /flow publish separately from the Modbus poll cadence above (FLOW_METER_POLL_INTERVAL_MS
// inside the driver) - polling stays cheap and frequent regardless of whether a given poll's result
// actually goes out over MQTT. The actual cadence rule (once/minute while active, threshold-while-
// idle) lives in flowPublishGate (irrigator::domain::FlowPublishGate) - see its class comment.
void updateFlowPublishCadence(uint32_t nowMs, const irrigator::drivers::Tuf2000mFlowMeter::Reading& reading) {
    const auto snapshot = irrigation.snapshot(nowMs);
    const bool active = snapshot.state == irrigator::domain::IrrigationState::Watering || snapshot.pumpOn;
    const uint8_t activeZone =
        snapshot.state == irrigator::domain::IrrigationState::Watering ? snapshot.activeZone : 0;

    const float totalLiters = reading.totalizerM3 * 1000.0f;
    if (!flowPublishGate.shouldPublish(nowMs, active, totalLiters)) {
        return;
    }

    char payload[96];
    snprintf(payload,
            sizeof(payload),
            R"({"flow_rate_lpm":%.3f,"total_liters":%.3f,"zone":%u})",
            static_cast<double>(reading.flowRateM3h * 1000.0f / 60.0f),
            static_cast<double>(totalLiters),
            static_cast<unsigned>(activeZone));
    mqtt.publishFlow(payload);
}

// cmd/meter: publishes the flow meter's latest poll plus the driver's sticky last-Modbus-error to
// /meter. Reading fields are only included when a valid poll exists - never zeros standing in for
// "no data" - and the reply says so explicitly otherwise. The error fields are always present, since
// they matter most exactly when there is no valid reading.
void publishMeterDiagnostics(uint32_t nowMs) {
    JsonDocument doc;
    if (haveValidFlowReading) {
        const auto& reading = lastValidFlowReading;
        doc["flow_rate_lpm"] = reading.flowRateM3h * 1000.0f / 60.0f;
        doc["total_m3"] = reading.totalizerM3;
        doc["signal_strength_up"] = reading.signalStrengthUp;
        doc["signal_strength_down"] = reading.signalStrengthDown;
        doc["quality"] = irrigator::domain::signalQualityValue(reading.signalQuality);
        doc["autogain_step"] = irrigator::domain::signalAutogainStep(reading.signalQuality);
        doc["sound_speed_ms"] = reading.soundSpeedMs;
        doc["reading_age_s"] = (nowMs - reading.timestampMs) / 1000;
    } else {
        doc["error"] = "no valid reading yet";
    }
    doc["last_modbus_error"] = flowMeter.lastErrorCode();
    doc["last_modbus_error_text"] = flowMeter.lastErrorText();

    String payload;
    serializeJson(doc, payload);
    mqtt.publishMeter(payload);
}

// Stage 1 of the flow-meter integration: drives flowMeter's own internal poll throttle every
// loop() tick and logs whatever lands. Stage 2 layers the MQTT /flow publish (cadence-gated
// separately, see updateFlowPublishCadence() above) on top of the same drained reading.
void updateFlowSampling(uint32_t nowMs) {
    flowMeter.readAsync(nowMs);
    if (!flowMeter.responseReady()) {
        return;
    }
    irrigator::drivers::Tuf2000mFlowMeter::Reading reading;
    if (!flowMeter.takeReading(reading)) {
        return;
    }
    if (!reading.valid) {
        irrigator::drivers::Log.warn("Flow meter poll failed (Modbus error/timeout)");
        // Same once-a-day-per-fault idiom as the current-sensor faults (consumeDailyAlertGate) -
        // a wedged transceiver or a disconnected RS-485 cable would otherwise either go completely
        // unreported over MQTT, or spam /alert on every poll for as long as it stays down. Uses
        // {"flow_meter":"failed",...} rather than a {"fault":...} payload deliberately: this is a
        // communication failure (no zone/level/base to report), the same category as
        // reportWeatherFailure()'s {"geolocation"/"weather":"failed",...} alerts above, not a
        // current-style out-of-range reading - and it's why this one isn't picked up by
        // irrigator_automations.yaml's fault-forwarding automation (that one keys specifically on
        // a `fault` field). Skipped entirely without a synced clock - no reliable "today" to dedup
        // against, same trade-off reportWeatherFailure() makes.
        struct tm timeinfo;
        if (clockService.now(timeinfo)) {
            char today[11];
            snprintf(today, sizeof(today), "%04d-%02d-%02d",
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
            if (consumeDailyAlertGate(
                    lastFlowMeterFailureAlertDate, sizeof(lastFlowMeterFailureAlertDate), today)) {
                mqtt.publishAlert(R"({"flow_meter":"failed","reason":"modbus_error_or_timeout"})");
            }
        }
        return;
    }
    lastValidFlowReading = reading;
    haveValidFlowReading = true;
    irrigator::drivers::Log.printf("[flow] rate=%.3f m3/h total=%.3f m3 signal=%u\n",
                                   static_cast<double>(reading.flowRateM3h),
                                   static_cast<double>(reading.totalizerM3),
                                   static_cast<unsigned>(reading.signalQuality));
    updateFlowLifetimeTotals(nowMs, reading);
    updateFlowPublishCadence(nowMs, reading);
}

void updateOutputsFromDomain() {
    const auto snapshot = irrigation.snapshot(millis());
    relayBank.setPump(snapshot.pumpOn);

    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        const bool zoneOn = snapshot.state == irrigator::domain::IrrigationState::Watering &&
                            snapshot.activeZone == zone;
        relayBank.setZone(zone, zoneOn);
    }

    // Current-fault alerting (drainCurrentBurstResults) is MQTT-only for now — not wired to the
    // LED yet.
    if (snapshot.state == irrigator::domain::IrrigationState::Watering) {
        led.setMode(irrigator::drivers::LedMode::Breathe);
    } else if (snapshot.pumpOn) {
        led.setMode(irrigator::drivers::LedMode::SolidOn);
    } else if (led.mode() == irrigator::drivers::LedMode::SolidOn ||
              led.mode() == irrigator::drivers::LedMode::Alert ||
              led.mode() == irrigator::drivers::LedMode::Breathe) {
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

    // Before anything else that could itself crash: if the previous session panicked, the dump is
    // already in flash and this is the cheapest point to notice it. Snapshotted once — see
    // coreDumpPresent's declaration for why this isn't re-checked later.
    coreDumpPresent = coreDumpService.available();
    if (coreDumpPresent) {
        irrigator::drivers::Log.printf(
            "[WARN] A core dump from an earlier crash is stored (%u bytes) - retrieve it with "
            "cmd/download/coredump, see docs/coredump.md\n",
            static_cast<unsigned>(coreDumpService.totalBytes()));
    }

    fileStore.begin();
    relayBank.begin();
    valveCurrentSensor.begin();
    pumpCurrentSensor.begin();

    {
        irrigator::drivers::Tuf2000mFlowMeter::Config flowMeterConfig;
        flowMeterConfig.rxPin = irrigator::board::FLOW_METER_RX_PIN;
        flowMeterConfig.txPin = irrigator::board::FLOW_METER_TX_PIN;
        flowMeterConfig.baudRate = irrigator::board::FLOW_METER_BAUD_RATE;
        flowMeterConfig.slaveId = irrigator::board::FLOW_METER_MODBUS_SLAVE_ID;
        flowMeterConfig.flowRateRegister = irrigator::board::FLOW_METER_REG_FLOW_RATE;
        flowMeterConfig.totalizerRegister = irrigator::board::FLOW_METER_REG_TOTALIZER;
        flowMeterConfig.signalQualityRegister = irrigator::board::FLOW_METER_REG_SIGNAL_QUALITY;
        flowMeterConfig.soundSpeedRegister = irrigator::board::FLOW_METER_REG_SOUND_SPEED;
        flowMeterConfig.calibrationMultiplier = irrigator::board::METER_CALIBRATION_MULTIPLIER;
        flowMeterConfig.pollIntervalMs = irrigator::board::FLOW_METER_POLL_INTERVAL_MS;
        // No Wi-Fi/captive-portal dependency, so - like the current sensors above - this begins
        // unconditionally rather than waiting for enterOperationalMode().
        flowMeter.begin(Serial2, flowMeterConfig);
    }
    // Depth 3 comfortably covers normal loop() jitter between drains; currentSamplingTask drops a
    // burst outright (see its comment) rather than blocking if this ever fills up.
    currentBurstResultQueue = xQueueCreate(3, sizeof(CurrentBurstResult));
    if (currentBurstResultQueue == nullptr) {
        irrigator::drivers::Log.error("Failed to create current-RMS burst result queue");
    }
    // Idle (blocked on its own notification) until the first valve/pump run wakes it — see
    // currentSamplingTask's comment above. Created unconditionally, before the captive-portal
    // early-return below, since it costs nothing while idle either way.
    const BaseType_t currentSamplingTaskCreated = xTaskCreate(
        currentSamplingTask, "currentRms", 4096, nullptr, tskIDLE_PRIORITY + 1, &currentSamplingTaskHandle);
    if (currentSamplingTaskCreated != pdPASS) {
        irrigator::drivers::Log.error("Failed to start current-RMS sampling task");
        currentSamplingTaskHandle = nullptr;
    }
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
                reportCoreDumpAtBoot();
            }
            // No-op unless a cmd/download/coredump is in flight; pumps out one base64 chunk per
            // iteration (see CoreDumpTransfer). Placed right after mqtt.loop() so a chunk is
            // published on a client that has just been serviced, and before the irrigation/UI work
            // below, which it must never displace — it does at most one flash read plus one
            // publish per iteration.
            serviceCoreDumpTransfer();

            if (factoryResetButton.poll()) {
                ui.confirmFactoryReset(nowMs);
            }

            tryGeolocateIfDue(nowMs);
            runAutoAdjustmentIfDue(nowMs);
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
            updateCurrentSampling(nowMs);
            drainCurrentBurstResults();
            updateFlowSampling(nowMs);
            updateOutputsFromDomain();
            sampleWifiRssiIfDue(nowMs);
            // sampleNoiseFloorIfDue(nowMs);  // DISABLED 2026-08-09 (noise-floor/SNR feature)

            {
                irrigator::ui::RenderData renderData;
                renderData.snapshot = irrigation.snapshot(nowMs);
                renderData.settings = &settings;
                renderData.schedule = &scheduleList;
                renderData.scheduleActive = settings.scheduleActive;
                renderData.deviceIp = WiFi.localIP();
                renderData.mqttBrokerText = deviceConfig.mqttBroker;
                renderData.mqttTopicText = mqtt.prefix();
                renderData.wifiRssi = cachedWifiRssi;
                renderData.uptimeSeconds = nowMs / 1000UL;
                renderData.queueCount = wateringQueue.size();
                for (uint8_t i = 0; i < renderData.queueCount; ++i) {
                    renderData.queueItems[i] = wateringQueue.at(i);
                }
                // "DD-MM HH:MM" in local wall time, not main.ino's own lastWeatherUpdateDate global
                // (that stays "YYYY-MM-DD" - it's the once-per-day gating key compared against
                // `today` in weatherAutoAdjustStatus()/runAutoAdjustmentIfDue(), reformatting it
                // would break those strcmp checks). lastWeatherUpdateBuffer only needs to outlive
                // this block - ui.poll() below consumes it synchronously, nothing retains the
                // pointer past that call.
                char lastWeatherUpdateBuffer[12] = "";
                struct tm lastWeatherUpdateLocalTm;
                if (lastWeatherUpdateEpoch != 0 &&
                    clockService.localTimeAt(lastWeatherUpdateEpoch, lastWeatherUpdateLocalTm)) {
                    snprintf(lastWeatherUpdateBuffer,
                            sizeof(lastWeatherUpdateBuffer),
                            "%02d-%02d %02d:%02d",
                            lastWeatherUpdateLocalTm.tm_mday,
                            lastWeatherUpdateLocalTm.tm_mon + 1,
                            lastWeatherUpdateLocalTm.tm_hour,
                            lastWeatherUpdateLocalTm.tm_min);
                }
                renderData.lastWeatherUpdateDisplay = lastWeatherUpdateBuffer;

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