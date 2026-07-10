#include "InputManager.h"
#include "MenuSystem.h"  // For selectedDistilMode, targetTemp, and temperature constants

// --- Input Objects ---
RotaryEncoder rotaryEncoder(ROTARY_CLK_PIN, ROTARY_DAT_PIN, RotaryEncoder::LatchMode::TWO03);
Button2 rotaryButton(ROTARY_SWITCH_PIN);

// --- Button Event State ---
volatile RotaryEvent lastButtonEvent = ROT_NONE;
volatile int lastEncoderPos = 0;

// External variable needed by menu navigation
extern bool screenNeedsUpdate;

IRAM_ATTR void checkPosition()
{
  rotaryEncoder.tick(); // just call tick() to check the state.
}

// --- Button2 Event Handlers ---
void handleSingleClick(Button2& btn) {
    lastButtonEvent = ROT_CLICK;
}

void handleDoubleClick(Button2& btn) {
    lastButtonEvent = ROT_DOUBLE_CLICK;
}

void handleLongClick(Button2& btn) {
    lastButtonEvent = ROT_LONG_CLICK;
}

void initializeInput() {
    // Initialize Rotary Encoder
    rotaryEncoder.setPosition(0);
    Serial.println("Rotary encoder initialized");
    
    // Initialize Button2
    rotaryButton.setClickHandler(handleSingleClick);
    rotaryButton.setDoubleClickHandler(handleDoubleClick);
    rotaryButton.setLongClickHandler(handleLongClick);
    rotaryButton.setLongClickTime(800);
    Serial.println("Button handler initialized");
}

RotaryEvent getRotaryEvent() {
    // Update encoder position from the library
    rotaryEncoder.tick();
    
    // Check for button event (priority over encoder)
    if (lastButtonEvent != ROT_NONE) {
        RotaryEvent event = lastButtonEvent;
        lastButtonEvent = ROT_NONE;
        return event;
    }
    
    // Check for encoder rotation
    int newPos = rotaryEncoder.getPosition();
    int change = newPos - lastEncoderPos;
    
    // Only update if there's actual movement
    if (change != 0) {
        lastEncoderPos = newPos;
        
        if (change > 0) {
            return ROT_RIGHT;
        } else if (change < 0) {
            return ROT_LEFT;
        }
    }
    
    return ROT_NONE;
}

// Update selected item in menu or invoke its callback on click
void handleMenuNavigation(RotaryEvent event, Menu& menu) {
    switch (event) {
        case ROT_LEFT:
            // In manual mode, LEFT decreases target temperature. However in manual menu scroll over menu items.
            if (selectedDistilMode == DISTIL_MANUAL && !menuVisible) {
                if (targetTemp <= TARGET_TEMP_MIN_C) {
                    targetTemp = TARGET_TEMP_DISABLED;  // Jump to disabled (0°C)
                } else if (targetTemp > TARGET_TEMP_MIN_C) {
                    targetTemp -= 1.0;  // Decrease by 1 degree
                }
            } else if (menu.selectedIndex > 0) {
                menu.selectedIndex--;
            } else {
                menu.selectedIndex = menu.itemCount - 1;
            }
            break;
            
        case ROT_RIGHT:
            // In manual mode, RIGHT increases target temperature. However in manual menu scroll over menu items.
            if (selectedDistilMode == DISTIL_MANUAL && !menuVisible) {
                if (targetTemp == TARGET_TEMP_DISABLED) {
                    targetTemp = TARGET_TEMP_MIN_C;  // Jump from disabled to min
                } else if (targetTemp < TARGET_TEMP_MAX_C) {
                    targetTemp += 1.0;  // Increase by 1 degree
                }
            } else if (menu.selectedIndex < menu.itemCount - 1) {
                menu.selectedIndex++;
            } else {
                menu.selectedIndex = 0;
            }
            break;
            
        case ROT_CLICK:
            if (menu.items[menu.selectedIndex].callback) {
                menu.items[menu.selectedIndex].callback();
            }
            break;
            
        case ROT_NONE:
        default:
            break;
    }
}
