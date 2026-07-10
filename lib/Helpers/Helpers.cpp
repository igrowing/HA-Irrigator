#include "Helpers.h"

// Helper function for linear interpolation
float interpolate(float x, float x0, float y0, float x1, float y1) {
    if (x1 == x0) return y0; // Avoid division by zero
    return y0 + (x - x0) * (y1 - y0) / (x1 - x0);
}


// --- Time Synchronization ---
const char* NTP_SERVER = "pool.ntp.org";
const int DAYLIGHT_OFFSET_S = 0; 

void setupTime() {
    long gmt_s = config.timeshift_hours * 3600;
    // TODO: Get proper DAYLIGHT_OFFSET_S
    configTime(gmt_s, DAYLIGHT_OFFSET_S, NTP_SERVER);
    Serial.printf("NTP time sync initialized with UTC offset: %ld seconds\n", gmt_s);
}


String getCurrentDateTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "Time not set";
    }
    char timeStringBuff[20]; // 20 characters for "YYYY-MM-DD HH:MM:SS"
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timeStringBuff);
}