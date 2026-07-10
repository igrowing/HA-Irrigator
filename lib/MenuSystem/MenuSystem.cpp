#include "MenuSystem.h"

// --- Menu Variables ---
DistilMode selectedDistilMode = DISTIL_WATER;
float targetTemp = TARGET_TEMP_DISABLED;  // Target temperature for manual mode (0 = disabled, 70-105°C = active)
UIScreen currentScreen = UI_INITIAL_MENU;
UIScreen previousScreen = UI_INITIAL_MENU;  // For returning from status menu
bool menuVisible = false;
bool screenNeedsUpdate = true;

// Status menu items (dynamically populated with values)
static MenuItem statusMenuItems[16];
Menu statusMenu("STATUS", statusMenuItems, 15);

// Diagnostics menu items (dynamically populated with relay states)
static MenuItem diagnosticsMenuItems[8];
Menu diagnosticsMenu("DIAGNOSTICS", diagnosticsMenuItems, 8);

// --- Initial State Menu Callbacks ---
void menuCallback_Water() {
    selectedDistilMode = DISTIL_WATER;
    runStartTime = millis();
    totalPausedTime = 0;
    pausedAtTime = 0;
    changeStateAndReport(STATE_RUNNING);
    playBuzzerStartMelody();  // Play start melody
    currentScreen = UI_RUNNING_STATE;
    menuVisible = false;
}

void menuCallback_FirstEthanol() {
    selectedDistilMode = DISTIL_FIRST_ETHANOL;
    runStartTime = millis();
    totalPausedTime = 0;
    pausedAtTime = 0;
    changeStateAndReport(STATE_RUNNING);
    playBuzzerStartMelody();  // Play start melody
    currentScreen = UI_RUNNING_STATE;
    menuVisible = false;
}

void menuCallback_SecondEthanol() {
    selectedDistilMode = DISTIL_SECOND_ETHANOL;
    runStartTime = millis();
    totalPausedTime = 0;
    pausedAtTime = 0;
    changeStateAndReport(STATE_RUNNING);
    playBuzzerStartMelody();  // Play start melody
    currentScreen = UI_RUNNING_STATE;
    menuVisible = false;
}

void menuCallback_Manual() {
    selectedDistilMode = DISTIL_MANUAL;
    runStartTime = millis();
    totalPausedTime = 0;
    pausedAtTime = 0;
    changeStateAndReport(STATE_RUNNING);
    playBuzzerStartMelody();  // Play start melody
    currentScreen = UI_RUNNING_STATE;
    menuVisible = false;
}

void menuCallback_Status() {
    Serial.println("Opened: Status menu");
    previousScreen = currentScreen;  // Save current screen to return to it later
    currentScreen = UI_STATUS_MENU;
    menuVisible = true;
}

void menuCallback_Diagnostics() {
    Serial.println("Opened: Diagnostics menu");
    currentScreen = UI_DIAGNOSTICS_MENU;
    menuVisible = true;
}

// --- Initial State Menu Definition ---
const MenuItem initialMenuItems[] = {
    { "Water", menuCallback_Water },
    { "1st ethanol", menuCallback_FirstEthanol },
    { "2nd ethanol", menuCallback_SecondEthanol },
    { "Manual", menuCallback_Manual },
    { "Status", menuCallback_Status },
    { "Diagnostics", menuCallback_Diagnostics }
};

const uint8_t INITIAL_MENU_ITEM_COUNT = sizeof(initialMenuItems) / sizeof(initialMenuItems[0]);
Menu initialMenu("DISTILLER", initialMenuItems, INITIAL_MENU_ITEM_COUNT);

// --- Running State Menu Callbacks ---
void menuCallback_Pause() {
    changeStateAndReport(STATE_PAUSED);
    currentScreen = UI_PAUSED_STATE;
    menuVisible = false;
    screenNeedsUpdate = true;
    pausedAtTime = millis();
}

void menuCallback_Stop() {
    changeStateAndReport(STATE_ENDED);
    playBuzzerEndMelody();  // Play end/completion melody
    currentScreen = UI_ENDED_STATE;
    menuVisible = false;
    screenNeedsUpdate = true;
}

void menuCallback_Back() {
    if (currentScreen == UI_DIAGNOSTICS_MENU) currentScreen = UI_INITIAL_MENU;
    menuVisible = false;
    screenNeedsUpdate = true;
}

// --- Running State Menu Definition ---
const MenuItem runningMenuItems[] = {
    { "Pause", menuCallback_Pause },
    { "Stop", menuCallback_Stop },
    { "Status", menuCallback_Status },
    { "Back", menuCallback_Back }
};

const uint8_t RUNNING_MENU_ITEM_COUNT = sizeof(runningMenuItems) / sizeof(runningMenuItems[0]);
Menu runningMenu("RUNNING", runningMenuItems, RUNNING_MENU_ITEM_COUNT);

// --- Paused State Menu Callbacks ---
void menuCallback_Resume() {
    if (pausedAtTime > 0) {
        totalPausedTime += (millis() - pausedAtTime);
        pausedAtTime = 0;
    }
    changeStateAndReport(STATE_RUNNING);
    playBuzzerStartMelody();  // Play resume melody (same as start)
    currentScreen = UI_RUNNING_STATE;
    menuVisible = false;
    screenNeedsUpdate = true;
}

// --- Paused State Menu Definition ---
const MenuItem pausedMenuItems[] = {
    { "Resume", menuCallback_Resume },
    { "Stop", menuCallback_Stop },
    { "Status", menuCallback_Status },
    { "Back", menuCallback_Back }
};

const uint8_t PAUSED_MENU_ITEM_COUNT = sizeof(pausedMenuItems) / sizeof(pausedMenuItems[0]);
Menu pausedMenu("PAUSED", pausedMenuItems, PAUSED_MENU_ITEM_COUNT);


// --- Status Menu ---
// Status menu items are generated dynamically based on current sensor data
// (no static statusMenuItems array since values change)

/**
 * @brief Generate dynamic status menu with current sensor values
 * Called right before displaying status screen to ensure fresh data
 */
void dynamicStatusMenu() {
    // Build status menu items dynamically with current sensor data
    static MenuItem statusMenuItems[16];
    static char verStr[50];
    static char tempStr[50];
    static char ipStr[50];
    static char levelHiStr[50];
    static char levelLoStr[50];
    static char washValveStr[50];
    static char potsValveStr[50];
    static char headsValveStr[50];
    static char tailsValveStr[50];
    static char heaterStr[50];
    static char coolerStr[50];
    static char washVolStr[50];
    static char headsVolStr[50];
    static char bodyVolStr[50];
    static char tailsVolStr[50];
    
    // Populate string buffers with current values
    snprintf(verStr, sizeof(verStr), "Version: %s", APP_VERSION);
    snprintf(tempStr, sizeof(tempStr), "Temp: %.1f C", lastSensorData.temperature);
    snprintf(ipStr, sizeof(ipStr), "IP: %s", WiFi.localIP().toString().c_str());
    snprintf(levelHiStr, sizeof(levelHiStr), "Hi level: %s", lastSensorData.levelHigh ? "HIGH" : "LOW");
    snprintf(levelLoStr, sizeof(levelLoStr), "Lo level: %s", lastSensorData.levelLow ? "HIGH" : "LOW");
    snprintf(washValveStr, sizeof(washValveStr), "Wash valve: %s", lastSensorData.valveWash ? "OPEN" : "CLOSED");
    snprintf(potsValveStr, sizeof(potsValveStr), "Dump valve: %s", lastSensorData.valvePots ? "OPEN" : "CLOSED");
    snprintf(headsValveStr, sizeof(headsValveStr), "Heads valve: %s", lastSensorData.valveHeads ? "OPEN" : "CLOSED");
    snprintf(tailsValveStr, sizeof(tailsValveStr), "Tails valve: %s", lastSensorData.valveTails ? "OPEN" : "CLOSED");
    snprintf(heaterStr, sizeof(heaterStr), "Heater: %s", lastSensorData.heater ? "ON" : "OFF");
    snprintf(coolerStr, sizeof(coolerStr), "Cooler: %s", lastSensorData.cooler ? "ON" : "OFF");
    snprintf(washVolStr, sizeof(washVolStr), "Wash: %.1f L", lastSensorData.washVolume);
    snprintf(headsVolStr, sizeof(headsVolStr), "Heads: %.1f mL", lastSensorData.headsVolume);
    snprintf(bodyVolStr, sizeof(bodyVolStr), "Body: %.1f mL", lastSensorData.bodyVolume);
    snprintf(tailsVolStr, sizeof(tailsVolStr), "Tails: %.1f mL", lastSensorData.tailsVolume);
    
    // Build menu items array with dynamically generated strings
    statusMenuItems[0] = {verStr, menuCallback_Back};
    statusMenuItems[1] = {ipStr, menuCallback_Back};
    statusMenuItems[2] = {tempStr, menuCallback_Back};
    statusMenuItems[3] = {levelHiStr, menuCallback_Back};
    statusMenuItems[4] = {levelLoStr, menuCallback_Back};
    statusMenuItems[5] = {washValveStr, menuCallback_Back};
    statusMenuItems[6] = {potsValveStr, menuCallback_Back};
    statusMenuItems[7] = {headsValveStr, menuCallback_Back};
    statusMenuItems[8] = {tailsValveStr, menuCallback_Back};
    statusMenuItems[9] = {heaterStr, menuCallback_Back};
    statusMenuItems[10] = {coolerStr, menuCallback_Back};
    statusMenuItems[11] = {washVolStr, menuCallback_Back};
    statusMenuItems[12] = {headsVolStr, menuCallback_Back};
    statusMenuItems[13] = {bodyVolStr, menuCallback_Back};
    statusMenuItems[14] = {tailsVolStr, menuCallback_Back};
    
    // Update global statusMenu with new items
    statusMenu.items = statusMenuItems;
    statusMenu.itemCount = 15;

}

// --- Diagnostics Menu Callbacks ---
// void menuCallback_ExitDiagnostics() {
//     currentScreen = UI_INITIAL_MENU;
//     menuVisible = false;
//     screenNeedsUpdate = true;
// }

void menuCallback_ToggleHeater() {
    setHeater(!heaterState);
    Serial.print("Heater toggled: ");
    Serial.println(heaterState ? "ON" : "OFF");
}

void menuCallback_ToggleCooler() {
    setCooler(!coolerState);
    Serial.print("Cooler toggled: ");
    Serial.println(coolerState ? "ON" : "OFF");
}

void menuCallback_ToggleHeadsValve() {
    setValveHeads(!valveHeadsOpen);
    Serial.print("Heads valve toggled: ");
    Serial.println(valveHeadsOpen ? "OPEN" : "CLOSED");
}

void menuCallback_ToggleTailsValve() {
    setValveTails(!valveTailsOpen);
    Serial.print("Tails valve toggled: ");
    Serial.println(valveTailsOpen ? "OPEN" : "CLOSED");
}

void menuCallback_TogglePotsValve() {
    setValvePotAle(!valvePotsOpen);
    Serial.print("Pots valve toggled: ");
    Serial.println(valvePotsOpen ? "OPEN" : "CLOSED");
}

void menuCallback_ToggleWashValve() {
    setValveWash(!valveWashOpen);
    Serial.print("Wash valve toggled: ");
    Serial.println(valveWashOpen ? "OPEN" : "CLOSED");
}

void menuCallback_TestBuzzer() {
    // Play ding-dong tone
    playBuzzerDingDong();
    Serial.println("Buzzer ding-dong played");
}

/**
 * @brief Generate dynamic diagnostics menu with relay toggle controls
 * Called right before displaying diagnostics screen to update relay states
 */
void dynamicDiagnosticsMenu() {
    static MenuItem diagnosticsMenuItems[8];
    static char heaterStr[50];
    static char coolerStr[50];
    static char headsStr[50];
    static char tailsStr[50];
    static char potsStr[50];
    static char washStr[50];
    
    // Populate string buffers with current relay states (ON/OFF suffix)
    snprintf(heaterStr, sizeof(heaterStr), "Heater: %s", heaterState ? "ON" : "OFF");
    snprintf(coolerStr, sizeof(coolerStr), "Cooler: %s", coolerState ? "ON" : "OFF");
    snprintf(headsStr, sizeof(headsStr), "Heads: %s", valveHeadsOpen ? "OPEN" : "CLOSED");
    snprintf(tailsStr, sizeof(tailsStr), "Tails: %s", valveTailsOpen ? "OPEN" : "CLOSED");
    snprintf(potsStr, sizeof(potsStr), "Pots: %s", valvePotsOpen ? "OPEN" : "CLOSED");
    snprintf(washStr, sizeof(washStr), "Wash: %s", valveWashOpen ? "OPEN" : "CLOSED");
    
    // Build menu items array with toggle callbacks
    diagnosticsMenuItems[0] = {heaterStr, menuCallback_ToggleHeater};
    diagnosticsMenuItems[1] = {coolerStr, menuCallback_ToggleCooler};
    diagnosticsMenuItems[2] = {headsStr, menuCallback_ToggleHeadsValve};
    diagnosticsMenuItems[3] = {tailsStr, menuCallback_ToggleTailsValve};
    diagnosticsMenuItems[4] = {potsStr, menuCallback_TogglePotsValve};
    diagnosticsMenuItems[5] = {washStr, menuCallback_ToggleWashValve};
    diagnosticsMenuItems[6] = {"Buzzer Test", menuCallback_TestBuzzer};
    diagnosticsMenuItems[7] = {"Back", menuCallback_Back};
    
    // Update global diagnosticsMenu with new items
    diagnosticsMenu.items = diagnosticsMenuItems;
    diagnosticsMenu.itemCount = 8;
}


