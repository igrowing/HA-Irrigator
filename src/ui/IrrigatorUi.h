#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <functional>
#include <time.h>

#include "../domain/IrrigationStateMachine.h"
#include "../domain/Schedule.h"
#include "../domain/Settings.h"
#include "../domain/UiStateMachine.h"
#include "../domain/WifiQuality.h"
#include "../drivers/RotaryInput.h"

namespace irrigator::ui {

// Everything IrrigatorUi needs to render a frame, gathered by the caller (main.ino, the
// composition root) each poll() call — keeps this class from needing direct references to
// every other subsystem.
struct RenderData {
    ::irrigator::domain::IrrigationSnapshot snapshot;
    const ::irrigator::domain::Settings* settings = nullptr;
    const ::irrigator::domain::ScheduleList* schedule = nullptr;
    bool scheduleActive = false;
    IPAddress deviceIp;
    String mqttBrokerText;
    String mqttTopicText;
    bool hasWallClock = false;
    struct tm wallClockLocal {};
    uint8_t nextScheduledZone = 0;  // 0 = none upcoming
    char nextScheduledZoneName[16] = "";
    // Weather auto-adjustment's last successful update, pre-formatted "DD-MM HH:MM" in local wall
    // time by the caller (main.ino, from lastWeatherUpdateEpoch via Clock::localTimeAt) - in-RAM
    // only, not part of Settings, so this resets to "" on every reboot. Not the same string as
    // main.ino's lastWeatherUpdateDate global (that one stays "YYYY-MM-DD" — it's a day-comparison
    // key for the once-per-day gating logic, not a display value).
    const char* lastWeatherUpdateDisplay = "";
    // Sampled at most every 5s by the caller (see main.ino's cachedWifiRssi), not read live —
    // RSSI jitters several dB reading-to-reading, so a live read would just flicker the display.
    int32_t wifiRssi = -127;
};

class IrrigatorUi {
public:
    void begin();
    void showCaptivePortal(const String& apSsid, const IPAddress& ip);

    // Enters "Active interaction" (screen 1, 20s timeout armed) — call once when entering
    // operational mode so the display starts on rather than defaulting to display-off.
    void wake(uint32_t nowMs);

    // Forces the factory-reset Yes/No confirm screen on, overriding whatever was showing —
    // called from the dedicated 10s-hold hardware trigger, independent of rotary navigation.
    void confirmFactoryReset(uint32_t nowMs);

    // Shows the Success/Failure outcome and redraws immediately (the caller is about to block
    // and restart, so there's no further poll() cycle left to pick the change up naturally).
    void showFactoryResetResult(bool success, uint32_t nowMs);

    // Polls the rotary input, advances the UI state machine, renders the current screen
    // (throttled — see kRenderIntervalMs), and returns whatever intent the state machine
    // produced this call (UiIntentType::None if nothing actionable happened).
    ::irrigator::domain::UiIntent poll(uint32_t nowMs, const RenderData& data);

private:
    using MenuLabelBuilder = std::function<void(uint8_t index, char* buffer, size_t bufferSize)>;

    void render(uint32_t nowMs, const RenderData& data);
    void drawTopStatusLine(const RenderData& data);
    void renderNormal(uint32_t nowMs, const RenderData& data);
    void renderZonePicker(const RenderData& data);
    void renderDurationPicker(const RenderData& data);
    void renderStopConfirm(const RenderData& data);
    void renderMenuMain(const RenderData& data);
    void renderMenuSchedule(const RenderData& data);
    void renderMenuSettings(const RenderData& data);
    void renderMenuTestAll();
    void renderRestartConfirm();
    void renderFactoryResetConfirm();
    void renderFactoryResetResult();
    void drawMenuList(const char* title, uint8_t itemCount, uint8_t selectedIndex,
                      const MenuLabelBuilder& labelBuilder);

    const char* zoneLabel(const RenderData& data, uint8_t zone) const;
    static const char* dayOfWeekName(uint8_t isoDay);

    // NONAME0 vs NONAME2 is an init-sequence/inversion difference between SSD1309 module
    // batches — if the real panel shows garbage or inverted colors, swap this for the
    // NONAME2 variant. Needs hardware-in-the-loop verification (agents.md Step 5).
    U8G2_SSD1309_128X64_NONAME0_F_HW_I2C display_{U8G2_R0, U8X8_PIN_NONE};

    ::irrigator::drivers::RotaryInput input_;
    ::irrigator::domain::UiStateMachine uiState_;

    ::irrigator::domain::UiScreen lastRenderedScreen_ = ::irrigator::domain::UiScreen::DisplayOff;
    uint32_t lastRenderMs_ = 0;
    static constexpr uint32_t kRenderIntervalMs = 250;
    mutable char zoneLabelBuf_[24];
};

}  // namespace irrigator::ui
