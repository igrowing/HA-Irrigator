#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>           // I2C communication for OLED
#include <WiFi.h>
#include <Ticker.h>
// #include "Helpers.h"
#include "Menu.h"
#include "Sensors.h"
#include "MenuSystem.h"

#define SCREEN_TIMEOUT_MS 300000 // 5 minutes screen timeout

// Forward declarations
void initDisplay();
void displayOff();
void displayOperationalMode(float tempC);
void displayCaptivePortalMode(const String& ap_ssid, const String& ap_password);
void displayOTAProgress();
void displayWiFiFailed();
void drawMenu(const Menu& menu, const char* modeName);
void resetTargetTempDisplay();  // Reset target temp display after 1 second
void refreshScreen();  // Refresh screen every 5 seconds in RUNNING/PAUSED/ENDED states

String verboseValves();

extern Ticker displayOffTimer; // One-shot timer
extern Ticker targetTempDisplayTimer;  // One-shot timer for 1 second
extern Ticker screenRefreshTimer;  // Refresh screen every 5 seconds (endless)

// State display functions
void displayRunningMode(float tempC, const char* modeName, bool heater, bool cooler);
void displayPausedMode(float tempC, const char* modeName, bool heater, bool cooler);
void displayEndedMode(float tempC, unsigned long elapsedMs, const char* modeName);

// External display object (defined in Display.cpp)
extern U8G2_SSD1306_128X64_NONAME_F_SW_I2C display;
#endif // DISPLAY_H
