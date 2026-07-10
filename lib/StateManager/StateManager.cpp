#include "StateManager.h"
#include <ArduinoJson.h>
#include "Helpers.h"
#include "mqtt.h"
#include "Sensors.h"

// --- State Variables ---
SystemState currentState = STATE_INITIAL;
unsigned long runStartTime = 0;
unsigned long pausedAtTime = 0;
unsigned long totalPausedTime = 0;

// External variables needed for state reporting
extern SensorData lastSensorData;
extern Config config;

// Set the state visible system wide if changed. Don't report if just entered the menu of a state.
void changeStateAndReport(SystemState next) {
  if (currentState == next) return;

  bool report =
    (currentState == STATE_INITIAL && next == STATE_RUNNING) ||
    (currentState == STATE_RUNNING && next == STATE_PAUSED) ||
    (currentState == STATE_RUNNING && next == STATE_ENDED) ||
    (currentState == STATE_PAUSED && next == STATE_RUNNING) ||
    (currentState == STATE_PAUSED && next == STATE_ENDED);

  currentState = next;
  
  if (report) {
    JsonDocument doc;
    doc["state"] = SYSTEM_STATE_STRING[currentState];
    doc["timestamp"] = getCurrentDateTime();
    doc["message"] = "State changed";
    doc["temp_c"] = lastSensorData.temperature;
    doc["wash_l"] = lastSensorData.washVolume;
    doc["heads_ml"] = lastSensorData.headsVolume;
    doc["tails_ml"] = lastSensorData.tailsVolume;
    doc["body_ml"] = lastSensorData.bodyVolume;
    mqttPublish(config.mqtt_topic + "/state", doc);
  }
}
