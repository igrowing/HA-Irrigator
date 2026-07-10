#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <Arduino.h>
#include <RotaryEncoder.h>
#include <Button2.h>
#include "Menu.h"

// --- Input Event Types ---
enum RotaryEvent {
  ROT_NONE,
  ROT_LEFT,
  ROT_RIGHT,
  ROT_CLICK,
  ROT_DOUBLE_CLICK,
  ROT_LONG_CLICK
};

// --- Hardware Pin Definitions ---
#define ROTARY_SWITCH_PIN   0
#define ROTARY_CLK_PIN      16
#define ROTARY_DAT_PIN      17

// --- External Input Objects ---
extern RotaryEncoder rotaryEncoder;
extern Button2 rotaryButton;

// --- Input Functions ---
void initializeInput();
RotaryEvent getRotaryEvent();
void handleMenuNavigation(RotaryEvent event, Menu& menu);

#endif // INPUT_MANAGER_H
