#include "Sensors.h"

// --- Sensor State Variables (Inputs Only) ---
// bool levelLow = false;
// bool levelHigh = false;
volatile uint32_t meterDistilCount = 0;
volatile uint32_t meterWashCount = 0;
TempTrend tempTrend = TREND_STABLE;

// --- Distillate Volume Tracking (categorized by outlet) ---
// Track which outlet was active during last pulse count check
static uint32_t lastMeterDistilCount = 0;
static bool lastValveHeadsState = false;
static bool lastValveTailsState = false;

// Cached sensor data for display and reporting (fully initialized)
SensorData lastSensorData = {
    .temperature = -100.0,
    .levelLow = false,
    .levelHigh = false,
    .headsVolume = 0.0,
    .bodyVolume = 0.0,
    .tailsVolume = 0.0,
    .washVolume = 0.0,
    .valvePots = false,
    .valveHeads = false,
    .valveTails = false,
    .valveWash = false,
    .heater = false,
    .cooler = false
};

// --- Flow Meter ISRs ---
void IRAM_ATTR onMeterDistilPulse() {
    meterDistilCount++;
}

void IRAM_ATTR onMeterWashPulse() {
    meterWashCount++;
}

void initSensors() {
    // Configure level sensors as inputs
    pinMode(LEVEL_LOW_PIN, INPUT);
    pinMode(LEVEL_HIGH_PIN, INPUT);
    
    // Configure flow meter inputs with ISRs for pulse counting
    pinMode(METER_DISTIL_PIN, INPUT);
    pinMode(METER_WASH_PIN, INPUT);
    
    // Attach ISRs to flow meters (trigger on rising edge)
    attachInterrupt(digitalPinToInterrupt(METER_DISTIL_PIN), onMeterDistilPulse, RISING);
    attachInterrupt(digitalPinToInterrupt(METER_WASH_PIN), onMeterWashPulse, RISING);
    
    Serial.println("Sensors initialized");
}

void readLevelSensors() {
    lastSensorData.levelLow = digitalRead(LEVEL_LOW_PIN);
    lastSensorData.levelHigh = digitalRead(LEVEL_HIGH_PIN);
}

float getFlowVolume(uint32_t pulseCount) {
    return pulseCount / FLOW_METER_PULSES_PER_ML;
}

void resetFlowMeters() {
    meterDistilCount = 0;
    meterWashCount = 0;
    // Reset volume accumulators when distillation ends
    lastSensorData.headsVolume = 0.0;
    lastSensorData.bodyVolume = 0.0;
    lastSensorData.tailsVolume = 0.0;
    lastMeterDistilCount = 0;
}

/**
 * @brief Categorize distillate volume by outlet based on valve states
 * Logic:
 *   - valveHeadsOpen OR both closed → Heads outlet (including body/heads distinction)
 *   - valveHeadsOpen closed AND valveTailsOpen open → Tails outlet
 *   - Both valves closed → Body outlet
 */
void updateDistillateOutlets() {
    // Calculate new flow since last check
    uint32_t newPulses = meterDistilCount - lastMeterDistilCount;
    float newVolume = getFlowVolume(newPulses);
    
    if (newVolume > 0.0) {
        // Determine which outlet was active based on current valve states
        if (valveHeadsOpen) {
            // Heads valve is open - all flow goes to heads
            lastSensorData.headsVolume += newVolume;
        } else if (valveTailsOpen) {
            // Tails valve is open and heads is closed - all flow goes to tails
            lastSensorData.tailsVolume += newVolume;
        } else {
            // Both valves closed - all flow goes to body
            lastSensorData.bodyVolume += newVolume;
        }
        
        lastMeterDistilCount = meterDistilCount;
    }
}

// --- NTC Temperature Calculation ---
// Function to read NTC and convert resistance to temperature using look-up/interpolation
float readNTC() {
    // 1. Read the Analog Value
    int adcValue = analogRead(NTC_PIN);
    
    // Safety check for common ADC issues
    if (adcValue <= 0 || adcValue >= (int)ADC_MAX_VALUE) {
        Serial.println("ADC read out of range (0 or 4095). Check wiring.");
        return lastSensorData.temperature;  // Return last known good value
    }

    // 2. Calculate Resistance of NTC (R_thermistor)
    // R_thermistor = R_pullup * ( (ADC_max / ADC_value) - 1.0 )
    float ntcResistance = PULLUP_RESISTANCE * ( (ADC_MAX_VALUE / (float)adcValue) - 1.0 );
    Serial.printf("ADC: %d, NTC Resistance: %.1f ohms. ", adcValue, ntcResistance);

    // 3. Select the correct resistance array based on the current trend
    const float* resistance_array;
    if (tempTrend == TREND_COOLING) {
        resistance_array = NTC_RESISTANCE_COOLING;
        Serial.print("Using COOLING trend data. ");
    } else {
        // Default to Heating trend if STABLE or TREND_HEATING
        resistance_array = NTC_RESISTANCE_HEATING; 
        Serial.print("Using HEATING/default trend data. ");
    }

    // 4. Look up, Interpolate, and Extrapolate Temperature
    float tempC = -999.0;
    
    // Extrapolation Low (Resistance is high, Temp is below NTC_TEMP[0] e.g., < 20C)
    if (ntcResistance > resistance_array[0]) {
        Serial.print("Extrapolating low (below 20C). ");
        // Use first two points for extrapolation: (R[1], T[1]) and (R[0], T[0])
        tempC = interpolate(ntcResistance, 
                            resistance_array[1], NTC_TEMP[1], 
                            resistance_array[0], NTC_TEMP[0]);
    } 
    // Extrapolation High (Resistance is low, Temp is above NTC_TEMP[NUM_POINTS-1] e.g., > 100C)
    else if (ntcResistance < resistance_array[NUM_POINTS - 1]) {
        Serial.print("Extrapolating high (above 100C). ");
        // Use last two points for extrapolation
        int last = NUM_POINTS - 1;
        int second_to_last = NUM_POINTS - 2;
        tempC = interpolate(ntcResistance, 
                            resistance_array[last], NTC_TEMP[last], 
                            resistance_array[second_to_last], NTC_TEMP[second_to_last]);
    }
    // Interpolate within range
    else {
        // Find segment [i, i+1] where R[i+1] <= R_ntc <= R[i]
        // R decreases as T increases, so R[i] is R_high, R[i+1] is R_low
        for (int i = 0; i < NUM_POINTS - 1; ++i) {
            if (ntcResistance <= resistance_array[i] && ntcResistance >= resistance_array[i + 1]) {
                tempC = interpolate(ntcResistance, 
                                    resistance_array[i + 1], NTC_TEMP[i + 1], // X0 (R_low), Y0 (T_high)
                                    resistance_array[i], NTC_TEMP[i]          // X1 (R_high), Y1 (T_low)
                                   );
                break;
            }
        }
    }
    
    if (tempC > -900.0) { // Check if a valid temp was calculated
        Serial.printf("Calculated Temp: %.2f C\n", tempC);
        lastSensorData.temperature = tempC;
        return tempC;
    }

    Serial.println("NTC Reading failed to fit lookup table range. Returning last known.");
    return lastSensorData.temperature;  // Fallback to last valid reading
}

void readAllSensors() {
    // Do not read temperature. It is read periodically by ticker.
    readLevelSensors();
    updateActuatorStates();  // Update actuator states (valves, heater, cooler)
    updateDistillateOutlets();  // Categorize distillate flow by outlet (heads/body/tails)
    lastSensorData.washVolume = getFlowVolume(meterWashCount);
}
