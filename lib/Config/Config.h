#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <FS.h>             // File System base class
#include <SPIFFS.h>         // File System Implementation (default)
#include <ArduinoJson.h>    // JSON handling for config

// File system constant
#pragma once
extern const char* CONFIG_FILE;
extern const char* CONFIG_FILE_BAK;

// Application States
enum AppMode {
    APP_MODE_SETUP,
    APP_MODE_OPERATIONAL,
    APP_MODE_CAPTIVE_PORTAL
};

// Configuration structure
struct Config {
    String wifi_ssid = "";
    String wifi_pass = "";
    String mqtt_broker = "";
    String mqtt_user = "mqtt_user";
    String mqtt_pass = "";
    String mqtt_topic = "esp/temp";
    int timeshift_hours = 1; // UTC offset in hours
};
extern Config config; 

// Forward declaration for HTML templates
String generateTimezoneOptions(int current_timeshift = 0);
bool deleteConfig(const char* filename = CONFIG_FILE);
bool backupConfig();
bool restoreConfig();
bool updateConfig();
bool saveConfig(const Config& newConfig);
bool loadConfig(Config& config);

#endif // CONFIG_H