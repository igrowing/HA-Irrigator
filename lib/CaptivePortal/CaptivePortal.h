#ifndef CAPTIVEPORTAL_H
#define CAPTIVEPORTAL_H
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "Helpers.h"
#include "Config.h"
#include "html_templates.h"
#include "Display.h"
#include <WiFiClient.h>
#include <DNSServer.h>
#include <WebServer.h>

extern WebServer server;
extern DNSServer dnsServer;

String generateCaptivePortalHtml();
void loop_captive_portal();
void loop_operational_mode();
extern const char AP_PASSWORD[];
void init_captive_portal();
void handleNotFound();
void handleSaveConfig();
void handleRoot();

#endif // CAPTIVEPORTAL_H