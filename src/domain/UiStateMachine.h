#pragma once

#include <stdint.h>

#include "UiTypes.h"
#include "board_config.h"

namespace irrigator::domain {

// Pure navigation/interaction state machine for the rotary-encoder UI. Deliberately knows
// nothing about Settings/ScheduleList/IrrigationStateMachine directly (see agents.md's driver-
// interface-only rule for domain/) — the caller (src/ui/IrrigatorUi, which owns the live data)
// supplies whatever the currently-selected item needs via Context each call, and executes the
// UiIntent this returns. That keeps this class fully host-testable.
//
// Every menu row is clickable: it either opens a submenu or enters edit mode for that item
// (only IP/MQTT IP are spec'd "not clickable"). Zone/day-of-week cycle by 1 per tick; start-time
// is edited as two sequential steps (hour, then minute) under the same click-to-edit gesture.
class UiStateMachine {
public:
    struct Context {
        // Defaults every zone to enabled so callers/tests that never touch this (most of them —
        // it's only load-bearing for the manual zone picker) see the pre-existing "all zones
        // reachable" behavior without having to fill it in themselves.
        Context() {
            for (bool& enabled : zoneEnabled) {
                enabled = true;
            }
        }

        bool wateringActive = false;
        bool scheduleActive = false;
        bool autoAdjustmentEnabled = false;
        uint8_t scheduleRecordCount = 0;
        int16_t wateringPercentage = 0;
        int16_t upperLimitPercent = 0;
        int16_t lowerLimitPercent = 0;
        int16_t rainSkipProbabilityPercent = 0;
        int16_t rainSkipAmountMm = 0;
        uint8_t defaultManualDuration = 0;
        // Disabled zones are skipped by the manual zone picker (handleNormal/handleZonePicker) —
        // see isZoneEnabled() in main.ino for the matching enforcement at the point of starting a
        // run (schedule/manual/MQTT alike). Index 0 = zone 1.
        bool zoneEnabled[board::ZONE_COUNT];
        // Zone the Normal screen is currently showing as the upcoming pick when idle+scheduled
        // (0 = none/PUMP) — see IrrigatorUi::renderNormal's `scheduled` branch, which is what
        // this mirrors. handleNormal starts CW/CCW from here instead of always from PUMP, so a
        // rotation moves relative to what's on screen rather than jumping back to PUMP first.
        uint8_t nextScheduledZone = 0;
        // Only need to be valid when a MenuSchedule row is selected; the caller refreshes these
        // from the live schedule record at selectedIndex()/kScheduleFieldsPerRecord before
        // calling handleInput().
        uint8_t selectedScheduleZone = 0;
        uint8_t selectedScheduleDay = 0;
        uint8_t selectedScheduleHour = 0;
        uint8_t selectedScheduleMinute = 0;
        uint16_t selectedScheduleDurationMin = 0;
        bool selectedScheduleActive = false;
    };

    static constexpr uint8_t kScheduleFieldsPerRecord = 5;
    static constexpr uint8_t kScheduleFieldZone = 0;
    static constexpr uint8_t kScheduleFieldDay = 1;
    static constexpr uint8_t kScheduleFieldTime = 2;
    static constexpr uint8_t kScheduleFieldDuration = 3;
    static constexpr uint8_t kScheduleFieldActive = 4;

    // MenuSchedule's item count: `recordCount` rows of kScheduleFieldsPerRecord fields each, plus
    // a trailing BACK row. Shared so the renderer (IrrigatorUi::renderMenuSchedule) and the click
    // routing below (handleMenuSchedule) can't compute it two different ways.
    static constexpr uint8_t scheduleItemCount(uint8_t recordCount) {
        return static_cast<uint8_t>(recordCount * kScheduleFieldsPerRecord + 1);
    }

    // MenuMain's item order/count depends on whether a run is active — [Stop, Stop all] only
    // appear while watering, shifting every index after them by 2. One source for both the
    // renderer's row order and handleMenuMain's click routing, instead of each independently
    // recomputing the same offsets (see agents.md's layer-separation rule for why this lives here
    // rather than in IrrigatorUi: it's the same navigation-bounds concern kScheduleFieldsPerRecord
    // already covers for MenuSchedule, and keeping it in the host-testable domain layer means it's
    // covered by the native test suite instead of only by hardware smoke-testing).
    struct MenuMainLayout {
        uint8_t itemCount;
        uint8_t scheduleIndex;
        uint8_t testAllIndex;
        uint8_t settingsIndex;
        uint8_t infoIndex;
        uint8_t backIndex;
    };
    static MenuMainLayout menuMainLayout(bool wateringActive);

    // MenuTestAll: 3 rows — Run, Dur./zone, BACK.
    static constexpr uint8_t kTestAllItemCount = 3;
    static constexpr uint8_t kTestAllIndexRun = 0;
    static constexpr uint8_t kTestAllIndexDuration = 1;
    static constexpr uint8_t kTestAllIndexBack = 2;

    // MenuSettings: 9 rows, all the settings that can actually change. All rows are clickable
    // except BACK (see spec-comment on kInfoItemCount below for the read-only counterparts, split
    // out into their own MenuInfo screen) but every row still gets a named index here so the
    // renderer's switch and this class's click routing share one definition of the layout rather
    // than two hand-kept-in-sync copies (this is exactly how the WiFi row's insertion previously
    // broke BACK: the renderer's item count moved to 8 without this class's matching update).
    // Auto-adjustment is deliberately first: it's the master on/off for the whole weather feature,
    // so it reads before any of the fields it affects (Adjust/Upper/Lower/rain-skip thresholds).
    static constexpr uint8_t kSettingsItemCount = 9;
    static constexpr uint8_t kSettingsIndexAutoAdjustment = 0;
    static constexpr uint8_t kSettingsIndexPercentage = 1;
    static constexpr uint8_t kSettingsIndexUpperLimit = 2;
    static constexpr uint8_t kSettingsIndexLowerLimit = 3;
    static constexpr uint8_t kSettingsIndexRainSkipProbability = 4;
    static constexpr uint8_t kSettingsIndexRainSkipAmount = 5;
    static constexpr uint8_t kSettingsIndexScheduleActive = 6;
    static constexpr uint8_t kSettingsIndexRestart = 7;
    static constexpr uint8_t kSettingsIndexBack = 8;

    // MenuInfo: 9 rows, the read-only counterparts split out of MenuSettings (WiFi/IP/MQTT/Topic/
    // City/Coordinates/LastUpdate/FW are all display-only, per spec) plus BACK. None of the
    // display-only rows are clickable — only BACK is.
    static constexpr uint8_t kInfoItemCount = 9;
    static constexpr uint8_t kInfoIndexWifi = 0;
    static constexpr uint8_t kInfoIndexIp = 1;
    static constexpr uint8_t kInfoIndexMqtt = 2;
    static constexpr uint8_t kInfoIndexTopic = 3;
    static constexpr uint8_t kInfoIndexCity = 4;
    static constexpr uint8_t kInfoIndexCoordinates = 5;
    static constexpr uint8_t kInfoIndexLastUpdate = 6;
    static constexpr uint8_t kInfoIndexFirmware = 7;
    static constexpr uint8_t kInfoIndexBack = 8;

    UiIntent handleInput(UiInputEvent event, const Context& ctx, uint32_t nowMs);

    // Call every loop() iteration regardless of input, to catch the 20s active-interaction
    // timeout expiring with no further ticks/clicks.
    void tick(uint32_t nowMs);

    // Enters "Active interaction" showing screen 1, with the 20s timeout armed — call once when
    // entering operational mode (boot starts active per spec, not display-off; display-off is
    // only reached via the timeout or explicitly turning the display off first).
    void wake(uint32_t nowMs);

    // Forces the factory-reset Yes/No confirm screen, overriding whatever screen was showing
    // (any unsaved edit is abandoned, same as a display-off timeout) — called from a dedicated
    // hardware trigger (10s hold on the BOOT button), independent of normal rotary navigation.
    void enterFactoryResetConfirm(uint32_t nowMs);

    // Shows the Success/Failure outcome after the caller has actually performed the reset.
    void showFactoryResetResult(bool success, uint32_t nowMs);

    UiScreen screen() const { return screen_; }
    uint8_t selectedIndex() const { return selectedIndex_; }
    bool editing() const { return editing_; }
    uint8_t pendingZone() const { return pendingZone_; }
    uint16_t pendingDuration() const { return pendingDuration_; }
    bool stopConfirmYesSelected() const { return stopConfirmYes_; }
    // Which action StopConfirm is currently confirming — set when entering the screen from
    // MenuMain's "Stop" (false) vs "Stop all" (true) item; distinguishes the two on Yes.
    bool stopAllPending() const { return stopAllPending_; }
    int16_t pendingPercentage() const { return pendingPercentage_; }
    int16_t pendingUpperLimit() const { return pendingUpperLimit_; }
    int16_t pendingLowerLimit() const { return pendingLowerLimit_; }
    int16_t pendingRainSkipProbability() const { return pendingRainSkipProbability_; }
    int16_t pendingRainSkipAmount() const { return pendingRainSkipAmount_; }
    bool factoryResetConfirmYesSelected() const { return factoryResetConfirmYes_; }
    bool factoryResetSucceeded() const { return factoryResetSucceeded_; }
    bool restartConfirmYesSelected() const { return restartConfirmYes_; }

    uint8_t pendingScheduleZone() const { return pendingScheduleZone_; }
    uint8_t pendingScheduleDay() const { return pendingScheduleDay_; }
    uint8_t pendingScheduleHour() const { return pendingScheduleHour_; }
    uint8_t pendingScheduleMinute() const { return pendingScheduleMinute_; }
    bool editingScheduleTimeMinuteStep() const { return editingScheduleMinuteStep_; }

    // Per-zone duration used by "Test all zones" (MenuTestAll) — session-only, not persisted;
    // resets to kDefaultTestAllZonesDurationMin on boot, like the rest of UiStateMachine's state.
    uint16_t testAllZonesDurationMin() const { return testAllZonesDurationMin_; }

private:
    void enterDisplayOff();
    void armTimeout(uint32_t nowMs);

    UiIntent handleNormal(UiInputEvent event, const Context& ctx);
    UiIntent handleZonePicker(UiInputEvent event, const Context& ctx);
    UiIntent handleDurationPicker(UiInputEvent event);
    UiIntent handleStopConfirm(UiInputEvent event);
    UiIntent handleMenuMain(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuSchedule(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuScheduleEdit(UiInputEvent event, uint8_t fieldIndex, uint8_t recordIndex);
    UiIntent handleMenuSettings(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuInfo(UiInputEvent event);
    UiIntent handleMenuTestAll(UiInputEvent event);
    UiIntent handleRestartConfirm(UiInputEvent event);
    UiIntent handleFactoryResetConfirm(UiInputEvent event);

    static uint8_t stepIndex(uint8_t current, uint8_t count, bool forward);

    // Steps from `current` (0 = PUMP, 1..ZONE_COUNT = zones) to the next reachable zone in the
    // given direction, skipping any disabled zone. PUMP is always reachable, so this always
    // terminates — worst case (every zone disabled) it wraps all the way back to PUMP.
    static uint8_t nextZone(uint8_t current, bool forward, const Context& ctx);

    static constexpr uint16_t kDefaultTestAllZonesDurationMin = 5;

    UiScreen screen_ = UiScreen::DisplayOff;
    uint8_t selectedIndex_ = 0;
    bool editing_ = false;

    uint8_t pendingZone_ = 0;
    uint16_t pendingDuration_ = 0;
    bool stopConfirmYes_ = false;
    bool stopAllPending_ = false;
    int16_t pendingPercentage_ = 0;
    int16_t pendingUpperLimit_ = 0;
    int16_t pendingLowerLimit_ = 0;
    int16_t pendingRainSkipProbability_ = 0;
    int16_t pendingRainSkipAmount_ = 0;

    uint8_t pendingScheduleZone_ = 0;
    uint8_t pendingScheduleDay_ = 0;
    uint8_t pendingScheduleHour_ = 0;
    uint8_t pendingScheduleMinute_ = 0;
    bool editingScheduleMinuteStep_ = false;

    uint16_t testAllZonesDurationMin_ = kDefaultTestAllZonesDurationMin;

    bool factoryResetConfirmYes_ = false;
    bool factoryResetSucceeded_ = false;
    bool restartConfirmYes_ = false;

    bool active_ = false;  // Active-interaction vs. display-off.
    uint32_t interactionDeadlineMs_ = 0;
};

}  // namespace irrigator::domain
