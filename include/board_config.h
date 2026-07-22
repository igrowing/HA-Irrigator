#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef APP_VERSION
#define APP_VERSION "1.0.2"
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

static constexpr uint16_t VALVE_CURRENT_MIN_MA = 20;
static constexpr uint16_t VALVE_CURRENT_MAX_MA = 800;
static constexpr uint16_t PUMP_CURRENT_MIN_MA = 200;
static constexpr uint16_t PUMP_CURRENT_MAX_MA = 1800;

// PLACEHOLDERS — not a real calibration. Tune against the actual installed current-sensor
// modules on real hardware (see agents.md Step 5); readMilliAmps() is meaningless until then.
static constexpr uint16_t VALVE_CURRENT_SENSOR_ZERO_OFFSET_MV = 1650;
static constexpr float VALVE_CURRENT_SENSOR_MV_PER_MA = 1.0f;
static constexpr uint16_t PUMP_CURRENT_SENSOR_ZERO_OFFSET_MV = 1650;
static constexpr float PUMP_CURRENT_SENSOR_MV_PER_MA = 1.0f;

static constexpr uint16_t ACTIVE_INTERACTION_TIMEOUT_MS = 20000;
static constexpr uint16_t PUMP_PRESTART_MS = 500;
static constexpr uint32_t PUMP_GRACEFUL_SHUTDOWN_MS = 60000UL;
static constexpr uint16_t CURRENT_MEASURE_POST_OFF_MS = 10000;
// 15 covers "Test all zones" queuing up to ZONE_COUNT (11) requests behind whatever's already
// running/queued, with headroom to spare. WateringRequest is a few bytes, so the extra slots
// cost tens of bytes total — a non-issue against the ESP32's RAM budget.
static constexpr uint8_t WATERING_QUEUE_DEPTH = 15;

static constexpr uint8_t MANUAL_DURATION_MIN_MINUTES = 1;
static constexpr uint8_t MANUAL_DURATION_MAX_MINUTES = 120;
static constexpr uint8_t SCHEDULE_DURATION_MIN_MINUTES = 1;
static constexpr uint8_t SCHEDULE_DURATION_MAX_MINUTES = 60;
static constexpr uint8_t DEFAULT_MANUAL_DURATION_MINUTES = 5;
static constexpr int16_t DEFAULT_WATERING_PERCENTAGE = 100;

static constexpr uint8_t SCHEDULE_MAX_RECORDS = 32;
static constexpr int8_t TIMEZONE_OFFSET_MIN_HOURS = -13;
static constexpr int8_t TIMEZONE_OFFSET_MAX_HOURS = 13;
static constexpr uint16_t DEFAULT_MQTT_PORT = 1883;

static constexpr size_t CONFIG_JSON_MAX_BYTES = 768;
static constexpr size_t SETTINGS_JSON_MAX_BYTES = 768;
static constexpr size_t SCHEDULE_JSON_MAX_BYTES = 4096;

static constexpr const uint8_t DISPLAY_1ST_LINE_OFFSET = 7;
static constexpr const uint8_t DISPLAY_2ND_LINE_OFFSET = 28;
static constexpr const uint8_t DISPLAY_3RD_LINE_OFFSET = 50;
static constexpr const uint8_t DISPLAY_4TH_LINE_OFFSET = 63;

static constexpr const char* CONFIG_FILE_PATH = "/config.json";
static constexpr const char* SETTINGS_FILE_PATH = "/settings.json";
static constexpr const char* SCHEDULE_FILE_PATH = "/schedule.json";

}  // namespace irrigator::board