#pragma once

#include <Arduino.h>

namespace irrigator::network {

class CaptivePortalService {
public:
    void begin(const String& apSsid);
    void loop();
    bool active() const { return active_; }

private:
    bool active_ = false;
};

}  // namespace irrigator::network