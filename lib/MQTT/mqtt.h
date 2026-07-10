#ifndef MQTT_H
#define MQTT_H
#define MQTT_MAX_PACKET_SIZE 2048  // Buffer for incoming messages

#include <WiFi.h>
#include <PubSubClient.h> // Library for MQTT client
#include <HTTPClient.h>   // Library for downloading firmware
#include <Update.h>       // Library for flashing firmware
#include "Config.h"       // For AppState, Config struct
#include <ArduinoJson.h>
#include "Helpers.h"
#include <SPIFFS.h>
#include <Ticker.h>

// --- External Global Variables from main.ino ---
// These variables are defined in main.ino but needed here.
extern Config config;
extern String mac_address;

// --- Global MQTT Objects (Defined in mqtt.cpp) ---
extern WiFiClient espClient;
extern PubSubClient mqttClient;
extern bool ota_in_progress; // Flag to stop main loop during OTA update
extern Ticker mqttBootPublishTimer;

// --- MQTT and OTA Constants ---
#pragma once
extern const char* OTA_TOPIC_SUFFIX;
extern const char* FACTORY_RESET_TOPIC_SUFFIX;
extern const char* UPDATE_CONFIG_TOPIC_SUFFIX;

// --- Function Declarations ---

/**
 * @brief Initializes the MQTT client setup (setting server and callback).
 */
void initMQTT(); 

/**
 * @brief Must be called in the main loop to keep the MQTT connection alive 
 * and process incoming messages. It handles reconnection automatically.
 */
void mqttLoop();
void reconnectMQTT();

/**
 * @brief Builds a JSON payload and publishes sensor data to the MQTT broker.
 * * @param tempC The temperature reading in Celsius.
 * @param trend The current temperature trend (e.g., "Heating").
 * @param timestamp The current date/time string.
 */
void mqttPublishData(float tempC);
void updateConfig(const byte* payload, unsigned int length);
void otaUpdate(const byte* payload, unsigned int length);
void factoryReset();
void mqttPublish(String topic, JsonDocument& doc);

/**
 * @brief Publishes a boot report with device info, datetime, boot reason, RAM usage, IP, and config.
 */
void publishBootReport();

/**
 * @brief Handles incoming MQTT messages (especially the OTA update command).
 */
void mqttCallback(char* topic, byte* payload, unsigned int length);

void updateConfig(const byte* payload, unsigned int length);

#endif // MQTT_H