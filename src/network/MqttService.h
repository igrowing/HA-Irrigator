#pragma once

#include <Arduino.h>

#include "../domain/IrrigationTypes.h"

namespace irrigator::network {

struct MqttConfig {
    String broker;
    String user;
    String password;
    String prefix;
};

class MqttService {
public:
    void begin(const MqttConfig& config);
    void loop();
    bool connected() const;
    void publishAvailability(bool online);
    void publishStatus();
    void publishState(uint8_t zone,
                      const char* name,
                      bool on,
                      bool pump,
                      ::irrigator::domain::RunReason reason);
    void publishAlert(const char* payload);
    const String& prefix() const { return config_.prefix; }

private:
    MqttConfig config_;
};

}  // namespace irrigator::network