#pragma once

#include <Arduino.h>

namespace irrigator::network {

// There is deliberately no timezone input here: the device geolocates itself over ip-api.com once
// it's on the network (main.ino's tryGeolocateIfDue) and derives config.json's `timezone` from
// that, so asking the user for something the device already knows only creates a way for the two
// to disagree.

// Placeholders: %SSID_VALUE% %PASSWORD_VALUE% %MQTT_BROKER_VALUE% %MQTT_PORT_VALUE%
// %MQTT_USER_VALUE% %MQTT_PASSWORD_VALUE% %ERROR_MESSAGE%
const char CAPTIVE_PORTAL_HTML[] PROGMEM = R"raw(
<!DOCTYPE html>
<html>
<head>
    <title>Irrigator Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; text-align: center; margin: 0; background: #f0f4f8; color: #333; }
        .container { background: #ffffff; padding: 25px; border-radius: 12px; box-shadow: 0 6px 15px rgba(0,0,0,0.1); max-width: 450px; margin: 40px auto; }
        h1 { color: #007bff; font-size: 1.8em; margin-bottom: 5px; }
        p { color: #666; margin-top: 5px; margin-bottom: 25px; }
        form { text-align: left; }
        .input-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; font-weight: 600; color: #555; }
        input[type="text"], input[type="password"], input[type="number"], select {
            width: 100%; padding: 10px; border: 1px solid #ccc; border-radius: 6px;
            box-sizing: border-box; font-size: 1em;
        }
        button[type="submit"] {
            width: 100%; padding: 12px; background-color: #28a745; color: white;
            border: none; border-radius: 6px; font-size: 1.1em; cursor: pointer; margin-top: 20px;
        }
        button[type="submit"]:hover { background-color: #218838; }
        hr { border: 0; border-top: 1px solid #eee; margin: 20px 0; }
        .error { color: #dc3545; font-weight: 600; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Irrigator Setup</h1>
        <p>Configure Wi-Fi and MQTT. The timezone is detected from your location.</p>
        <p class="error">%ERROR_MESSAGE%</p>
        <form method="post" action="/save">
            <h2>Wi-Fi Settings</h2>
            <div class="input-group"><label for="ssid">Wi-Fi SSID</label><input type="text" id="ssid" name="ssid" value="%SSID_VALUE%" required></div>
            <div class="input-group"><label for="password">Wi-Fi Password</label><input type="password" id="password" name="password" value="%PASSWORD_VALUE%"></div>

            <hr>

            <h2>MQTT Settings (optional)</h2>
            <div class="input-group"><label for="mqtt_broker">MQTT Broker Address</label><input type="text" id="mqtt_broker" name="mqtt_broker" placeholder="Leave blank to disable MQTT" value="%MQTT_BROKER_VALUE%"></div>
            <div class="input-group"><label for="mqtt_port">MQTT Broker Port</label><input type="number" id="mqtt_port" name="mqtt_port" min="1" max="65535" value="%MQTT_PORT_VALUE%"></div>
            <div class="input-group"><label for="mqtt_user">MQTT Username (optional)</label><input type="text" id="mqtt_user" name="mqtt_user" value="%MQTT_USER_VALUE%"></div>
            <div class="input-group"><label for="mqtt_password">MQTT Password (optional)</label><input type="password" id="mqtt_password" name="mqtt_password" value="%MQTT_PASSWORD_VALUE%"></div>

            <button type="submit">Save Configuration &amp; Restart</button>
        </form>
    </div>
</body>
</html>
)raw";

// Placeholders: %DEVICE_IP% %MQTT_STATUS_LINE%
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
    </style>
</head>
<body>
    <div class="container">
        <h2>Configuration Saved!</h2>
        <p>%MQTT_STATUS_LINE%</p>
        <p>Device IP: <strong>%DEVICE_IP%</strong></p>
        <p>The device is now restarting to apply the new settings.</p>
    </div>
</body>
</html>
)raw";

}  // namespace irrigator::network
