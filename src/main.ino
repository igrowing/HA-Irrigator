#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <Wire.h>

#include "board_config.h"
#include "domain/CurrentFaultMonitor.h"
#include "domain/IrrigationStateMachine.h"
#include "domain/RainSkip.h"
#include "domain/Settings.h"
#include "domain/WateringQueue.h"
#include "drivers/Clock.h"
#include "drivers/CurrentSensor.h"
#include "drivers/FileStore.h"
#include "drivers/LedIndicator.h"
#include "drivers/Logger.h"
#include "drivers/RelayBank.h"
#include "network/CaptivePortalService.h"
#include "network/MqttService.h"
#include "network/OtaService.h"
#include "ui/IrrigatorUi.h"

using irrigator::board::PUMP_CURRENT_ADC_PIN;
using irrigator::board::STATUS_LED_PIN;
using irrigator::board::VALVE_CURRENT_ADC_PIN;

namespace {

enum class AppMode { Booting, CaptivePortal, Operational };

AppMode appMode = AppMode::Booting;

irrigator::drivers::FileStore fileStore;
irrigator::drivers::RelayBank relayBank;
irrigator::drivers::CurrentSensor valveCurrentSensor(VALVE_CURRENT_ADC_PIN);
irrigator::drivers::CurrentSensor pumpCurrentSensor(PUMP_CURRENT_ADC_PIN);
irrigator::drivers::LedIndicator led(STATUS_LED_PIN);
irrigator::drivers::Clock clockService;

irrigator::domain::Settings settings;
irrigator::domain::WateringQueue<irrigator::board::WATERING_QUEUE_DEPTH> wateringQueue;
irrigator::domain::RainSkip rainSkip;
irrigator::domain::CurrentFaultMonitor valveFaultMonitor({irrigator::board::VALVE_CURRENT_MIN_MA,
                                                          irrigator::board::VALVE_CURRENT_MAX_MA});
irrigator::domain::CurrentFaultMonitor pumpFaultMonitor({irrigator::board::PUMP_CURRENT_MIN_MA,
                                                         irrigator::board::PUMP_CURRENT_MAX_MA});
irrigator::domain::IrrigationStateMachine irrigation;

irrigator::network::CaptivePortalService captivePortal;
irrigator::network::MqttService mqtt;
irrigator::network::OtaService ota;
irrigator::ui::IrrigatorUi ui;

String makeDevicePrefix() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    if (mac.length() < 2) {
        return "irrigator00";
    }
    return "irrigator" + mac.substring(mac.length() - 2);
}

void enterCaptivePortalMode() {
    appMode = AppMode::CaptivePortal;
    const String apSsid = makeDevicePrefix() + "_SETUP";
    captivePortal.begin(apSsid);
    led.setMode(irrigator::drivers::LedMode::CaptivePortal);
    ui.showCaptivePortal(apSsid, IPAddress(192, 168, 4, 1));
}

void enterOperationalMode() {
    appMode = AppMode::Operational;
    settings = irrigator::domain::makeDefaultSettings();
    led.setMode(irrigator::drivers::LedMode::Off);
    clockService.begin("CET-1CEST,M3.5.0,M10.5.0/3");

    irrigator::network::MqttConfig mqttConfig;
    mqttConfig.prefix = makeDevicePrefix();
    mqtt.begin(mqttConfig);
    ui.showNormal(irrigation.snapshot(millis()));
}

void updateOutputsFromDomain() {
    const auto snapshot = irrigation.snapshot(millis());
    relayBank.setPump(snapshot.pumpOn);

    for (uint8_t zone = 1; zone <= irrigator::board::ZONE_COUNT; ++zone) {
        const bool zoneOn = snapshot.state == irrigator::domain::IrrigationState::Watering &&
                            snapshot.activeZone == zone;
        relayBank.setZone(zone, zoneOn);
    }

    if (snapshot.pumpOn) {
        led.setMode(irrigator::drivers::LedMode::SolidOn);
    } else if (appMode == AppMode::Operational) {
        led.setMode(irrigator::drivers::LedMode::Off);
    }
}

}  // namespace

void setup() {
    irrigator::drivers::Log.begin(115200);
    irrigator::drivers::Log.info(F("Irrigator firmware starting"));

    Wire.begin(irrigator::board::I2C_SDA_PIN, irrigator::board::I2C_SCL_PIN);
    Wire.setClock(irrigator::board::I2C_CLOCK_HZ);

    analogReadResolution(irrigator::board::ADC_RESOLUTION_BITS);
    analogSetAttenuation(ADC_11db);

    fileStore.begin();
    relayBank.begin();
    valveCurrentSensor.begin();
    pumpCurrentSensor.begin();
    led.begin();
    ui.begin();

    if (!fileStore.exists("/config.json")) {
        enterCaptivePortalMode();
        return;
    }

    enterOperationalMode();
}

void loop() {
    const uint32_t nowMs = millis();
    led.update(nowMs);

    if (ota.inProgress()) {
        led.setMode(irrigator::drivers::LedMode::Ota);
        return;
    }

    switch (appMode) {
        case AppMode::CaptivePortal:
            captivePortal.loop();
            break;
        case AppMode::Operational:
            mqtt.loop();
            irrigation.tick(nowMs);
            updateOutputsFromDomain();
            break;
        case AppMode::Booting:
        default:
            break;
    }
}