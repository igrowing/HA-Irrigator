#ifndef MENU_SYSTEM_H
#define MENU_SYSTEM_H

#include "Menu.h"
#include <Ticker.h>
#include "StateManager.h"
#include <WiFi.h>
#include "Sensors.h"
#include "Actuators.h"

// --- Distillation Mode Enum ---
enum DistilMode {
    DISTIL_WATER,
    DISTIL_FIRST_ETHANOL,
    DISTIL_SECOND_ETHANOL,
    DISTIL_MANUAL
};

static const char *DISTIL_MODE_STRING[] = {
    "Water",
    "1st Ethanol",
    "2nd Ethanol",
    "Manual"
};

// --- UI Screen Enum ---
enum UIScreen {
    UI_INITIAL_MENU,
    UI_RUNNING_STATE,
    UI_PAUSED_STATE,
    UI_ENDED_STATE,
    UI_STATUS_MENU,
    UI_DIAGNOSTICS_MENU
};

// --- Manual Distillation Mode Constants ---
#define TARGET_TEMP_MIN_C 70.0   // Minimum target temperature (°C)
#define TARGET_TEMP_MAX_C 105.0  // Maximum target temperature (°C)
#define TARGET_TEMP_DISABLED 0.0 // Special value: 0 = heater disabled

// --- Menu Variables ---
extern DistilMode selectedDistilMode;
extern float targetTemp;  // Target temperature for manual mode
extern UIScreen currentScreen;
extern UIScreen previousScreen;  // Track previous screen for navigation back from status menu
extern bool menuVisible;
extern bool screenNeedsUpdate;
extern Menu initialMenu;
extern Menu runningMenu;
extern Menu pausedMenu;
extern Menu statusMenu;  // Dynamically updated by dynamicStatusMenu()
extern Menu diagnosticsMenu;  // Dynamically updated by dynamicDiagnosticsMenu()

// --- Menu Callback Functions ---
// Initial menu
void menuCallback_Water();
void menuCallback_FirstEthanol();
void menuCallback_SecondEthanol();
void menuCallback_Manual();
void menuCallback_Status();
void menuCallback_Diagnostics();

// Running menu
void menuCallback_Pause();
void menuCallback_Stop();
void menuCallback_Back();

// Paused menu
void menuCallback_Resume();

// Diagnostics menu
void menuCallback_ExitDiagnostics();
void menuCallback_ToggleHeater();
void menuCallback_ToggleCooler();
void menuCallback_ToggleHeadsValve();
void menuCallback_ToggleTailsValve();
void menuCallback_TogglePotsValve();
void menuCallback_ToggleWashValve();
void menuCallback_TestBuzzer();

// --- Dynamic Status Menu ---
void dynamicStatusMenu();

// --- Dynamic Diagnostics Menu ---
void dynamicDiagnosticsMenu();

// // --- Menu Initialization ---
// void initializeMenus();

#endif // MENU_SYSTEM_H
