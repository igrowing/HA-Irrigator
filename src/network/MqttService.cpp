#include "MqttService.h"

#include "../drivers/Logger.h"

namespace irrigator::network {

void MqttService::begin(const MqttConfig& config) {
    config_ = config;
    ::irrigator::drivers::Log.printf("[INFO] MQTT service configured for prefix %s\n",
                                     config_.prefix.c_str());
}

void MqttService::loop() {
    // TODO: move PubSubClient setup/callbacks from legacy lib/MQTT into this service.
}

bool MqttService::connected() const {
    return false;
}

void MqttService::publishAvailability(bool online) {
    ::irrigator::drivers::Log.printf("[INFO] MQTT availability: %s\n",
                                     online ? "online" : "offline");
}

void MqttService::publishStatus() {
    ::irrigator::drivers::Log.info("MQTT status publish requested");
}

void MqttService::publishState(uint8_t zone,
                               const char* name,
                               bool on,
                               bool pump,
                               ::irrigator::domain::RunReason reason) {
    ::irrigator::drivers::Log.printf("[INFO] State zone=%u name=%s on=%d pump=%d reason=%s\n",
                                     static_cast<unsigned>(zone),
                                     name == nullptr ? "" : name,
                                     on,
                                     pump,
                                     ::irrigator::domain::toString(reason));
}

void MqttService::publishAlert(const char* payload) {
    ::irrigator::drivers::Log.printf("[WARN] MQTT alert: %s\n", payload == nullptr ? "" : payload);
}

}  // namespace irrigator::network