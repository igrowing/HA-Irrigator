#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "../domain/ConfigStore.h"
#include "../drivers/FileStore.h"

namespace irrigator::network {

class CaptivePortalService {
public:
    void begin(const String& apSsid, ::irrigator::drivers::FileStore& fileStore);
    void loop();
    bool active() const { return active_; }

private:
    void handleRoot();
    void handleSave();
    void handleNotFound();
    String renderForm(const ::irrigator::domain::DeviceConfig& values, const char* errorMessage);
    bool parseFormConfig(::irrigator::domain::DeviceConfig& out);
    bool testMqttOnce(const ::irrigator::domain::DeviceConfig& config);
    void failAndStayInApMode(const ::irrigator::domain::DeviceConfig& formValues,
                             const char* errorMessage);

    bool active_ = false;
    String apSsid_;
    ::irrigator::drivers::FileStore* fileStore_ = nullptr;
    WebServer server_{80};
    DNSServer dnsServer_;
};

}  // namespace irrigator::network
