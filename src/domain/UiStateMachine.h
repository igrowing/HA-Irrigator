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
// Every menu row is clickable: it either opens a submenu or enters edit mode for that item
// (only IP/MQTT IP are spec'd "not clickable"). Zone/day-of-week cycle by 1 per tick; start-time
// is edited as two sequential steps (hour, then minute) under the same click-to-edit gesture.
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
    int16_t pendingPercentage() const { return pendingPercentage_; }
    bool factoryResetConfirmYesSelected() const { return factoryResetConfirmYes_; }
    bool factoryResetSucceeded() const { return factoryResetSucceeded_; }

    uint8_t pendingScheduleZone() const { return pendingScheduleZone_; }
    uint8_t pendingScheduleDay() const { return pendingScheduleDay_; }
    uint8_t pendingScheduleHour() const { return pendingScheduleHour_; }
    uint8_t pendingScheduleMinute() const { return pendingScheduleMinute_; }
    bool editingScheduleTimeMinuteStep() const { return editingScheduleMinuteStep_; }

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
    UiIntent handleFactoryResetConfirm(UiInputEvent event);

    static uint8_t stepIndex(uint8_t current, uint8_t count, bool forward);

    UiScreen screen_ = UiScreen::DisplayOff;
    uint8_t selectedIndex_ = 0;
    bool editing_ = false;

    uint8_t pendingZone_ = 0;
    uint16_t pendingDuration_ = 0;
    bool stopConfirmYes_ = false;
    int16_t pendingPercentage_ = 0;

    uint8_t pendingScheduleZone_ = 0;
    uint8_t pendingScheduleDay_ = 0;
    uint8_t pendingScheduleHour_ = 0;
    uint8_t pendingScheduleMinute_ = 0;
    bool editingScheduleMinuteStep_ = false;

    bool factoryResetConfirmYes_ = false;
    bool factoryResetSucceeded_ = false;

    bool active_ = false;  // Active-interaction vs. display-off.
    uint32_t interactionDeadlineMs_ = 0;
};

}  // namespace irrigator::domain
