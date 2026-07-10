#include "CaptivePortalService.h"

#include "../drivers/Logger.h"

namespace irrigator::network {

void CaptivePortalService::begin(const String& apSsid) {
    active_ = true;
    ::irrigator::drivers::Log.printf("[INFO] Captive portal skeleton started: %s\n",
                                     apSsid.c_str());
    // TODO: migrate legacy lib/CaptivePortal routes and one-shot WiFi+MQTT validation.
}

void CaptivePortalService::loop() {
    // TODO: dnsServer.processNextRequest(); server.handleClient();
}

}  // namespace irrigator::network