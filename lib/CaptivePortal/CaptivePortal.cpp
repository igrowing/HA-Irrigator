#include "CaptivePortal.h"

WebServer server(80);
DNSServer dnsServer;

const char AP_PASSWORD[] = "password123";

String generateCaptivePortalHtml() {
    String html = CAPTIVE_PORTAL_HTML;


    String timezoneOptions = generateTimezoneOptions(config.timeshift_hours);


    html.replace("%WIFI_SSID_VALUE%", config.wifi_ssid);
    html.replace("%WIFI_PASS_VALUE%", config.wifi_pass);
    html.replace("%MQTT_BROKER_VALUE%", config.mqtt_broker);
    html.replace("%MQTT_USER_VALUE%", config.mqtt_user);
    html.replace("%MQTT_PASS_VALUE%", config.mqtt_pass);
    html.replace("%MQTT_TOPIC_VALUE%", config.mqtt_topic);


    html.replace("%TIMEZONE_OPTIONS_PLACEHOLDER%", timezoneOptions);
    return html;
}

void handleRoot() {
    String html = generateCaptivePortalHtml();
    server.send(200, "text/html", html);
}

void handleSaveConfig() {
    Serial.println("Save config received...");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    Serial.println("Save config posted...");
    // Check if required fields exist
    if (!server.hasArg("wifi_ssid") || !server.hasArg("mqtt_broker") ||
        !server.hasArg("mqtt_topic") || !server.hasArg("timeshift")) {
        server.send(200, "text/html", generateCaptivePortalHtml() + "<p style='color:red;'>Missing required fields!</p>");
        return;
    }

    Serial.println("Save config ready...");
    // Update global config object
    config.wifi_ssid = server.arg("wifi_ssid");
    config.wifi_pass = server.arg("wifi_pass");
    config.mqtt_broker = server.arg("mqtt_broker");
    config.mqtt_user = server.arg("mqtt_user");
    config.mqtt_pass = server.arg("mqtt_pass");
    config.mqtt_topic = server.arg("mqtt_topic");
    config.timeshift_hours = server.arg("timeshift").toInt();

    Serial.println("Testing WiFi configuration...");
    Serial.printf("Attempting to connect to SSID: %s\n", config.wifi_ssid.c_str());

    // Switch to AP+STA mode to keep client connected while connecting to router
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());

    unsigned long startAttemptTime = millis();
    bool connected = false;
    
    // Wait up to 15 seconds for connection
    while (millis() - startAttemptTime < 15000) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(100);
        // We can optionally yield or handle clients here, but a blocking wait is safer for logic simplicity
    }

    if (connected) {
        Serial.println("WiFi Connection Successful!");
        Serial.print("Local IP Address: ");
        Serial.println(WiFi.localIP());

        if (saveConfig(config)) {
            String html = CONFIG_SAVED_HTML;
            html.replace("%WIFI_IP%", WiFi.localIP().toString());
            server.send(200, "text/html", html);
            Serial.println("Config saved. Restarting in 3 seconds...");
            delay(3000);
            ESP.restart();
        } else {
            server.send(200, "text/html", generateCaptivePortalHtml() + "<p style='color:red;'>Failed to save configuration!</p>");
            Serial.println("Save config failed...");  
        }
    } else {
        Serial.println("WiFi Connection Failed! Incorrect SSID or Password.");

        deleteConfig();

        // Notify user via HTML
        String errorHTML = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><style>body{font-family:sans-serif;text-align:center;padding:20px;background:#f0f4f8;}h1{color:#dc3545;}.container{background:#fff;padding:30px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:450px;margin:auto;}.btn{display:inline-block;padding:12px 24px;background:#007bff;color:white;text-decoration:none;border-radius:5px;font-weight:bold;margin-top:20px;}</style></head><body>";
        errorHTML += "<div class='container'><h1>Connection Failed!</h1>";
        errorHTML += "<p>Could not connect to <strong>" + config.wifi_ssid + "</strong>.</p>";
        errorHTML += "<p>Please verify your password and try again.</p>";
        errorHTML += "<a href='/' class='btn'>Go Back</a></div></body></html>";
        
        server.send(200, "text/html", errorHTML);
        
        // Note: stay in the current mode (AP_STA) so user can hit 'Go Back'
    }
}

void handleNotFound() {
    // This is the core of the Captive Portal: redirect any unknown request to the root handler
    if (server.hostHeader().indexOf("esp32-setup.local") < 0) {
        // Redirect if not the setup address (for Captive Portal)
        server.sendHeader("Location", "http://192.168.4.1/");
        server.send(302, "text/plain", "Redirecting to Captive Portal");
    } else {
        server.send(404, "text/plain", "File Not Found");
    }
}

// --- Mode Initialization Functions ---
void init_captive_portal() {
    Serial.println("Initializing Captive Portal Mode...");
    
    // Set up Access Point (AP)
    WiFi.mode(WIFI_AP);
    IPAddress localIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(localIP, gateway, subnet);
    

    String ap_ssid = "DISTILLER_SETUP_1234";
    WiFi.softAP(ap_ssid.c_str(), AP_PASSWORD);
    
    Serial.print("AP started. SSID: ");
    Serial.println(ap_ssid);
    Serial.print("AP started. IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("AP started. PASS: >");
    Serial.print(AP_PASSWORD);
    Serial.println("<");
    
    // Update display immediately
    displayCaptivePortalMode(ap_ssid, AP_PASSWORD);

    // Start DNS server (port 53)
    dnsServer.start(53, "*", localIP);

    // Set up Web Server routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSaveConfig);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP server started.");
    
}

void loop_captive_portal() {
    // Process Captive Portal requests (DNS and HTTP)
    dnsServer.processNextRequest();
    server.handleClient();
    
    // Only update the display periodically since the content is static
    static unsigned long last_display_update = 0;
    if (millis() - last_display_update > 5000) { // Update every 5 seconds
        String ap_ssid = WiFi.softAPSSID();
        displayCaptivePortalMode(ap_ssid, AP_PASSWORD);
        last_display_update = millis();
    }
}
