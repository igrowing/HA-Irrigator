#include "OtaService.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>

#include "../drivers/Logger.h"

namespace irrigator::network {

void OtaService::reportResult(bool success, const char* message) {
    if (success) {
        ::irrigator::drivers::Log.printf("[INFO] OTA: %s\n", message);
    } else {
        ::irrigator::drivers::Log.printf("[ERROR] OTA: %s\n", message);
    }
    if (resultHandler_) {
        resultHandler_(success, message);
    }
}

void OtaService::beginUpdateFromUrl(const char* url) {
    if (inProgress_) {
        reportResult(false, "Update already in progress; ignoring new request");
        return;
    }
    if (url == nullptr || url[0] == '\0') {
        reportResult(false, "Firmware URL is empty");
        return;
    }

    inProgress_ = true;

    HTTPClient http;
    if (!http.begin(url)) {
        reportResult(false, "Could not begin HTTP request");
        inProgress_ = false;
        return;
    }

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        char message[64];
        snprintf(message, sizeof(message), "HTTP GET failed, code %d", httpCode);
        reportResult(false, message);
        http.end();
        inProgress_ = false;
        return;
    }

    const int contentLength = http.getSize();
    if (contentLength <= 0) {
        reportResult(false, "Content-Length missing or zero");
        http.end();
        inProgress_ = false;
        return;
    }

    if (!Update.begin(static_cast<size_t>(contentLength))) {
        char message[96];
        snprintf(message, sizeof(message), "Not enough space for update: %s", Update.errorString());
        reportResult(false, message);
        http.end();
        inProgress_ = false;
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    const size_t written = Update.writeStream(*stream);
    if (written != static_cast<size_t>(contentLength)) {
        char message[80];
        snprintf(message,
                sizeof(message),
                "Wrote %u of %d bytes",
                static_cast<unsigned>(written),
                contentLength);
        reportResult(false, message);
        Update.abort();
        http.end();
        inProgress_ = false;
        return;
    }

    if (!Update.end() || !Update.isFinished()) {
        char message[96];
        snprintf(
            message, sizeof(message), "Update did not finish cleanly: %s", Update.errorString());
        reportResult(false, message);
        http.end();
        inProgress_ = false;
        return;
    }

    http.end();
    reportResult(true, "Update successful, restarting");
    delay(200);
    ESP.restart();
}

}  // namespace irrigator::network
