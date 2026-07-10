#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include "Helpers.h"
#include "Actuators.h"  // For valve and actuator state tracking

// --- Sensor Pin Definitions (Input Sensors Only) ---
#define LEVEL_LOW_PIN       25     // Level low sensor (GPIO 25)
#define LEVEL_HIGH_PIN      32     // Level high sensor (GPIO 32)
#define METER_DISTIL_PIN    27     // Distiller flow meter (GPIO 27)
#define METER_WASH_PIN      36     // Wash flow meter (GPIO 36)
#define NTC_PIN             39     // NTC temperature sensor (GPIO 39)

// Flow meter calibration
#define FLOW_METER_PULSES_PER_ML 1.0  // Pulses per mL (calibration constant)
#define ADC_MAX_VALUE 4095.0 // 12-bit ADC resolution
#define PULLUP_RESISTANCE 8800.0 // R1 in the voltage divider (10k ohms)

// --- NTC Lookup Table and Hysteresis ---

/**
 * @brief Sensor data structure for collecting all readings
 */
struct SensorData {
    float temperature;
    bool levelLow;
    bool levelHigh;
    float headsVolume;      // Volume collected from Heads outlet (mL)
    float bodyVolume;       // Volume collected from Body outlet (mL)
    float tailsVolume;      // Volume collected from Tails outlet (mL)
    float washVolume;
    bool valvePots;
    bool valveHeads;
    bool valveTails;
    bool valveWash;
    bool heater;
    bool cooler;
};

// Defines the temperature trend for selecting the correct resistance column
enum TempTrend {
    TREND_HEATING,
    TREND_COOLING,
    TREND_STABLE // Use a fallback or the last known good value
};
extern TempTrend tempTrend;

// Resistance values in Ohms (corresponding to NTC_TEMP)
const float NTC_RESISTANCE_HEATING[] = {
    12250.0, // 20C
    9200.0,  // 30C
    5800.0,  // 40C
    3540.0,  // 50C
    2600.0,  // 60C
    1840.0,  // 70C
    1330.0,  // 80C
    860.0,   // 90C
    740.0    // 100C
};

const float NTC_RESISTANCE_COOLING[] = {
    12000.0, // 20C
    7820.0,  // 30C 
    5100.0,  // 40C
    3000.0,  // 50C 
    2210.0,  // 60C 
    1450.0,  // 70C
    1100.0,  // 80C
    780.0,   // 90C
    710.0    // 100C
};

// Temperature points (fixed)
const float NTC_TEMP[] = {20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0};
const int NUM_POINTS = sizeof(NTC_TEMP) / sizeof(NTC_TEMP[0]);

// Global state for the current trend (needs external logic to be updated)
extern SensorData lastSensorData;  // Consolidated temperature source

// --- Sensor State Variables (Inputs Only) ---
// extern bool levelLow;
// extern bool levelHigh;
extern volatile uint32_t meterDistilCount;
extern volatile uint32_t meterWashCount;

// --- Sensor Functions ---
void initSensors();
void readLevelSensors();
float readNTC();
float getFlowVolume(uint32_t pulseCount);
void resetFlowMeters();
void updateDistillateOutlets();  // Track which outlet is receiving flow and categorize volume
void readAllSensors();

#endif // SENSORS_H
