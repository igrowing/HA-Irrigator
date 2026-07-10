#include "Actuators.h"

// --- Actuator State Variables ---
volatile bool heaterState = false;
volatile bool coolerState = false;
bool valvePotsOpen = false;
bool valveHeadsOpen = false;
bool valveTailsOpen = false;
bool valveWashOpen = false;

/**
 * @brief Initialize all actuator pins
 * Note: Relays are ACTIVE LOW (LOW=ON, HIGH=OFF)
 *       Buzzer is ACTIVE HIGH (HIGH=ON, LOW=OFF)
 */
void initActuators() {
    // Configure heater and cooler as outputs (relays - active low)
    pinMode(HEATER_PIN, OUTPUT);
    pinMode(COOLER_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    
    // Configure valve outputs (relays - active low)
    pinMode(VALVE_POTALE_PIN, OUTPUT);
    pinMode(VALVE_HEADS_PIN, OUTPUT);
    pinMode(VALVE_TAILS_PIN, OUTPUT);
    pinMode(VALVE_WASH_PIN, OUTPUT);
    
    // Initialize all relays to OFF state (HIGH for active-low relays)
    digitalWrite(HEATER_PIN, HIGH);
    digitalWrite(COOLER_PIN, HIGH);
    digitalWrite(VALVE_POTALE_PIN, HIGH);
    digitalWrite(VALVE_HEADS_PIN, HIGH);
    digitalWrite(VALVE_TAILS_PIN, HIGH);
    digitalWrite(VALVE_WASH_PIN, HIGH);
    
    // Initialize buzzer to OFF state (LOW for active-high buzzer)
    digitalWrite(BUZZER_PIN, LOW);
    
    Serial.println("Actuators initialized");
}

/**
 * @brief Update actuator state tracking by reading GPIO outputs
 * Note: Relays are ACTIVE LOW (LOW=ON, HIGH=OFF)
 *       Buzzer is ACTIVE HIGH (HIGH=ON, LOW=OFF)
 */
void updateActuatorStates() {
    // Relays: LOW = ON, HIGH = OFF
    lastSensorData.heater = (digitalRead(HEATER_PIN) == LOW);  // TODO: maybe good enough to avoid reading of GPIO and compare to variable.
    lastSensorData.cooler = (digitalRead(COOLER_PIN) == LOW);
    lastSensorData.valvePots = (digitalRead(VALVE_POTALE_PIN) == LOW);
    lastSensorData.valveHeads = (digitalRead(VALVE_HEADS_PIN) == LOW);
    lastSensorData.valveTails = (digitalRead(VALVE_TAILS_PIN) == LOW);
    lastSensorData.valveWash = (digitalRead(VALVE_WASH_PIN) == LOW);
}

/**
 * @brief Set heater state
 * @param state true to enable, false to disable
 * Note: Relay is ACTIVE LOW (LOW=ON, HIGH=OFF)
 */
void setHeater(bool state) {
    digitalWrite(HEATER_PIN, state ? LOW : HIGH);
    heaterState = state;
    tempTrend = (heaterState == LOW) ? TREND_HEATING : TREND_COOLING;
}

/**
 * @brief Set cooler state
 * @param state true to enable, false to disable
 * Note: Relay is ACTIVE LOW (LOW=ON, HIGH=OFF)
 */
void setCooler(bool state) {
    digitalWrite(COOLER_PIN, state ? LOW : HIGH);
    coolerState = state;
}

/**
 * @brief Control buzzer
 * TODO: give argument which sound to play (OK, error, alarm)
 * @param state true to sound, false to silence
 */
void setBuzzer(bool state) {
    digitalWrite(BUZZER_PIN, state ? HIGH : LOW);
}

/**
 * @brief Set potale valve (pots valve)
 * @param state true to open, false to close
 * Note: Relay is ACTIVE LOW (LOW=OPEN, HIGH=CLOSED)
 */
void setValvePotAle(bool state) {
    digitalWrite(VALVE_POTALE_PIN, state ? LOW : HIGH);
    valvePotsOpen = state;
}

/**
 * @brief Set heads valve
 * @param state true to open, false to close
 * Note: Relay is ACTIVE LOW (LOW=OPEN, HIGH=CLOSED)
 */
void setValveHeads(bool state) {
    digitalWrite(VALVE_HEADS_PIN, state ? LOW : HIGH);
    valveHeadsOpen = state;
}

/**
 * @brief Set tails valve
 * @param state true to open, false to close
 * Note: Relay is ACTIVE LOW (LOW=OPEN, HIGH=CLOSED)
 */
void setValveTails(bool state) {
    digitalWrite(VALVE_TAILS_PIN, state ? LOW : HIGH);
    valveTailsOpen = state;
}

/**
 * @brief Set wash valve
 * @param state true to open, false to close
 * Note: Relay is ACTIVE LOW (LOW=OPEN, HIGH=CLOSED)
 */
void setValveWash(bool state) {
    digitalWrite(VALVE_WASH_PIN, state ? LOW : HIGH);
    valveWashOpen = state;
}

/**
 * @brief Play entrance bell ding-dong tone using PWM
 * Generates two frequencies using ESP32's LED PWM controller
 * Pattern: 440 Hz for 300ms, then 330 Hz for 500ms
 */
void playBuzzerDingDong() {
    const uint8_t PWM_CHANNEL = 0;
    const uint8_t PWM_RESOLUTION = 8;  // 8-bit resolution (0-255)
    const uint8_t PWM_DUTY = 128;  // 50% duty cycle
    
    // First tone: 440 Hz (ding)
    ledcSetup(PWM_CHANNEL, 440, PWM_RESOLUTION);
    ledcAttachPin(BUZZER_PIN, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(300);
    
    // Second tone: 330 Hz (dong)
    ledcSetup(PWM_CHANNEL, 330, PWM_RESOLUTION);  // Change frequency
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(500);
    
    // Stop buzzer
    ledcWrite(PWM_CHANNEL, 0);
    ledcDetachPin(BUZZER_PIN);
}

/**
 * @brief Play distillation START melody (ascending pleasant chime)
 * Notes: C4 (262 Hz) - E4 (330 Hz) - G4 (392 Hz)
 * Duration: 200ms per note
 * Creates a positive, startup-like sound
 */
void playBuzzerStartMelody() {
    const uint8_t PWM_CHANNEL = 0;
    const uint8_t PWM_RESOLUTION = 8;
    const uint8_t PWM_DUTY = 128;  // 50% duty cycle
    const uint16_t NOTE_DURATION = 200;  // milliseconds per note
    
    ledcAttachPin(BUZZER_PIN, PWM_CHANNEL);
    
    // C4: 262 Hz
    ledcSetup(PWM_CHANNEL, 262, PWM_RESOLUTION);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(NOTE_DURATION);
    
    // E4: 330 Hz
    ledcSetup(PWM_CHANNEL, 330, PWM_RESOLUTION);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(NOTE_DURATION);
    
    // G4: 392 Hz
    ledcSetup(PWM_CHANNEL, 392, PWM_RESOLUTION);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(NOTE_DURATION);
    
    // Stop buzzer
    ledcWrite(PWM_CHANNEL, 0);
    ledcDetachPin(BUZZER_PIN);
}

/**
 * @brief Play distillation END melody (descending completion chime - reverse of start)
 * Notes: G4 (392 Hz) - E4 (330 Hz) - C4 (262 Hz)
 * Duration: 200ms per note
 * Creates a graceful shutdown/completion sound
 */
void playBuzzerEndMelody() {
    const uint8_t PWM_CHANNEL = 0;
    const uint8_t PWM_RESOLUTION = 8;
    const uint8_t PWM_DUTY = 128;  // 50% duty cycle
    const uint16_t NOTE_DURATION = 200;  // milliseconds per note
    
    ledcAttachPin(BUZZER_PIN, PWM_CHANNEL);
    
    // G4: 392 Hz
    ledcSetup(PWM_CHANNEL, 392, PWM_RESOLUTION);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(NOTE_DURATION);
    
    // E4: 330 Hz
    ledcSetup(PWM_CHANNEL, 330, PWM_RESOLUTION);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(NOTE_DURATION);
    
    // C4: 262 Hz
    ledcSetup(PWM_CHANNEL, 262, PWM_RESOLUTION);
    ledcWrite(PWM_CHANNEL, PWM_DUTY);
    delay(NOTE_DURATION);
    
    // Stop buzzer
    ledcWrite(PWM_CHANNEL, 0);
    ledcDetachPin(BUZZER_PIN);
}

/**
 * @brief Play ERROR/ALERT melody (alarming high-low warning beep)
 * Pattern: Alternating 1000 Hz (high) - 500 Hz (low) 
 * Duration: 100ms pulses for urgent feel
 * Creates a screaming, unpleasant alert sound
 * NOTE: Not currently called - available for error conditions
 */
void playBuzzerErrorMelody() {
    const uint8_t PWM_CHANNEL = 0;
    const uint8_t PWM_RESOLUTION = 8;
    const uint8_t PWM_DUTY = 128;  // 50% duty cycle
    const uint16_t PULSE_DURATION = 100;  // milliseconds per pulse (urgent feel)
    const uint8_t PULSE_COUNT = 4;  // 4 pulses = 2 high + 2 low cycles
    
    ledcAttachPin(BUZZER_PIN, PWM_CHANNEL);
    
    for (uint8_t i = 0; i < PULSE_COUNT; i++) {
        if (i % 2 == 0) {
            // High frequency: 1000 Hz (alarming high pitch)
            ledcSetup(PWM_CHANNEL, 1000, PWM_RESOLUTION);
        } else {
            // Low frequency: 500 Hz (warning tone)
            ledcSetup(PWM_CHANNEL, 500, PWM_RESOLUTION);
        }
        ledcWrite(PWM_CHANNEL, PWM_DUTY);
        delay(PULSE_DURATION);
    }
    
    // Stop buzzer
    ledcWrite(PWM_CHANNEL, 0);
    ledcDetachPin(BUZZER_PIN);
}

