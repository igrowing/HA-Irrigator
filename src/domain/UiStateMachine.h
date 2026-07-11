#pragma once

#include <stdint.h>

#include "UiTypes.h"

namespace irrigator::domain {

// Pure navigation/interaction state machine for the rotary-encoder UI. Deliberately knows
// nothing about Settings/ScheduleList/IrrigationStateMachine directly (see agents.md's driver-
// interface-only rule for domain/) — the caller (src/ui/IrrigatorUi, which owns the live data)
// supplies whatever the currently-selected item needs via Context each call, and executes the
// UiIntent this returns. That keeps this class fully host-testable.
//
// Scope note: schedule-record editing only covers the "For Xmin" duration (existing formula)
// and the active/paused toggle. Editing a record's zone/day-of-week/start-time fields from the
// local UI is not implemented yet — the spec lists them as menu rows (shown here, view-only)
// but doesn't specify a tick-adjustment shape for them the way it does for durations.
class UiStateMachine {
public:
    struct Context {
        bool wateringActive = false;
        bool scheduleActive = false;
        uint8_t scheduleRecordCount = 0;
        int16_t wateringPercentage = 0;
        uint8_t defaultManualDuration = 0;
        // Only need to be valid when a MenuSchedule row is selected; the caller refreshes these
        // from the live schedule record at selectedIndex()/kScheduleFieldsPerRecord before
        // calling handleInput().
        uint16_t selectedScheduleDurationMin = 0;
        bool selectedScheduleActive = false;
    };

    static constexpr uint8_t kScheduleFieldsPerRecord = 5;
    static constexpr uint8_t kScheduleFieldZone = 0;
    static constexpr uint8_t kScheduleFieldDay = 1;
    static constexpr uint8_t kScheduleFieldTime = 2;
    static constexpr uint8_t kScheduleFieldDuration = 3;
    static constexpr uint8_t kScheduleFieldActive = 4;

    UiIntent handleInput(UiInputEvent event, const Context& ctx, uint32_t nowMs);

    // Call every loop() iteration regardless of input, to catch the 20s active-interaction
    // timeout expiring with no further ticks/clicks.
    void tick(uint32_t nowMs);

    UiScreen screen() const { return screen_; }
    uint8_t selectedIndex() const { return selectedIndex_; }
    bool editing() const { return editing_; }
    uint8_t pendingZone() const { return pendingZone_; }
    uint16_t pendingDuration() const { return pendingDuration_; }
    bool stopConfirmYesSelected() const { return stopConfirmYes_; }
    int16_t pendingPercentage() const { return pendingPercentage_; }

private:
    void enterDisplayOff();
    void wake(uint32_t nowMs);
    void armTimeout(uint32_t nowMs);

    UiIntent handleNormal(UiInputEvent event, const Context& ctx);
    UiIntent handleZonePicker(UiInputEvent event, const Context& ctx);
    UiIntent handleDurationPicker(UiInputEvent event);
    UiIntent handleStopConfirm(UiInputEvent event);
    UiIntent handleMenuMain(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuSchedule(UiInputEvent event, const Context& ctx);
    UiIntent handleMenuSettings(UiInputEvent event, const Context& ctx);

    static uint8_t stepIndex(uint8_t current, uint8_t count, bool forward);

    UiScreen screen_ = UiScreen::DisplayOff;
    uint8_t selectedIndex_ = 0;
    bool editing_ = false;

    uint8_t pendingZone_ = 0;
    uint16_t pendingDuration_ = 0;
    bool stopConfirmYes_ = false;
    int16_t pendingPercentage_ = 0;

    bool active_ = false;  // Active-interaction vs. display-off.
    uint32_t interactionDeadlineMs_ = 0;
};

}  // namespace irrigator::domain
