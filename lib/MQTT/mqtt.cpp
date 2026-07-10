#include "mqtt.h"

// Reconnection timing - used for non-blocking reconnect attempts
static unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL_MIN = 5000;   // Min 5 seconds between attempts
const unsigned long MQTT_RECONNECT_INTERVAL_MAX = 60000;  // Max 60 seconds (exponential backoff)
static unsigned long mqttReconnectBackoff = MQTT_RECONNECT_INTERVAL_MIN;
static uint8_t mqttReconnectAttempts = 0;

// --- Global MQTT Objects Definitions ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Ticker mqttBootPublishTimer(publishBootReport, 15000, 1);

// Configure PubSubClient buffer sizes for proper message handling
// Default is often too small and causes disconnections
// These are set in initMQTT() via setBufferSize()

bool ota_in_progress = false;
const char* OTA_TOPIC_SUFFIX = "/ota";
const char* FACTORY_RESET_TOPIC_SUFFIX = "/factoryReset";
const char* UPDATE_CONFIG_TOPIC_SUFFIX = "/updateConfig";

// --- Helper Functions ---

/**
 * @brief Sends a response message to the MQTT broker on the response topic.
 * @param baseTopic The base topic (e.g., "device/123/factoryReset")
 * @param success Whether the operation was successful
 * @param message The message to include in the response
 */
void sendMqttResponse(const String& baseTopic, bool success, const String& message) {
    if (!mqttClient.connected()) {
        Serial.println("Response failed: MQTT not connected.");
        return;
    }

    // Build response topic by adding /res suffix
    String responseTopic = baseTopic + "/res";
    
    // Build JSON response
    JsonDocument doc;
    doc["device_id"] = mac_address;
    doc["success"] = success;
    doc["message"] = message;
    doc["timestamp"] = getCurrentDateTime(); // Or use actual timestamp if available

    String payload;
    serializeJson(doc, payload);

    Serial.printf("Sending response to %s: %s\n", responseTopic.c_str(), payload.c_str());
    
    if (mqttClient.publish(responseTopic.c_str(), payload.c_str())) {
        Serial.println("Response sent successfully.");
    } else {
        Serial.printf("Response failed. MQTT state: %d\n", mqttClient.state());
    }
}

/**
 * @brief Performs a factory reset by deleting the config file and restarting.
 */
void factoryReset() {
    Serial.println("Factory Reset: Starting...");
    
    String baseTopic = config.mqtt_topic + FACTORY_RESET_TOPIC_SUFFIX;
    
    bool res = deleteConfig();

    // Delete the config file
    if (res) {
        sendMqttResponse(baseTopic, true, "Config deleted, restarting...");
        SPIFFS.end();
        delay(100); // Give time for MQTT message to send
        ESP.restart();
    } else {
        sendMqttResponse(baseTopic, false, "Failed to delete config file");
        SPIFFS.end();
    }
}

// --- OTA Function Implementation ---

/**
 * @brief Performs the OTA update by downloading a binary file from a given URL.
 * * This function blocks execution until the download and flash process is complete.
 * If successful, it restarts the ESP32.
 * @param url The full URL of the firmware binary file.
 */
void otaUpdate(const byte* payload, unsigned int length) {
    if (ota_in_progress) {
        Serial.println("OTA: Update already in progress. Ignoring new request.");
        return;
    }
    
    // Payload should be the URL of the new firmware binary
    String url = "";
    for (unsigned int i = 0; i < length; i++) {
        url += (char)payload[i];
    }
    url.trim();
    Serial.printf("OTA: Received firmware URL: %s\n", url.c_str());

    if (url.length() == 0) {
        Serial.println("OTA: Error - Firmware URL is empty.");
        return;
    }

    Serial.printf("OTA: Starting update from URL: %s\n", url.c_str());
    ota_in_progress = true;

    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("OTA: HTTP GET failed, code: %d\n", httpCode);
        http.end();
        ota_in_progress = false;
        return;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("OTA: Error - Content length is 0 or unknown.");
        http.end();
        ota_in_progress = false;
        return;
    }

    Serial.printf("OTA: Firmware size: %d bytes\n", contentLength);

    if (!Update.begin(contentLength)) {
        Serial.printf("OTA: Error - Not enough space for update: %s\n", Update.errorString());
        http.end();
        ota_in_progress = false;
        return;
    }

    WiFiClient* client = http.getStreamPtr();
    size_t written = Update.writeStream(*client);
    
    if (written == contentLength) {
        Serial.printf("OTA: Written %d bytes successfully.\n", written);
    } else {
        Serial.printf("OTA: Wrote %d of %d bytes. Error.\n", written, contentLength);
        Update.abort();
        http.end();
        ota_in_progress = false;
        return;
    }

    if (Update.end()) {
        if (Update.isFinished()) {
            // TODO: Update to MQTT the results
            Serial.println("OTA: Update successfully finished. Restarting...");
            http.end();
            // Give serial time to flush before restarting
            delay(100); 
            ESP.restart();
        } else {
            // TODO: Update to MQTT the results
            Serial.println("OTA: Update finished but was not complete.");
        }
    } else {
        // TODO: Update to MQTT the results
        Serial.printf("OTA: Error - Update failed. Error code: %s\n", Update.errorString());
    }

    http.end();
    ota_in_progress = false; // Reset flag on error/completion
}


void updateConfig(const byte* payload, unsigned int length) {
    String baseTopic = config.mqtt_topic + UPDATE_CONFIG_TOPIC_SUFFIX;

    // Parse the incoming JSON payload
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        String errorMessage = "Invalid JSON format: ";
        errorMessage += error.c_str();
        sendMqttResponse(baseTopic, false, errorMessage);
        return;
    }
    
    // Update config fields if they exist in the payload
    int updated = 0;
    
    if (doc.containsKey("wifi_ssid")) {
        config.wifi_ssid = doc["wifi_ssid"].as<String>();
        updated++;
        Serial.printf("Update Config: wifi_ssid updated to %s\n", config.wifi_ssid.c_str());
    }
    
    if (doc.containsKey("wifi_pass")) {
        config.wifi_pass = doc["wifi_pass"].as<String>();
        updated++;
        Serial.println("Update Config: wifi_pass updated");
    }
    
    if (doc.containsKey("mqtt_broker")) {
        config.mqtt_broker = doc["mqtt_broker"].as<String>();
        updated++;
        Serial.printf("Update Config: mqtt_broker updated to %s\n", config.mqtt_broker.c_str());
    }
    
    if (doc.containsKey("mqtt_user")) {
        config.mqtt_user = doc["mqtt_user"].as<String>();
        updated++;
        Serial.printf("Update Config: mqtt_user updated to %s\n", config.mqtt_user.c_str());
    }
    
    if (doc.containsKey("mqtt_pass")) {
        config.mqtt_pass = doc["mqtt_pass"].as<String>();
        updated++;
        Serial.println("Update Config: mqtt_pass updated");
    }
    
    if (doc.containsKey("mqtt_topic")) {
        config.mqtt_topic = doc["mqtt_topic"].as<String>();
        updated++;
        Serial.printf("Update Config: mqtt_topic updated to %s\n", config.mqtt_topic.c_str());
    }
    
    if (doc.containsKey("timeshift")) {
        config.timeshift_hours = doc["timeshift"].as<int>();
        updated++;
        Serial.printf("Update Config: timeshift updated to %d\n", config.timeshift_hours);
    }

    if (0 == updated) {
        String msg = "Update Config: No valid fields to update";
        Serial.println(msg);
        sendMqttResponse(baseTopic, false, msg);
        return;
    }

    backupConfig();
    deleteConfig();
    if (saveConfig(config)) {
        deleteConfig(CONFIG_FILE_BAK);
        String msg = "Update Config: ";
        msg += updated;
        msg += " new settings saved, restaring.";
        sendMqttResponse(baseTopic, true, msg);
        Serial.println(msg);
        delay(100); // Give time for MQTT message to send
        ESP.restart(); // Restart to apply new configuration
        return;
    } else {
        restoreConfig();
        String msg = "Update Config: cannot save new configuration.";
        sendMqttResponse(baseTopic, false, msg);
        Serial.println(msg);        
        return;
    };
}

// --- MQTT Callback Handler ---

/**
 * @brief Handles incoming MQTT messages.
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("MQTT: Message arrived on topic: %s\n", topic);

    // Check if the message is for OTA update
    if (String(topic).equals(config.mqtt_topic + OTA_TOPIC_SUFFIX)) {
        // Immediately start the OTA process (blocking for the duration of download/flash)
        otaUpdate(payload, length);
    }
    else if (String(topic).equals(config.mqtt_topic + FACTORY_RESET_TOPIC_SUFFIX)) {
        factoryReset();
    }
    else if (String(topic).equals(config.mqtt_topic + UPDATE_CONFIG_TOPIC_SUFFIX)) {
        updateConfig(payload, length);
    } else {
        Serial.println("MQTT: Unknown message type or topic.");
    }
}

// --- MQTT Connection and Loop Functions ---

/**
 * @brief Initializes the MQTT client setup.
 */
void initMQTT() {
    mqttClient.setServer(config.mqtt_broker.c_str(), 1883);
    mqttClient.setCallback(mqttCallback);
    
    // Set buffer sizes for proper message handling
    // Default buffer is often too small (128-256 bytes) which causes disconnections
    mqttClient.setBufferSize(MQTT_MAX_PACKET_SIZE);
    
    // Configure connection parameters for maximum stability
    // These settings match Home Assistant and other stable MQTT devices
    
    // Keep-alive: 60 seconds (sends PING if no activity for 60s)
    // Home Assistant default is also 60 seconds
    // MUST be less than broker's keep-alive timeout (usually 120s)
    mqttClient.setKeepAlive(60);
    
    // Socket timeout: 30 seconds 
    // PubSubClient expects broker response within this window
    // Set to 2x keep-alive to handle broker delays
    mqttClient.setSocketTimeout(30);
    
    // Reset reconnect backoff on initialization
    mqttReconnectAttempts = 0;
    mqttReconnectBackoff = MQTT_RECONNECT_INTERVAL_MIN;
    
    Serial.printf("MQTT client initialized (Buffer: %d bytes, KeepAlive: 60s, SocketTimeout: 30s).\n", 
                  MQTT_MAX_PACKET_SIZE);
}

/**
 * @brief Attempts to reconnect to the MQTT broker and subscribes to necessary topics.
 * Uses exponential backoff to avoid hammering the broker on connection failures.
 */
void reconnectMQTT() {  
    unsigned long currentTime = millis();
    
    // Only attempt reconnect after the backoff interval has passed
    if (currentTime - lastMqttReconnectAttempt < mqttReconnectBackoff) {
        return;
    }
    
    lastMqttReconnectAttempt = currentTime;

    Serial.println(config.mqtt_topic.c_str());
    Serial.println(config.mqtt_user.c_str());
    Serial.println(config.mqtt_pass.c_str());
    Serial.print(millis() / 1000);
    Serial.printf(" [Attempt %d] Attempting MQTT connection with backoff %lu ms...", 
                  mqttReconnectAttempts + 1, mqttReconnectBackoff);

    if (mqttClient.connect(config.mqtt_topic.c_str(), config.mqtt_user.c_str(), config.mqtt_pass.c_str())) {
        Serial.println("connected!");
        
        // Reset backoff on successful connection
        mqttReconnectAttempts = 0;
        mqttReconnectBackoff = MQTT_RECONNECT_INTERVAL_MIN;
        
        // Subscribe to OTA topic
        String otaTopic = config.mqtt_topic + OTA_TOPIC_SUFFIX;
        mqttClient.subscribe(otaTopic.c_str());
        Serial.printf("Subscribed to OTA topic: %s\n", otaTopic.c_str());

        // Subscribe to Factory Reset topic
        String factoryResetTopic = config.mqtt_topic + FACTORY_RESET_TOPIC_SUFFIX;
        mqttClient.subscribe(factoryResetTopic.c_str());
        Serial.printf("Subscribed to Factory Reset topic: %s\n", factoryResetTopic.c_str());

        // Subscribe to Update Config topic
        String updateConfigTopic = config.mqtt_topic + UPDATE_CONFIG_TOPIC_SUFFIX;
        mqttClient.subscribe(updateConfigTopic.c_str());
        Serial.printf("Subscribed to Update Config topic: %s\n", updateConfigTopic.c_str());

    } else {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        
        // Implement exponential backoff (5s -> 7.5s -> 11s -> 16s -> 24s -> 36s -> 54s -> 60s)
        mqttReconnectAttempts++;
        if (mqttReconnectBackoff < MQTT_RECONNECT_INTERVAL_MAX) {
            mqttReconnectBackoff = min((unsigned long)(mqttReconnectBackoff * 1.5), MQTT_RECONNECT_INTERVAL_MAX);
        }
        Serial.printf(" will retry in %lu ms (attempt %d)\n", mqttReconnectBackoff, mqttReconnectAttempts);
    }
}

/**
 * @brief Must be called in the main loop to keep the MQTT connection alive
 * and process incoming messages. This is now non-blocking and resilient.
 */
void mqttLoop() {
    // Update boot publish timer if running
    if (mqttBootPublishTimer.state() == RUNNING) {
        mqttBootPublishTimer.update();
    }

    // Check WiFi connection first (no point trying MQTT if WiFi is down)
    if (WiFi.status() != WL_CONNECTED) {
        if (mqttClient.connected()) {
            Serial.println("WiFi lost, disconnecting MQTT...");
            mqttClient.disconnect();
        }
        return;
    }

    // Attempt to reconnect if not connected
    if (!mqttClient.connected()) {
        reconnectMQTT();
    } else {
        // Process incoming messages and send keep-alives
        // CRITICAL: This MUST be called frequently (ideally every 100-500ms)
        // PubSubClient needs this to send keep-alive pings and handle subscribed messages
        if (!mqttClient.loop()) {
            // mqttClient.loop() returns false if connection was lost
            Serial.printf("MQTT loop failed, state: %d - disconnecting\n", mqttClient.state());
            mqttClient.disconnect();
        }
    }
}

// --- MQTT Data Publishing ---


void mqttPublish(String topic, JsonDocument& doc) {
    if (!mqttClient.connected()) {
        Serial.println("Publish failed: MQTT not connected.");
        return;
    }

    String payload;
    serializeJson(doc, payload);

    Serial.printf("Publishing to %s: %s\n", topic.c_str(), payload.c_str());

    if (mqttClient.publish(topic.c_str(), payload.c_str())) {
        Serial.println("Publish successful.");
    } else {
        Serial.printf("Publish failed. MQTT state: %d\n", mqttClient.state());
    }
}

/**
 * @brief Builds a JSON payload and publishes sensor data to the MQTT broker.
 */
void mqttPublishData(float tempC) {
    if (!mqttClient.connected()) {
        Serial.println("Publish failed: MQTT not connected.");
        return;
    }

    // Build JSON Payload
    JsonDocument doc;
    doc["temperature_c"] = tempC;
    doc["timestamp"] = getCurrentDateTime();

    String payload;
    serializeJson(doc, payload);

    // Publish
    String topic = config.mqtt_topic + "/temperature";
    Serial.printf("[%s] Publishing to %s: %s\n", doc["timestamp"].as<String>().c_str(), topic.c_str(), payload.c_str());

    if (mqttClient.publish(topic.c_str(), payload.c_str())) {
        Serial.println("Publish successful.");
    } else {
        Serial.printf("Publish failed. MQTT state: %d\n", mqttClient.state());
    }
}

/**
 * @brief Publishes a boot report with device info, datetime, boot reason, RAM usage, IP, and config.
 * This is called at the end of operational mode initialization.
 */
void publishBootReport() {
    // Wait briefly for MQTT to be connected before publishing boot report
    if (!mqttClient.connected()) {
        if (mqttBootPublishTimer.state() != RUNNING) mqttBootPublishTimer.start();
        return;
    }
    
    // Get boot reason
    esp_reset_reason_t resetReason = esp_reset_reason();
    String bootReason = "UNKNOWN";
    
    switch (resetReason) {
        case ESP_RST_UNKNOWN:
            bootReason = "Unknown";
            break;
        case ESP_RST_POWERON:
            bootReason = "Power On";
            break;
        case ESP_RST_EXT:
            bootReason = "External Reset";
            break;
        case ESP_RST_SW:
            bootReason = "Software Reset";
            break;
        case ESP_RST_PANIC:
            bootReason = "Panic/Exception";
            break;
        case ESP_RST_INT_WDT:
            bootReason = "Interrupt Watchdog";
            break;
        case ESP_RST_TASK_WDT:
            bootReason = "Task Watchdog";
            break;
        case ESP_RST_WDT:
            bootReason = "Other Watchdog";
            break;
        case ESP_RST_DEEPSLEEP:
            bootReason = "Deep Sleep";
            break;
        case ESP_RST_BROWNOUT:
            bootReason = "Brownout";
            break;
        case ESP_RST_SDIO:
            bootReason = "SDIO Reset";
            break;
        default:
            bootReason = "Unknown (" + String(resetReason) + ")";
    }
    
    // Get RAM usage
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t usedHeap = totalHeap - freeHeap;
    float heapUsagePercent = (float)usedHeap / totalHeap * 100.0;
    
    // Get IP address
    String ipAddress = WiFi.localIP().toString();
    
    // Build JSON payload
    JsonDocument doc;  // Larger buffer for config data
    doc["event"] = "boot";
    doc["device_id"] = mac_address;
    doc["datetime"] = getCurrentDateTime();
    doc["boot_reason"] = bootReason;
    doc["ram_total_bytes"] = totalHeap;
    doc["ram_used_bytes"] = usedHeap;
    doc["ram_free_bytes"] = freeHeap;
    doc["ram_usage_percent"] = serialized(String(heapUsagePercent, 1));
    doc["ip_address"] = ipAddress;
    doc["rssi"] = WiFi.RSSI();  // Signal strength
    
    // Add current configuration
    JsonObject configObj = doc.createNestedObject("config");
    configObj["mqtt_broker"] = config.mqtt_broker;
    configObj["mqtt_topic"] = config.mqtt_topic;
    configObj["mqtt_user"] = config.mqtt_user;
    configObj["timeshift_hours"] = config.timeshift_hours;
    
    // Publish boot report
    String topic = config.mqtt_topic + "/boot";
    String payload;
    serializeJson(doc, payload);
    
    Serial.printf("Publishing boot report to %s: %s\n", topic.c_str(), payload.c_str());
    
    if (mqttClient.publish(topic.c_str(), payload.c_str())) {
        Serial.println("Boot report published successfully.");
    } else {
        Serial.printf("Boot report publish failed. MQTT state: %d, will retry.\n", mqttClient.state());
        mqttBootPublishTimer.start();
    }
}