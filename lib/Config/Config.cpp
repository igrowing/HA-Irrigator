#include "Config.h"

const char* CONFIG_FILE = "/config.json";
const char* CONFIG_FILE_BAK = "/config.json.bak";
Config config;

// --- Configuration Management (Using SPIFFS) ---

bool loadConfig(Config& config) {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
        return false;
    }

    File configFile = SPIFFS.open(CONFIG_FILE, "r");
    if (!configFile) {
        Serial.println("Failed to open config file for reading.");
        SPIFFS.end();
        return false;
    }

    size_t size = configFile.size();
    if (size > 2048) { // Max 2KB file size
        Serial.println("Config file size is too large");
        configFile.close();
        SPIFFS.end();
        return false;
    }

    // Allocate a buffer to store contents
    JsonDocument doc; 
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    SPIFFS.end();

    if (error) {
        Serial.print("Failed to parse config file: ");
        Serial.println(error.c_str());
        return false;
    }

    // Load values into the struct
    config.wifi_ssid = doc["wifi_ssid"].as<String>();
    config.wifi_pass = doc["wifi_pass"].as<String>();
    config.mqtt_broker = doc["mqtt_broker"].as<String>();
    config.mqtt_user = doc["mqtt_user"].as<String>();
    config.mqtt_pass = doc["mqtt_pass"].as<String>();
    config.mqtt_topic = doc["mqtt_topic"].as<String>();
    config.timeshift_hours = doc["timeshift"].as<int>();
    
    Serial.println("Configuration loaded successfully.");
    Serial.printf("Wi-Fi SSID: %s\n", config.wifi_ssid.c_str());
    Serial.printf("MQTT Broker: %s\n", config.mqtt_broker.c_str());
    Serial.printf("Timezone Offset: UTC%+d\n", config.timeshift_hours);
    return true;
}


bool deleteConfig(const char* filename)
{
    bool res = false;
    // Delete config file if it exists so we don't boot into a broken state
    if (SPIFFS.begin(true))
    {
        if (SPIFFS.exists(filename))
        {
            res = SPIFFS.remove(filename);
            Serial.printf("%s removed.\n", filename);
        }
        SPIFFS.end();
    }
    return res;
}

bool backupConfig()
{
    bool res = false;
    deleteConfig(CONFIG_FILE_BAK);
    if (SPIFFS.begin(true))
    {
        if (SPIFFS.exists(CONFIG_FILE))
        {
            res = SPIFFS.rename(CONFIG_FILE, CONFIG_FILE_BAK);
            Serial.println("config.json renamed as backup.");
        }
        SPIFFS.end();
    }
    return res;
}

bool restoreConfig()
{
    bool res = false;
    deleteConfig();
    if (SPIFFS.begin(true))
    {
        if (SPIFFS.exists(CONFIG_FILE_BAK))
        {
            res = SPIFFS.rename(CONFIG_FILE_BAK, CONFIG_FILE);
            Serial.println("config.json restored from backup.");
        }
        SPIFFS.end();
    }
    return res;
}

bool saveConfig(const Config& newConfig) {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
        return false;
    }

    // Create JSON document
    JsonDocument doc;
    doc["wifi_ssid"] = newConfig.wifi_ssid;
    doc["wifi_pass"] = newConfig.wifi_pass;
    doc["mqtt_broker"] = newConfig.mqtt_broker;
    doc["mqtt_user"] = newConfig.mqtt_user;
    doc["mqtt_pass"] = newConfig.mqtt_pass;
    doc["mqtt_topic"] = newConfig.mqtt_topic;
    doc["timeshift"] = newConfig.timeshift_hours;

    File configFile = SPIFFS.open(CONFIG_FILE, "w");
    if (!configFile) {
        Serial.println("Failed to open config file for writing.");
        SPIFFS.end();
        return false;
    }

    // Serialize JSON to file
    if (serializeJson(doc, configFile) == 0) {
        Serial.println("Failed to write to file.");
        configFile.close();
        SPIFFS.end();
        return false;
    }
    
    configFile.close();
    SPIFFS.end();
    Serial.println("Configuration saved successfully.");
    return true;
}
