#include "Display.h"

// --- Display State Tracking ---
static float lastDisplayedTargetTemp = -1.0;  // Track last displayed target temp (-1 = never set)

Ticker displayOffTimer(displayOff, SCREEN_TIMEOUT_MS, 1); // One-shot timer
Ticker targetTempDisplayTimer(resetTargetTempDisplay, 1000, 1);  // One-shot timer for 1 second
Ticker screenRefreshTimer(refreshScreen, 5000, 0);  // Refresh screen every 5 seconds (endless)

/**
 * @brief Initializes the SSD1306 display via I2C.
 */
void initDisplay() {
    Serial.println("Initializing display with U8g2...");
    
    display.begin();
    display.enableUTF8Print();
    display.setContrast(20); // Reduce wear out of OLED
    
    // Test pattern
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB14_tr);
    display.drawStr(5, 25, " WELCOME ");
    display.drawStr(5, 55, "DISTILLER");
    display.drawFrame(0, 0, 128, 64);
    display.sendBuffer();
    
    Serial.println("U8g2 display initialized successfully");
    delay(2000);
}

void displayOff() {
    display.clearBuffer();
    display.sendBuffer();
}

String verboseValves()
{
    char buf[32];
    String valve_s = "BODY";
    if (lastSensorData.valveHeads) valve_s = "HEADS";
    else if (lastSensorData.valveTails) valve_s = "TAILS";
    return "Out: " + valve_s;
}

/**
 * @brief Updates the OLED screen for Operational Mode (IP & Temp).
 * @param tempC The current temperature reading.
 */
void displayOperationalMode(float tempC) {
    display.clearBuffer();
    
    // Line 1: Mode/Status
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(0, 7, "-- Operational Mode --");
    
    // Line 2: IP Address
    display.setFont(u8g2_font_6x10_tr);
    String ipStr = "IP: " + WiFi.localIP().toString();
    display.drawStr(0, 20, ipStr.c_str());
    
    // Line 3 & 4: Temperature (Large Font)
    display.setFont(u8g2_font_fur30_tn);  // Large numbers
    char tempStr[8];
    snprintf(tempStr, sizeof(tempStr), "%.1f", tempC);
    display.drawStr(0, 55, tempStr);
    
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(110, 55, "C");
    
    // Last update time
    display.setFont(u8g2_font_5x7_tr);
    String timestamp = getCurrentDateTime().substring(11, 19);
    char buf[64];
    snprintf(buf, sizeof(buf), "V:%s, U:%s", APP_VERSION, timestamp);
    display.drawStr(0, 63, buf);
    display.sendBuffer();
}

/**
 * @brief Updates the OLED screen for Captive Portal Mode (AP Name & Password).
 * @param ap_ssid The AP SSID.
 * @param ap_password The AP password.
 */
void displayCaptivePortalMode(const String& ap_ssid, const String& ap_password) {
    display.clearBuffer();
    
    // Line 1: Mode/Status
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(0, 7, "-- CAPTIVE PORTAL --");
    
    // Line 2: AP SSID
    display.setFont(u8g2_font_6x10_tr);
    String ssidStr = "SSID: " + ap_ssid;
    display.drawStr(0, 18, ssidStr.c_str());
    
    // Line 3: AP IP
    String ipStr = "IP: " + WiFi.softAPIP().toString();
    display.drawStr(0, 28, ipStr.c_str());
    
    // Line 4 & 5: Password (Larger)
    display.setFont(u8g2_font_ncenB10_tr);
    display.drawStr(0, 42, "PASS:");
    display.setFont(u8g2_font_ncenB14_tr);
    display.drawStr(0, 60, ap_password.c_str());
    
    display.sendBuffer();
}

/**
 * @brief Display the Running Mode screen with operational data.
 * Shows: Mode name, temperature (large), cooler/heater status, valve states, flow counters.
 * @param tempC Current temperature reading.
 * @param showMenu Whether to show menu access indicator.
 * @param modeName Mode name string (e.g., "Water", "1st Ethanol").
 * @param heater Heater state (true = ON).
 * @param cooler Cooler state (true = ON).
 */
void displayRunningMode(float tempC, const char* modeName, bool heater, bool cooler) {
    extern DistilMode selectedDistilMode;
    extern float targetTemp;
    
    float displayTemp = tempC;  // Default to actual temperature
    
    if (selectedDistilMode == DISTIL_MANUAL) {
        // Check if target temp changed (new rotation detected by InputManager/main)
        if (targetTemp != lastDisplayedTargetTemp) {
            lastDisplayedTargetTemp = targetTemp;
            displayTemp = targetTemp;
            targetTempDisplayTimer.stop();
            targetTempDisplayTimer.start();
        }
        // Show target temp if flag is set (Ticker will clear it after 1 second)
        else if (targetTempDisplayTimer.state() == RUNNING) {
            displayTemp = targetTemp;
        }
        // Otherwise, show actual temperature
        else {
            displayTemp = tempC;
        }
    }

    display.clearBuffer();
    
    // Line 1: Title
    display.setFont(u8g2_font_5x7_tr);
    const char* titleStr = "Distilling: click menu";
    display.drawStr(0, 7, titleStr);
    
    // Line 2-3: Mode name and temperature on left, status icons on right
    display.setFont(u8g2_font_6x10_tr);
    
    // Mode name (left)
    char modeStr[32];
    snprintf(modeStr, sizeof(modeStr), "Mode: %s", modeName);
    display.drawStr(0, 18, modeStr);
    
    // Temperature (large font, center-left)
    display.setFont(u8g2_font_fur30_tn);
    char tempStr[8];
    snprintf(tempStr, sizeof(tempStr), "%.1f", displayTemp);
    uint8_t tempX = 5;
    display.drawStr(tempX, 50, tempStr);
    
    // °C unit (right of temperature)
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(80, 35, "C");
    
    // Cooler/Heater status (right side)
    display.setFont(u8g2_font_5x7_tr);
    if (heater) display.drawStr(95, 35, "Heater");
    if (cooler) display.drawStr(95, 42, "Cooler");
    
    // Bottom line: Valve states + flow counters
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(0, 60, verboseValves().c_str());
    float vol = lastSensorData.bodyVolume;
    if (lastSensorData.valveHeads) {
        vol = lastSensorData.headsVolume;
    } else if (lastSensorData.valveTails) {
        vol = lastSensorData.tailsVolume;
    }
    char volStr[16];
    snprintf(volStr, sizeof(volStr), "%.1fmL", vol);
    display.drawStr(65, 60, volStr);

    display.sendBuffer();

}

/**
 * @brief Display the Paused Mode screen.
 * Similar to Running Mode but shows "PAUSED" header and different menu.
 * @param tempC Current temperature reading.
 * @param showMenu Whether to show menu access indicator.
 * @param modeName Mode name string (e.g., "Water", "1st Ethanol").
 * @param heater Heater state (true = ON).
 * @param cooler Cooler state (true = ON).
 */
void displayPausedMode(float tempC, const char* modeName, bool heater, bool cooler) {
    display.clearBuffer();
    
    // Line 1: Title
    display.setFont(u8g2_font_5x7_tr);
    const char* titleStr = "PAUSED: click menu";
    display.drawStr(0, 7, titleStr);
    
    // Line 2-3: Mode name and temperature on left, status icons on right
    display.setFont(u8g2_font_6x10_tr);
    
    // Mode name (left)
    char modeStr[32];
    snprintf(modeStr, sizeof(modeStr), "Mode: %s", modeName);
    display.drawStr(0, 18, modeStr);
    
    // Temperature (large font, center-left)
    display.setFont(u8g2_font_fur30_tn);
    char tempStr[8];
    snprintf(tempStr, sizeof(tempStr), "%.1f", tempC);
    uint8_t tempX = 5;
    display.drawStr(tempX, 50, tempStr);
    
    // °C unit (right of temperature)
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(80, 35, "C");
    
    // Status (right side, cooler/heater status)
    display.setFont(u8g2_font_5x7_tr);
    if (heater) display.drawStr(95, 35, "Heater");
    if (cooler) display.drawStr(95, 42, "Cooler");
    
    // Bottom line: Valve states + flow counters
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(0, 60, verboseValves().c_str());
    float vol = lastSensorData.bodyVolume;
    if (lastSensorData.valveHeads) {
        vol = lastSensorData.headsVolume;
    } else if (lastSensorData.valveTails) {
        vol = lastSensorData.tailsVolume;
    }
    char volStr[16];
    snprintf(volStr, sizeof(volStr), "%.1fmL", vol);
    display.drawStr(65, 60, volStr);

    display.sendBuffer();
}

/**
 * @brief Display the Ended Mode screen.
 * Shows mode, temperature, and elapsed time.
 * @param tempC Current temperature reading.
 * @param elapsedMs Total elapsed time in milliseconds.
 * @param modeName Mode name string (e.g., "Water", "1st Ethanol").
 */
void displayEndedMode(float tempC, unsigned long elapsedMs, const char* modeName) {
    display.clearBuffer();
    
    // Line 1: Title
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(0, 7, "ENDED: Shut Down");
    
    // Line 2: Mode name
    display.setFont(u8g2_font_6x10_tr);
    char modeStr[32];
    snprintf(modeStr, sizeof(modeStr), "Mode: %s", modeName);
    display.drawStr(0, 18, modeStr);
    
    // Line 3-4: Temperature (large font)
    display.setFont(u8g2_font_fur30_tn);
    char tempStr[8];
    snprintf(tempStr, sizeof(tempStr), "%.1f", tempC);
    uint8_t tempX = 5;
    display.drawStr(tempX, 50, tempStr);
    
    // °C unit
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(80, 35, "C");
    
    // Elapsed time calculation
    unsigned long elapsedSeconds = elapsedMs / 1000;
    unsigned long hours = elapsedSeconds / 3600;
    unsigned long minutes = (elapsedSeconds % 3600) / 60;
    unsigned long seconds = elapsedSeconds % 60;
    
    // Format and display elapsed time
    display.setFont(u8g2_font_5x7_tr);
    char elapsedStr[20];
    snprintf(elapsedStr, sizeof(elapsedStr), "Elapsed: %02lu:%02lu:%02lu", hours, minutes, seconds);
    display.drawStr(0, 60, elapsedStr);
    
    display.sendBuffer();
}

void displayOTAProgress() {
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB14_tr);
    display.drawStr(0, 20, "OTA IN");
    display.setFont(u8g2_font_ncenB10_tr);
    display.drawStr(0, 40, "PROGRESS");
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(0, 55, "DO NOT UNPLUG!");
    display.sendBuffer();
}

void displayWiFiFailed() {
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB10_tr);
    display.drawStr(0, 20, "WiFi FAILED");
    display.drawStr(0, 35, "RESTARTING...");
    display.sendBuffer();
}
/**
 * @brief Generic menu rendering function.
 * Displays a menu with title, items, and highlights selected item.
 * @param menu Reference to Menu structure containing items and selection state.
 */
void drawMenu(const Menu& menu, const char* modeName) {
    display.clearBuffer();
    
    // Draw title (font 5x7)
    display.setFont(u8g2_font_5x7_tr);
    char modeStr[32];
    snprintf(modeStr, sizeof(modeStr), "%s: choose, click", modeName);
    display.drawStr(0, 7, modeStr);
    
    // Draw menu items (font 6x10)
    // Display area: lines 8-63 (55 pixels), each item ~10 pixels high
    // Can fit ~5-6 items with 6x10 font
    display.setFont(u8g2_font_6x10_tr);
    
    const uint8_t ITEM_HEIGHT = 10;
    const uint8_t START_Y = 18;
    const uint8_t MAX_VISIBLE_ITEMS = 5;
    
    // Calculate which items to display (with scrolling support)
    uint8_t startIdx = 0;
    if (menu.selectedIndex >= MAX_VISIBLE_ITEMS) {
        startIdx = menu.selectedIndex - MAX_VISIBLE_ITEMS + 1;
    }
    uint8_t endIdx = startIdx + MAX_VISIBLE_ITEMS;
    if (endIdx > menu.itemCount) {
        endIdx = menu.itemCount;
        if (endIdx > MAX_VISIBLE_ITEMS) {
            startIdx = endIdx - MAX_VISIBLE_ITEMS;
        } else {
            startIdx = 0;
        }
    }
    
    // Draw visible items
    for (uint8_t i = startIdx; i < endIdx; i++) {
        uint8_t displayY = START_Y + (i - startIdx) * ITEM_HEIGHT;
        
        // Highlight selected item
        if (i == menu.selectedIndex) {
            // Draw inverted box for selection
            display.setDrawColor(1);
            display.drawBox(0, displayY - 9, 128, 10);
            display.setDrawColor(0);  // Invert text color
        }
        
        // Draw arrow and label
        char buf[32];
        if (i == menu.selectedIndex) {
            snprintf(buf, sizeof(buf), "%s", menu.items[i].label);
        } else {
            snprintf(buf, sizeof(buf), "%s", menu.items[i].label);
        }
        display.drawStr(5, displayY, buf);
        
        // Restore normal drawing for next item
        if (i == menu.selectedIndex) {
            display.setDrawColor(1);
        }
    }
    
    display.sendBuffer();
}



void resetTargetTempDisplay() {
    // Called after 1 second to reset target temp display flag
    screenNeedsUpdate = true;  // Force display refresh
}

void refreshScreen() {
    // Called every 5 seconds to update temperature and sensor data continuously
    if (currentScreen == UI_RUNNING_STATE || currentScreen == UI_PAUSED_STATE || currentScreen == UI_ENDED_STATE) {
        screenNeedsUpdate = true;
    }
}