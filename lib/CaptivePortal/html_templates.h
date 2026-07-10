#ifndef HTML_TEMPLATES_H
#define HTML_TEMPLATES_H

#include <Arduino.h>

// --- HTML Template Components ---

/**
 * @brief Generates the timezone selection options for the select dropdown.
 * The options range from UTC-12 to UTC+14.
 * * @param current_timeshift The currently configured UTC offset in hours.
 * @return String A string containing the HTML <option> elements, with the saved value marked as 'selected'.
 */
inline String generateTimezoneOptions(int current_timeshift) {
    String options = "";
    // UTC offsets usually range from -12 to +14
    for (int i = -12; i <= 14; i++) {
        String sign = (i >= 0) ? "+" : "";
        // Check if this option matches the currently saved value
        String selected = (i == current_timeshift) ? " selected" : "";
        
        // Output format: <option value='-5' selected>UTC-5</option>
        options += "<option value='";
        options += i;
        options += "'";
        options += selected;
        options += ">UTC";
        options += sign;
        options += i;
        options += "</option>";
    }
    return options;
}

// Placeholders for value injection:
// %WIFI_SSID_VALUE%, %WIFI_PASS_VALUE%, %MQTT_BROKER_VALUE%, %MQTT_USER_VALUE%, %MQTT_PASS_VALUE%, %MQTT_TOPIC_VALUE%
// %TIMEZONE_OPTIONS_PLACEHOLDER%
const char CAPTIVE_PORTAL_HTML[] PROGMEM = R"raw(
<!DOCTYPE html>
<html>
<head>
    <title>Distiller Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu, Cantarell, "Open Sans", "Helvetica Neue", sans-serif; text-align: center; margin: 0; background: #f0f4f8; color: #333; }
        .container { background: #ffffff; padding: 25px; border-radius: 12px; box-shadow: 0 6px 15px rgba(0,0,0,0.1); max-width: 450px; margin: 40px auto; }
        h1 { color: #007bff; font-size: 1.8em; margin-bottom: 5px; }
        p { color: #666; margin-top: 5px; margin-bottom: 25px; }
        form { text-align: left; }
        .input-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; font-weight: 600; color: #555; }
        input[type="text"], input[type="password"], select {
            width: 100%;
            padding: 10px;
            border: 1px solid #ccc;
            border-radius: 6px;
            box-sizing: border-box;
            font-size: 1em;
            transition: border-color 0.3s;
        }
        input[type="text"]:focus, input[type="password"]:focus, select:focus {
            border-color: #007bff;
            outline: none;
        }
        button[type="submit"] {
            width: 100%;
            padding: 12px;
            background-color: #28a745;
            color: white;
            border: none;
            border-radius: 6px;
            font-size: 1.1em;
            cursor: pointer;
            margin-top: 20px;
            transition: background-color 0.3s;
        }
        button[type="submit"]:hover { background-color: #218838; }
        hr { border: 0; border-top: 1px solid #eee; margin: 20px 0; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Distiller Setup</h1>
        <p>Configure Wi-Fi, MQTT, and Timezone.</p>
        <form method="post" action="/save">
            <h2>Wi-Fi Settings</h2>
            <div class="input-group"><label for="wifi_ssid">Wi-Fi SSID</label><input type="text" id="wifi_ssid" name="wifi_ssid" placeholder="Your Wi-Fi Name" value="%WIFI_SSID_VALUE%" required></div>
            <div class="input-group"><label for="wifi_pass">Wi-Fi Password</label><input type="password" id="wifi_pass" name="wifi_pass" placeholder="Your Wi-Fi Password" value="%WIFI_PASS_VALUE%" required></div>
            
            <hr>
            
            <h2>MQTT Settings (Optional)</h2>
            <div class="input-group"><label for="mqtt_broker">MQTT Broker Address</label><input type="text" id="mqtt_broker" name="mqtt_broker" placeholder="e.g., test.mosquitto.org" value="%MQTT_BROKER_VALUE%"></div>
            <div class="input-group"><label for="mqtt_user">MQTT Username (Optional)</label><input type="text" id="mqtt_user" name="mqtt_user" placeholder="Username (optional)" value="%MQTT_USER_VALUE%"></div>
            <div class="input-group"><label for="mqtt_pass">MQTT Password (Optional)</label><input type="password" id="mqtt_pass" name="mqtt_pass" placeholder="Password (optional)" value="%MQTT_PASS_VALUE%"></div>
            <div class="input-group"><label for="mqtt_topic">MQTT Topic</label><input type="text" id="mqtt_topic" name="mqtt_topic" placeholder="e.g., sensor/temperature" value="%MQTT_TOPIC_VALUE%" required></div>
            
            <hr>

            <h2>Timezone Settings</h2>
            <label for="timeshift">Timezone (UTC Offset):</label>
            <select id="timeshift" name="timeshift" required>
                %TIMEZONE_OPTIONS_PLACEHOLDER%
            </select>
            
            <button type="submit">Save Configuration & Restart</button>
        </form>
    </div>
</body>
</html>
)raw";

const char CONFIG_SAVED_HTML[] PROGMEM = R"raw(
<!DOCTYPE html>
<html>
<head>
    <title>Configuration Saved</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: sans-serif; text-align: center; margin: 40px; background: #f0f4f8; color: #333; }
        .container { background: #ffffff; padding: 40px; border-radius: 12px; box-shadow: 0 6px 15px rgba(0,0,0,0.1); max-width: 400px; margin: auto; }
        h2 { color: #38a13a; margin-top: 0; }
        p { color: #666; }
        a { color: #007bff; text-decoration: none; font-weight: 600; }
        a:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <div class="container">
        <h2>Configuration Saved!</h2>
        <p>The WiFi IP: "%WIFI_IP%</p>
        <p>The device is now restarting to apply new settings.</p>
        <p>If the captive portal does not close, please disconnect from the access point.</p>
    </div>
</body>
</html>
)raw";

#endif // HTML_TEMPLATES_H