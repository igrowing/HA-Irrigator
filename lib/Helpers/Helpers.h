#ifndef HELPERS_H
#define HELPERS_H

#include <Arduino.h>
#include "Config.h"

// Function declarations
float interpolate(float x, float x0, float y0, float x1, float y1);
void setupTime();
String getCurrentDateTime();

// Constants
extern const char* NTP_SERVER;
extern const int DAYLIGHT_OFFSET_S;

#endif // HELPERS_H