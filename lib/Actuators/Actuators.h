#ifndef ACTUATORS_H
#define ACTUATORS_H

#include <Arduino.h>
#include "Sensors.h"

// --- Actuator Pin Definitions ---
#define BUZZER_PIN          2     // Buzzer control
#define COOLER_PIN          5     // Cooler relay control
#define HEATER_PIN          14    // Heater relay control
#define VALVE_POTALE_PIN    18    // Potale valve
#define VALVE_HEADS_PIN     19    // Heads valve
#define VALVE_TAILS_PIN     23    // Tails valve
#define VALVE_WASH_PIN      26    // Wash valve

// --- Actuator State Variables ---
extern volatile bool heaterState;
extern volatile bool coolerState;
extern bool valvePotsOpen;
extern bool valveHeadsOpen;
extern bool valveTailsOpen;
extern bool valveWashOpen;

// --- Actuator Control Functions ---
void initActuators();
void updateActuatorStates();
void setHeater(bool state);
void setCooler(bool state);
void setBuzzer(bool state);
void setValvePotAle(bool state);
void setValveHeads(bool state);
void setValveTails(bool state);
void setValveWash(bool state);
void playBuzzerDingDong();  // Play entrance bell ding-dong tonevoid playBuzzerStartMelody();  // Play ascending chime when distillation starts
void playBuzzerStartMelody();
void playBuzzerEndMelody();  // Play descending chime when distillation ends
void playBuzzerErrorMelody();  // Play alarming error/alert sound (not currently used)
#endif // ACTUATORS_H
