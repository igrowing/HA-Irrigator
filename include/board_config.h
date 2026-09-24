#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef APP_VERSION
#define APP_VERSION "1.2.1"
#endif

namespace irrigator::board {

static constexpr const char* FIRMWARE_VERSION = APP_VERSION;

// I2C bus shared by DS3231 RTC and SSD1309 OLED (PCF8574 relay expander retired — unreliable
// on this bus; zones 1-8 now drive direct GPIO like zones 9-11 always did).
static constexpr uint8_t I2C_SDA_PIN = 21;
static constexpr uint8_t I2C_SCL_PIN = 22;
static constexpr uint32_t I2C_CLOCK_HZ = 400000UL;

static constexpr uint8_t DS3231_ADDRESS = 0x68;
static constexpr uint8_t SSD1309_ADDRESS = 0x3C;

static constexpr uint8_t ZONE_COUNT = 11;
// Zones 1-11, all active-low, direct GPIO. Avoids boot-strapping pins 0/2/12.
static constexpr uint8_t GPIO_ZONE_PINS[ZONE_COUNT] = {4, 16, 17, 5, 18, 19, 23, 25, 26, 27, 13};
static constexpr uint8_t PUMP_RELAY_PIN = 15;

static constexpr uint8_t STATUS_LED_PIN = 14;

static constexpr uint8_t ROTARY_CLK_PIN = 34;
static constexpr uint8_t ROTARY_DAT_PIN = 39;
static constexpr uint8_t ROTARY_BUTTON_PIN = 35;
static constexpr uint16_t ROTARY_LONG_CLICK_MS = 1000;

// The board's BOOT button, repurposed as a factory-reset trigger.
static constexpr uint8_t FACTORY_RESET_BUTTON_PIN = 0;
static constexpr uint32_t FACTORY_RESET_HOLD_MS = 10000UL;
static constexpr uint32_t FACTORY_RESET_RESULT_DISPLAY_MS = 3000UL;

static constexpr uint8_t VALVE_CURRENT_ADC_PIN = 32;
static constexpr uint8_t PUMP_CURRENT_ADC_PIN = 33;
static constexpr uint8_t ADC_RESOLUTION_BITS = 12;

static constexpr uint16_t ACTIVE_INTERACTION_TIMEOUT_MS = 20000;
static constexpr uint16_t PUMP_PRESTART_MS = 500;
static constexpr uint32_t PUMP_GRACEFUL_SHUTDOWN_MS = 60000UL;
static constexpr uint16_t CURRENT_MEASURE_POST_OFF_MS = 10000;
// True-RMS current sampling: a background task bursts CURRENT_RMS_SAMPLE_INTERVAL_MS-spaced ADC
// reads for CURRENT_RMS_BURST_DURATION_MS (at 1ms/1kHz that's ~5-6 full 50/60Hz mains cycles,
// comfortably enough to compute a real RMS rather than an instantaneous single-sample guess),
// repeating every CURRENT_RMS_BURST_PERIOD_MS while a valve/pump is on (plus the
// CURRENT_MEASURE_POST_OFF_MS tail above). Each burst's RMS is a "virtual" level in raw millivolts,
// zeroed against that same burst's own (min+max)/2 rather than a fixed board constant — there's no
// mA conversion at all (see cmd/calibrate below); a burst's level is only ever compared against a
// per-sensor base captured under real load. See src/drivers/CurrentSensor.h's sampleRmsMv().
static constexpr uint16_t CURRENT_RMS_BURST_DURATION_MS = 100;
static constexpr uint16_t CURRENT_RMS_SAMPLE_INTERVAL_MS = 1;
static constexpr uint32_t CURRENT_RMS_BURST_PERIOD_MS = 5000;
// MQTT cmd/calibrate captures a sensor's "base" as the average of the most recent bursts — at one
// burst per CURRENT_RMS_BURST_PERIOD_MS (5s), 12 covers the trailing ~minute.
static constexpr uint8_t CURRENT_RMS_HISTORY_LEN = 12;
// cmd/calibrate rejects (doesn't save) a captured average below this floor — a real valve/pump
// under load should read well above ADC noise, so a near-zero average usually means the output
// wasn't actually drawing current (wiring fault, relay not energizing) rather than a genuinely
// low-draw device. PLACEHOLDER — needs tuning against real hardware noise floor (agents.md Step 5).
static constexpr uint16_t CURRENT_CALIBRATION_MIN_BASE_LEVEL = 30;
// Fault thresholds as multipliers of the calibrated base (see above), not fixed mA bands — checked
// per burst while a base is set (0 = uncalibrated = no check). Each of the four checks (valve/pump
// x over/under) is additionally gated by its own Settings::alert* toggle (Current_sensor group in
// settings.json) — see drainCurrentBurstResults() in main.ino.
static constexpr float CURRENT_OVERCURRENT_MULTIPLIER = 2.9f;
static constexpr float CURRENT_VALVE_UNDERCURRENT_MULTIPLIER = 0.5f;
static constexpr float CURRENT_PUMP_UNDERCURRENT_MULTIPLIER = 0.5f;
// 15 covers "Test all zones" queuing up to ZONE_COUNT (11) requests behind whatever's already
// running/queued, with headroom to spare. WateringRequest is a few bytes, so the extra slots
// cost tens of bytes total — a non-issue against the ESP32's RAM budget.
static constexpr uint8_t WATERING_QUEUE_DEPTH = 15;

// --- TUF-2000M ultrasonic flow meter (Modbus RTU/RS-485) ---
// See hardware/readme.md's "Water flow sensing" section for the wiring/converter this talks to
// (Waveshare TTL-to-RS485 (C), auto direction switching - no DE/RE GPIO needed) and
// src/drivers/Tuf2000mFlowMeter.h for the driver built on top of these. RX/TX are the only two
// strapping-safe GPIOs left free on this board once zones/pump/LED/rotary/ADC/I2C are all
// accounted for (see the pin groups above).
static constexpr uint8_t FLOW_METER_RX_PIN = 36;
static constexpr uint8_t FLOW_METER_TX_PIN = 2;
static constexpr uint32_t FLOW_METER_BAUD_RATE = 9600;
static constexpr uint8_t FLOW_METER_MODBUS_SLAVE_ID = 1;
// Throttles Tuf2000mFlowMeter::readAsync() - independent of any later MQTT publish cadence (that's
// a Stage 2+ concern layered on top; every poll here still needs to happen regardless of whether
// its result gets published). Short enough for a bench sanity-check ("two back-to-back reads while
// flow is genuinely unchanged agree" - see the driver's class comment) without hammering the bus.
static constexpr uint32_t FLOW_METER_POLL_INTERVAL_MS = 2000;
// Holding-register map (function code 03), per the register map documented by
// github.com/tczerwonka/tuf2000-modbus-reader - cross-check against the specific unit during
// bring-up since TUF-2000M firmware batches can vary. Flow rate/totalizer are REAL4 (IEEE-754
// float) values spanning 2 words each, word-swapped before reinterpreting as a float (see
// Tuf2000mFlowMeter::decodeSwappedFloat()); signal quality is a single word (high byte: autogain
// step, low byte: the actual 0-100 quality number).
static constexpr uint16_t FLOW_METER_REG_FLOW_RATE = 1;
static constexpr uint16_t FLOW_METER_REG_TOTALIZER = 9;
static constexpr uint16_t FLOW_METER_REG_SIGNAL_QUALITY = 92;
// Ceiling for one whole poll cycle (3 sequential register reads - see the driver): eModbus's own
// per-request timeout is 2000ms, so this covers all three timing out in a row plus margin. If
// Tuf2000mFlowMeter::readAsync() finds a poll still in flight past this, it force-clears and lets
// the next throttle window retry rather than wedging every future poll on a stuck transceiver/bug.
static constexpr uint32_t FLOW_METER_STUCK_REQUEST_TIMEOUT_MS = 8000;

static constexpr uint8_t MANUAL_DURATION_MIN_MINUTES = 1;
static constexpr uint8_t MANUAL_DURATION_MAX_MINUTES = 120;
static constexpr uint8_t SCHEDULE_DURATION_MIN_MINUTES = 1;
static constexpr uint8_t SCHEDULE_DURATION_MAX_MINUTES = 60;
static constexpr uint8_t DEFAULT_MANUAL_DURATION_MINUTES = 5;
static constexpr int16_t DEFAULT_WATERING_PERCENTAGE = 100;

// 255 is the exact ceiling a uint8_t record count/index can hold — see Schedule.h's
// ScheduleList::count and UiIntent::scheduleRecordIndex, both uint8_t. Going any higher would
// require widening those too, not just this constant.
static constexpr uint8_t SCHEDULE_MAX_RECORDS = 255;
static constexpr int8_t TIMEZONE_OFFSET_MIN_HOURS = -13;
static constexpr int8_t TIMEZONE_OFFSET_MAX_HOURS = 13;
static constexpr uint16_t DEFAULT_MQTT_PORT = 1883;

static constexpr size_t CONFIG_JSON_MAX_BYTES = 768;
// Bumped from 1280: settings.json is now nested into Zones/Location/Current_sensor/Watering
// groups (extra object overhead per field) plus 4 new alert_* booleans under Current_sensor.
static constexpr size_t SETTINGS_JSON_MAX_BYTES = 1536;
// Worst case per record is ~81 bytes (2-digit zone/duration, "false"): 255 * 81 + 2 (brackets) +
// 254 (commas) ~= 20.9KB. Rounded up to 24KB for margin. This is heap-allocated (see main.ino's
// persistScheduleAndReport), not a stack buffer, so headroom here doesn't cost stack space —
// undersizing it would silently truncate serializeJson()'s output instead of failing loudly, so
// err generous rather than tight.
static constexpr size_t SCHEDULE_JSON_MAX_BYTES = 24576;

// --- Weather-based irrigation auto-adjustment ---
static constexpr int16_t AUTOADJUSTMENT_DEFAULT_UPPER_LIMIT_PERCENT = 200;
static constexpr int16_t AUTOADJUSTMENT_DEFAULT_LOWER_LIMIT_PERCENT = 40;
// schedule.json's per-zone duration_min is already the base runtime at BASE_TEMP. Note what
// BASE_TEMP means: the *average daily maximum* over the trailing window that a "normal" day
// reads — NOT a 24h mean. The formula's input switched from an hourly mean to daily max in
// af3497b while BASE_TEMP stayed at 20, which silently added a near-constant bias: over Apr-Sep
// 2026 at this site the season-mean gap Tmax - Tmean is 5.2°C, i.e. +52 percentage points at the
// old 10 %/°C. Recalibrated 2026-09-19 to pivot the line rather than shift it (raising the base
// alone would have dropped peak summer out of the 200% cap that is deliberately relied on), then
// re-fitted the same day when the history source moved to the Open-Meteo ERA5 archive, then
// re-fitted again (2026-09-21) so cool spells keep watering at the 40% floor instead of skipping:
// raw percentage now hits the 200% cap at 31.0°C and crosses below the 40% skip threshold at
// 18.2°C, both holding rainfall at 0mm — see docs/usage_irrigator.md's Step 2 table.
// These constants are calibrated against *archive* temperatures/rainfall specifically: the
// forecast endpoint's own past_days series runs ~1°C cooler and reports ~6mm more rain over the
// same five days, which is worth ~25 percentage points — do not feed this formula from a
// different source without re-fitting (see docs/technical.md's "Calibration").
// RAIN_MODIFIER is entangled with the temperature terms (it was partly compensating the old
// bias) and stays a candidate for re-tuning — see docs/technical.md.
// All three are compile-time only: a per-device override via settings.json's Watering group is
// currently disabled (commented out in Settings.cpp), so tuning them needs a firmware flash.
static constexpr float AUTOADJUSTMENT_BASE_TEMP_C = 23.0f;
static constexpr float AUTOADJUSTMENT_DEFAULT_TEMP_MODIFIER_PCT_PER_DEGREE = 12.5f;
static constexpr float AUTOADJUSTMENT_DEFAULT_RAIN_MODIFIER_PCT_PER_MM = 3.1f;

// Rain-skip (Step 1) is the only *forecast*-based check — probability is inclusive (>=), amount
// is not (>), matching shouldSkipForRain()'s exact comparison. Both thresholds are user-configurable
// (Settings::rainSkipProbabilityPercent/rainSkipAmountMm) — these are only the factory defaults.
static constexpr int16_t AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_PROBABILITY_PERCENT = 70;
static constexpr int16_t AUTOADJUSTMENT_DEFAULT_RAIN_SKIP_AMOUNT_MM = 4;
static constexpr int64_t AUTOADJUSTMENT_RAIN_SKIP_DURATION_S = 24LL * 3600;

// The correction formula's (Step 2) temperature and rain terms are both historical, over this
// same trailing window — a rolling 5-day average temperature and a 5-day rainfall total. Only
// the rain-skip check above looks forward.
static constexpr uint8_t AUTOADJUSTMENT_PAST_WEATHER_DAYS = 5;
static constexpr uint8_t AUTOADJUSTMENT_FORECAST_WINDOW_HOURS = 12;
// How far ahead of the day's first scheduled watering the fetch/retry cycle starts, on days that
// have a watering scheduled. Kept short so the reading stays close to watering time; the retry
// pacing below is correspondingly tight (5 attempts fit in this window) rather than the old
// 3h/5min combo.
static constexpr int AUTOADJUSTMENT_LEAD_TIME_MINUTES = 5;
// Retries are deliberately unbounded (not capped at a fixed attempt count): a forecast that
// arrives late still helps later same-day waterings or tomorrow's, so this is just the pacing
// between attempts, not a timeout. Used for the pre-watering check above.
static constexpr uint32_t AUTOADJUSTMENT_WEATHER_RETRY_INTERVAL_MS = 1UL * 60UL * 1000UL;
// Geolocation's own retry pacing (separate concern from the weather-fetch retry above).
static constexpr uint32_t AUTOADJUSTMENT_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;
// On days with nothing scheduled, how often to check anyway — keeps the weather pipeline
// (and its failure alerts) exercised daily instead of going silent for however long the
// schedule stays empty, and lets a rain-skip decision land ahead of tomorrow's first watering
// rather than waiting for that day's own 5-minute pre-watering window.
static constexpr int64_t AUTOADJUSTMENT_OFFDAY_CHECK_INTERVAL_S = 24LL * 3600;

// Bounds on every outbound HTTP/HTTPS call (weather providers, ip-api geolocation). Without these
// the ESP32 HTTPClient default 5s connect timeout applies but nothing bounds the TLS handshake,
// and a stalled handshake was observed blocking the weather task for 70 SECONDS - holding its
// ~60KB of TLS buffers the whole time. The retry loop above already re-attempts every 5 minutes,
// so failing fast costs nothing and giving up slowly costs a lot.
static constexpr int32_t HTTP_CONNECT_TIMEOUT_MS = 8000;
static constexpr uint16_t HTTP_RESPONSE_TIMEOUT_MS = 8000;
static constexpr unsigned long TLS_HANDSHAKE_TIMEOUT_S = 10;

// `offset` is the located region's CURRENT UTC offset in seconds, DST already folded in — it's
// what replaces the timezone dropdown the captive portal used to ask for (see
// domain::standardUtcOffsetHoursFromCurrent for how it's turned back into the plain standard-time
// offset config.json stores).
static constexpr const char* GEO_IP_API_URL = "http://ip-api.com/json/?fields=status,lat,lon,city,offset";
static constexpr const char* OPEN_METEO_BASE_URL = "https://api.open-meteo.com/v1/forecast";
// ERA5 reanalysis - the source of the correction formula's *historical* inputs (avg daily max
// temperature, rainfall). Unlike the forecast endpoint's past_days series (the weather model's own
// output for those hours, never revised), ERA5 assimilates real observations after the fact. The
// forecast endpoint above is still used for the forward-looking rain-skip window.
static constexpr const char* OPEN_METEO_ARCHIVE_URL = "https://archive-api.open-meteo.com/v1/archive";
static constexpr const char* WEATHERAPI_FORECAST_URL = "https://api.weatherapi.com/v1/forecast.json";
static constexpr const char* WEATHERAPI_HISTORY_URL = "https://api.weatherapi.com/v1/history.json";

static constexpr const uint8_t DISPLAY_1ST_LINE_OFFSET = 7;
static constexpr const uint8_t DISPLAY_2ND_LINE_OFFSET = 28;
static constexpr const uint8_t DISPLAY_3RD_LINE_OFFSET = 50;
static constexpr const uint8_t DISPLAY_4TH_LINE_OFFSET = 63;

static constexpr const char* CONFIG_FILE_PATH = "/config.json";
static constexpr const char* SETTINGS_FILE_PATH = "/settings.json";
static constexpr const char* SCHEDULE_FILE_PATH = "/schedule.json";

}  // namespace irrigator::board