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
        // Current_sensor group's alert toggles (settings.json) — see kSettingsIndexAlertPumpUc
        // etc. below for the Settings-menu rows that read/flip these.
        bool alertPumpUc = false;
        bool alertPumpOc = false;
        bool alertValveUc = false;
        bool alertValveOc = false;
        uint8_t scheduleRecordCount = 0;
        // Rows waiting behind the currently-active run — see WateringQueue::size() in main.ino.
        // Only meaningful while wateringActive, but harmless to read otherwise (menuQueueItemCount
        // just adds 1 for BACK either way). Doesn't include the active run itself: that's shown by
        // the Normal screen (activeZone) already, not by MenuQueue.
        uint8_t queueSize = 0;
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
        // Zone of the run that's currently active (0 = a pump-only run), only meaningful when
        // wateringActive. Same purpose as nextScheduledZone above, for the other branch of
        // IrrigatorUi::renderNormal: while watering, the big line on screen is the *running*
        // zone, so that — not the next scheduled one, and not PUMP — is what handleNormal's
        // rotation and click have to move relative to.
        uint8_t activeZone = 0;
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
    // uint16_t (not uint8_t): at SCHEDULE_MAX_RECORDS=255 this reaches 255*5+1=1276, which would
    // silently wrap in 8-bit arithmetic starting around 52 records.
    static constexpr uint16_t scheduleItemCount(uint8_t recordCount) {
        return static_cast<uint16_t>(static_cast<uint16_t>(recordCount) * kScheduleFieldsPerRecord + 1);
    }

    // MenuScheduleRoot: 4 rows — View/Edit, Add, Delete, BACK. Entered from MenuMain's Schedule
    // row; View/Edit pushes into the existing MenuSchedule list/edit screen one level down.
    static constexpr uint8_t kMenuScheduleRootItemCount = 4;
    static constexpr uint8_t kMenuScheduleRootIndexViewEdit = 0;
    static constexpr uint8_t kMenuScheduleRootIndexAdd = 1;
    static constexpr uint8_t kMenuScheduleRootIndexDelete = 2;
    static constexpr uint8_t kMenuScheduleRootIndexBack = 3;

    // ScheduleAdd: 6 rows — Zone, Day, Start time, Duration, Save, BACK. Unlike MenuSchedule's
    // per-field immediate commit, fields here only stage into the pending* buffers; Save is what
    // bundles them into a single AddScheduleRecord intent.
    static constexpr uint8_t kScheduleAddItemCount = 6;
    static constexpr uint8_t kScheduleAddIndexZone = 0;
    static constexpr uint8_t kScheduleAddIndexDay = 1;
    static constexpr uint8_t kScheduleAddIndexTime = 2;
    static constexpr uint8_t kScheduleAddIndexDuration = 3;
    static constexpr uint8_t kScheduleAddIndexSave = 4;
    static constexpr uint8_t kScheduleAddIndexBack = 5;

    // MenuMain's item order/count depends on whether a run is active — [Queue, Stop, Stop all]
    // only appear while watering, shifting every index after them by 3. One source for both the
    // renderer's row order and handleMenuMain's click routing, instead of each independently
    // recomputing the same offsets (see agents.md's layer-separation rule for why this lives here
    // rather than in IrrigatorUi: it's the same navigation-bounds concern kScheduleFieldsPerRecord
    // already covers for MenuSchedule, and keeping it in the host-testable domain layer means it's
    // covered by the native test suite instead of only by hardware smoke-testing).
    struct MenuMainLayout {
        uint8_t itemCount;
        uint8_t queueIndex;    // only valid while watering
        uint8_t stopIndex;     // only valid while watering
        uint8_t stopAllIndex;  // only valid while watering
        uint8_t scheduleIndex;
        uint8_t testAllIndex;
        uint8_t settingsIndex;
        uint8_t infoIndex;
        uint8_t backIndex;
    };
    static MenuMainLayout menuMainLayout(bool wateringActive);

    // MenuQueue: the rows waiting behind the currently-active run, plus a trailing BACK row — same
    // "N items + 1" shape as scheduleItemCount() above. With an empty queue this is 1 (just BACK),
    // which is exactly the "nothing queued" display the spec calls for.
    static constexpr uint16_t queueMenuItemCount(uint8_t queueSize) {
        return static_cast<uint16_t>(static_cast<uint16_t>(queueSize) + 1);
    }

    // MenuTestAll: 3 rows — Run, Dur./zone, BACK.
    static constexpr uint8_t kTestAllItemCount = 3;
    static constexpr uint8_t kTestAllIndexRun = 0;
    static constexpr uint8_t kTestAllIndexDuration = 1;
    static constexpr uint8_t kTestAllIndexBack = 2;

    // MenuSettings: 14 rows, all the settings that can actually change. All rows are clickable
    // except BACK (see spec-comment on kInfoItemCount below for the read-only counterparts, split
    // out into their own MenuInfo screen) but every row still gets a named index here so the
    // renderer's switch and this class's click routing share one definition of the layout rather
    // than two hand-kept-in-sync copies (this is exactly how the WiFi row's insertion previously
    // broke BACK: the renderer's item count moved to 8 without this class's matching update).
    // Schedule is deliberately first: weather-based auto-adjustment only makes sense when the
    // schedule itself is active — with scheduling off, watering is manual/on-demand, and there's
    // nothing for the daily Adjust% recalculation to apply to. Auto-adjustment reads second, still
    // before the fields it affects when enabled (Adjust/Upper/Lower/rain-skip thresholds); with it
    // off, Adjust% is a plain manual value instead. The 4 alert toggles sit directly above
    // Calibrate CS (per spec) since they're only meaningful once a sensor is calibrated.
    static constexpr uint8_t kSettingsItemCount = 14;
    static constexpr uint8_t kSettingsIndexScheduleActive = 0;
    static constexpr uint8_t kSettingsIndexAutoAdjustment = 1;
    static constexpr uint8_t kSettingsIndexPercentage = 2;
    static constexpr uint8_t kSettingsIndexUpperLimit = 3;
    static constexpr uint8_t kSettingsIndexLowerLimit = 4;
    static constexpr uint8_t kSettingsIndexRainSkipProbability = 5;
    static constexpr uint8_t kSettingsIndexRainSkipAmount = 6;
    static constexpr uint8_t kSettingsIndexAlertPumpUc = 7;
    static constexpr uint8_t kSettingsIndexAlertPumpOc = 8;
    static constexpr uint8_t kSettingsIndexAlertValveUc = 9;
    static constexpr uint8_t kSettingsIndexAlertValveOc = 10;
    static constexpr uint8_t kSettingsIndexCalibrate = 11;
    static constexpr uint8_t kSettingsIndexRestart = 12;
    static constexpr uint8_t kSettingsIndexBack = 13;

    // MenuInfo: 10 rows, the read-only counterparts split out of MenuSettings (WiFi/IP/MQTT/Topic/
    // City/Coordinates/LastUpdate/FW/Uptime are all display-only, per spec) plus BACK. None of the
    // display-only rows are clickable — only BACK is.
    static constexpr uint8_t kInfoItemCount = 10;
    static constexpr uint8_t kInfoIndexWifi = 0;
    static constexpr uint8_t kInfoIndexIp = 1;
    static constexpr uint8_t kInfoIndexMqtt = 2;
    static constexpr uint8_t kInfoIndexTopic = 3;
    static constexpr uint8_t kInfoIndexCity = 4;
    static constexpr uint8_t kInfoIndexCoordinates = 5;
    static constexpr uint8_t kInfoIndexLastUpdate = 6;
    static constexpr uint8_t kInfoIndexFirmware = 7;
    static constexpr uint8_t kInfoIndexUptime = 8;
    static constexpr uint8_t kInfoIndexBack = 9;

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

    // Shows the outcome of a current-sensor calibration run (see main.ino's
    // handleCalibrateCommand, the same logic MQTT cmd/calibrate uses) after the caller has
    // actually performed it. Unlike showFactoryResetResult (whose caller immediately forces a
    // restart, so there's no further poll() left to redraw), this screen just sits until the user
    // clicks OK (back to Normal, see handleCalibrateResult) or the regular 20s inactivity timeout
    // blanks the display — nothing auto-dismisses it. valve/pumpMessage are only meaningful when
    // the corresponding succeeded flag is false (a short reason: "no data", "too low", etc. — see
    // calibrateOneSensor's messageOut in main.ino); both false with the same message means the
    // whole run was rejected outright (e.g. valve not open) before either sensor was attempted.
    void showCalibrateResult(bool valveSucceeded,
                             uint16_t valveBaseLevel,
                             const char* valveMessage,
                             bool pumpSucceeded,
                             uint16_t pumpBaseLevel,
                             const char* pumpMessage,
                             uint32_t nowMs);

    // Shows the Success/Failure outcome of an on-device schedule Add/Delete after the caller
    // (main.ino) has actually performed it. The return screen/index were already recorded (in
    // scheduleResultReturnScreen_/Index_) by whichever click emitted the AddScheduleRecord/
    // DeleteScheduleRecord intent — this only forces the result screen on, same imperative-override
    // pattern as showCalibrateResult.
    void showScheduleActionResult(bool success, uint32_t nowMs);

    // Shows the outcome of adding a manual watering request to the queue while a run is already
    // active (rotary-picked zone/duration on the Normal screen, same as an immediate start, but
    // the request goes to the back of the queue instead) — main.ino calls this right after
    // actually pushing the request, same imperative-override pattern as showCalibrateResult/
    // showScheduleActionResult. zoneLabel is pre-formatted by the caller (mirrors
    // calibrateValveMessage/calibratePumpMessage below) since this class doesn't have access to
    // Settings' zone names. success is false when the queue was full and the request got dropped.
    void showQueueAddResult(const char* zoneLabel, uint16_t duration, bool success, uint32_t nowMs);

    UiScreen screen() const { return screen_; }
    // uint16_t: MenuSchedule's item count can exceed 255 (see scheduleItemCount() above).
    uint16_t selectedIndex() const { return selectedIndex_; }
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
    bool calibrateConfirmYesSelected() const { return calibrateConfirmYes_; }
    bool calibrateValveSucceeded() const { return calibrateValveSucceeded_; }
    uint16_t calibrateValveBaseLevel() const { return calibrateValveBaseLevel_; }
    const char* calibrateValveMessage() const { return calibrateValveMessage_; }
    bool calibratePumpSucceeded() const { return calibratePumpSucceeded_; }
    uint16_t calibratePumpBaseLevel() const { return calibratePumpBaseLevel_; }
    const char* calibratePumpMessage() const { return calibratePumpMessage_; }

    const char* queueAddZoneLabel() const { return queueAddZoneLabel_; }
    uint16_t queueAddDuration() const { return queueAddDuration_; }
    bool queueAddSucceeded() const { return queueAddSucceeded_; }

    uint8_t pendingScheduleZone() const { return pendingScheduleZone_; }
    uint8_t pendingScheduleDay() const { return pendingScheduleDay_; }
    uint8_t pendingScheduleHour() const { return pendingScheduleHour_; }
    uint8_t pendingScheduleMinute() const { return pendingScheduleMinute_; }
    bool editingScheduleTimeMinuteStep() const { return editingScheduleMinuteStep_; }

    bool scheduleDeleteConfirmYesSelected() const { return scheduleDeleteConfirmYes_; }
    uint8_t scheduleDeleteRecordIndex() const { return scheduleDeleteRecordIndex_; }
    bool scheduleActionSucceeded() const { return scheduleActionSucceeded_; }
    // Which screen/index Add-Save or Delete-Yes will restore the cursor to once the user
    // dismisses ScheduleActionResult with OK — see handleScheduleActionResult(). The renderer also
    // reads scheduleResultReturnScreen() to pick the result screen's "ADD"/"DELETE" title.
    UiScreen scheduleResultReturnScreen() const { return scheduleResultReturnScreen_; }

    // Per-zone duration used by "Test all zones" (MenuTestAll) — session-only, not persisted;
    // resets to kDefaultTestAllZonesDurationMin on boot, like the rest of UiStateMachine's state.
    uint16_t testAllZonesDurationMin() const { return testAllZonesDurationMin_; }

private:
    void enterDisplayOff();
    void armTimeout(uint32_t nowMs);

    UiIntent handleNormal(UiInputEvent event, const Context& ctx);
    UiIntent handleZonePicker(UiInputEvent event, const Context& ctx);
    UiIntent handleDurationPicker(UiInputEvent event, const Context& ctx);
    UiIntent handleStopConfirm(UiInputEvent event);
    UiIntent handleMenuMain(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuQueue(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuScheduleRoot(UiInputEvent event);
    UiIntent handleMenuSchedule(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuScheduleEdit(UiInputEvent event, uint8_t fieldIndex, uint8_t recordIndex);
    UiIntent handleScheduleAdd(UiInputEvent event);
    UiIntent handleScheduleAddEdit(UiInputEvent event, uint8_t fieldIndex);
    UiIntent handleScheduleDelete(UiInputEvent event, const Context& ctx);
    UiIntent handleScheduleDeleteConfirm(UiInputEvent event);
    UiIntent handleScheduleActionResult(UiInputEvent event);
    UiIntent handleMenuSettings(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuInfo(UiInputEvent event);
    UiIntent handleMenuTestAll(UiInputEvent event);
    // Shared by RestartConfirm/FactoryResetConfirm/CalibrateConfirm (see UiStateMachine.cpp's
    // definition for why they can share one implementation).
    UiIntent handleYesNoConfirm(UiInputEvent event, bool& yesSelected, UiIntentType confirmedIntentType);
    // Shared by CalibrateResult/QueueAddResult — dead-end "OK" screens that always dismiss to
    // Normal (see UiStateMachine.cpp's definition for why ScheduleActionResult can't join them).
    UiIntent handleDeadEndResult(UiInputEvent event);

    // uint16_t (not uint8_t): shared by every menu's CW/CCW wraparound, including MenuSchedule's
    // which can exceed 255 (see scheduleItemCount() above) — small menus pass small counts and
    // work identically at this width.
    static uint16_t stepIndex(uint16_t current, uint16_t count, bool forward);

    // Steps from `current` (0 = PUMP, 1..ZONE_COUNT = zones) to the next reachable zone in the
    // given direction, skipping any disabled zone. PUMP is always reachable, so this always
    // terminates — worst case (every zone disabled) it wraps all the way back to PUMP.
    static uint8_t nextZone(uint8_t current, bool forward, const Context& ctx);

    static constexpr uint16_t kDefaultTestAllZonesDurationMin = 5;

    UiScreen screen_ = UiScreen::DisplayOff;
    uint16_t selectedIndex_ = 0;
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

    bool scheduleDeleteConfirmYes_ = false;
    uint8_t scheduleDeleteRecordIndex_ = 0;
    UiScreen scheduleResultReturnScreen_ = UiScreen::MenuScheduleRoot;
    uint8_t scheduleResultReturnIndex_ = 0;
    bool scheduleActionSucceeded_ = false;

    uint16_t testAllZonesDurationMin_ = kDefaultTestAllZonesDurationMin;

    bool factoryResetConfirmYes_ = false;
    bool factoryResetSucceeded_ = false;
    bool restartConfirmYes_ = false;

    bool calibrateConfirmYes_ = false;
    bool calibrateValveSucceeded_ = false;
    uint16_t calibrateValveBaseLevel_ = 0;
    char calibrateValveMessage_[16] = {};
    bool calibratePumpSucceeded_ = false;
    uint16_t calibratePumpBaseLevel_ = 0;
    char calibratePumpMessage_[16] = {};

    char queueAddZoneLabel_[24] = {};
    uint16_t queueAddDuration_ = 0;
    bool queueAddSucceeded_ = false;

    bool active_ = false;  // Active-interaction vs. display-off.
    uint32_t interactionDeadlineMs_ = 0;
};

}  // namespace irrigator::domain
