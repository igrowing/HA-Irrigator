#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

namespace irrigator::board {

static constexpr const char* FIRMWARE_VERSION = APP_VERSION;

// I2C bus shared by PCF8574 relay expander, DS3231 RTC, and SSD1309 OLED.
static constexpr uint8_t I2C_SDA_PIN = 21;
static constexpr uint8_t I2C_SCL_PIN = 22;
static constexpr uint32_t I2C_CLOCK_HZ = 400000UL;

static constexpr uint8_t PCF8574_ADDRESS = 0x20;
static constexpr uint8_t DS3231_ADDRESS = 0x68;
static constexpr uint8_t SSD1309_ADDRESS = 0x3C;

static constexpr uint8_t ZONE_COUNT = 11;
static constexpr uint8_t PCF8574_ZONE_COUNT = 8;
static constexpr uint8_t GPIO_ZONE_9_PIN = 25;
static constexpr uint8_t GPIO_ZONE_10_PIN = 26;
static constexpr uint8_t GPIO_ZONE_11_PIN = 27;
static constexpr uint8_t PUMP_RELAY_PIN = 16;

static constexpr uint8_t STATUS_LED_PIN = 14;

static constexpr uint8_t ROTARY_CLK_PIN = 34;
static constexpr uint8_t ROTARY_DAT_PIN = 35;
static constexpr uint8_t ROTARY_BUTTON_PIN = 39;
static constexpr uint16_t ROTARY_LONG_CLICK_MS = 1000;

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
static constexpr uint8_t WATERING_QUEUE_DEPTH = 10;

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

static constexpr const char* CONFIG_FILE_PATH = "/config.json";
static constexpr const char* SETTINGS_FILE_PATH = "/settings.json";
static constexpr const char* SCHEDULE_FILE_PATH = "/schedule.json";

}  // namespace irrigator::board