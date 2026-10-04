#include "UiStateMachine.h"

#include <stdio.h>

#include "DurationAdjuster.h"
#include "board_config.h"

namespace irrigator::domain {

uint16_t UiStateMachine::stepIndex(uint16_t current, uint16_t count, bool forward) {
    if (count == 0) {
        return 0;
    }
    if (forward) {
        return static_cast<uint16_t>((current + 1) % count);
    }
    return static_cast<uint16_t>((current + count - 1) % count);
}

uint8_t UiStateMachine::nextZone(uint8_t current, bool forward, const Context& ctx) {
    uint8_t zone = current;
    for (uint8_t attempt = 0; attempt <= board::ZONE_COUNT; ++attempt) {
        zone = forward ? static_cast<uint8_t>((zone + 1) % (board::ZONE_COUNT + 1))
                       : static_cast<uint8_t>((zone + board::ZONE_COUNT) % (board::ZONE_COUNT + 1));
        if (zone == 0 || ctx.zoneEnabled[zone - 1]) {
            return zone;
        }
    }
    return 0;  // Every zone disabled: PUMP is the only reachable option left.
}

void UiStateMachine::armTimeout(uint32_t nowMs) {
    active_ = true;
    interactionDeadlineMs_ = nowMs + board::ACTIVE_INTERACTION_TIMEOUT_MS;
}

void UiStateMachine::enterDisplayOff() {
    screen_ = UiScreen::DisplayOff;
    active_ = false;
    editing_ = false;
    selectedIndex_ = 0;
}

void UiStateMachine::wake(uint32_t nowMs) {
    armTimeout(nowMs);
    screen_ = UiScreen::Normal;
    selectedIndex_ = 0;
    editing_ = false;
}

void UiStateMachine::enterFactoryResetConfirm(uint32_t nowMs) {
    armTimeout(nowMs);
    screen_ = UiScreen::FactoryResetConfirm;
    factoryResetConfirmYes_ = false;
    editing_ = false;
}

void UiStateMachine::showFactoryResetResult(bool success, uint32_t nowMs) {
    armTimeout(nowMs);
    screen_ = UiScreen::FactoryResetResult;
    factoryResetSucceeded_ = success;
}

void UiStateMachine::showCalibrateResult(bool valveSucceeded,
                                         uint16_t valveBaseLevel,
                                         const char* valveMessage,
                                         bool pumpSucceeded,
                                         uint16_t pumpBaseLevel,
                                         const char* pumpMessage,
                                         uint32_t nowMs) {
    armTimeout(nowMs);
    screen_ = UiScreen::CalibrateResult;
    calibrateValveSucceeded_ = valveSucceeded;
    calibrateValveBaseLevel_ = valveBaseLevel;
    snprintf(calibrateValveMessage_, sizeof(calibrateValveMessage_), "%s", valveMessage == nullptr ? "" : valveMessage);
    calibratePumpSucceeded_ = pumpSucceeded;
    calibratePumpBaseLevel_ = pumpBaseLevel;
    snprintf(calibratePumpMessage_, sizeof(calibratePumpMessage_), "%s", pumpMessage == nullptr ? "" : pumpMessage);
}

void UiStateMachine::showScheduleActionResult(bool success, uint32_t nowMs) {
    armTimeout(nowMs);
    screen_ = UiScreen::ScheduleActionResult;
    scheduleActionSucceeded_ = success;
}

void UiStateMachine::showQueueAddResult(const char* zoneLabel, uint16_t duration, bool success, uint32_t nowMs) {
    armTimeout(nowMs);
    screen_ = UiScreen::QueueAddResult;
    snprintf(queueAddZoneLabel_, sizeof(queueAddZoneLabel_), "%s", zoneLabel == nullptr ? "" : zoneLabel);
    queueAddDuration_ = duration;
    queueAddSucceeded_ = success;
}

void UiStateMachine::tick(uint32_t nowMs) {
    if (active_ && screen_ != UiScreen::DisplayOff && nowMs >= interactionDeadlineMs_) {
        enterDisplayOff();
    }
}

UiIntent UiStateMachine::handleInput(UiInputEvent event, const Context& ctx, uint32_t nowMs) {
    if (screen_ == UiScreen::DisplayOff) {
        wake(nowMs);
        return {};
    }

    armTimeout(nowMs);

    switch (screen_) {
        case UiScreen::Normal:
            return handleNormal(event, ctx);
        case UiScreen::ZonePicker:
            return handleZonePicker(event, ctx);
        case UiScreen::DurationPicker:
            return handleDurationPicker(event, ctx);
        case UiScreen::StopConfirm:
            return handleStopConfirm(event);
        case UiScreen::MenuMain:
            return handleMenuMain(event, ctx);
        case UiScreen::MenuQueue:
            return handleMenuQueue(event, ctx);
        case UiScreen::MenuScheduleRoot:
            return handleMenuScheduleRoot(event);
        case UiScreen::MenuSchedule:
            return handleMenuSchedule(event, ctx);
        case UiScreen::ScheduleAdd:
            return handleScheduleAdd(event);
        case UiScreen::ScheduleDelete:
            return handleScheduleDelete(event, ctx);
        case UiScreen::ScheduleDeleteConfirm:
            return handleScheduleDeleteConfirm(event);
        case UiScreen::ScheduleActionResult:
            return handleScheduleActionResult(event);
        case UiScreen::MenuSettings:
            return handleMenuSettings(event, ctx);
        case UiScreen::MenuInfo:
            return handleMenuInfo(event);
        case UiScreen::MenuTestAll:
            return handleMenuTestAll(event);
        case UiScreen::RestartConfirm:
            return handleYesNoConfirm(event, restartConfirmYes_, UiIntentType::RestartDevice);
        case UiScreen::FactoryResetConfirm:
            return handleYesNoConfirm(event, factoryResetConfirmYes_, UiIntentType::FactoryReset);
        case UiScreen::CalibrateConfirm:
            return handleYesNoConfirm(event, calibrateConfirmYes_, UiIntentType::CalibrateCurrentSensors);
        case UiScreen::CalibrateResult:
        case UiScreen::QueueAddResult:
            return handleDeadEndResult(event);
        case UiScreen::FactoryResetResult:
        case UiScreen::DisplayOff:
        default:
            return {};
    }
}

UiIntent UiStateMachine::handleNormal(UiInputEvent event, const Context& ctx) {
    // Rotation/click work the same whether or not a run is already active — while watering, this
    // is how the user picks the *next* watering to append to the queue (see UiIntentType::
    // StartWatering below, and enqueueOrStart() in main.ino which already pushes every request
    // through the queue regardless of whether it can start immediately). handleDurationPicker's
    // Click is what tells the two cases apart and shows the "Added to queue" confirmation.
    //
    // Rotate relative to whatever the Normal screen is currently showing (the running zone while
    // watering, the next scheduled zone when idle+scheduled, PUMP otherwise) rather than always
    // starting from PUMP. The three cases and their precedence mirror IrrigatorUi::renderNormal's
    // `running`/`scheduled`/else chain for the big middle line exactly — if that chain changes,
    // this one has to change with it, or the knob starts moving relative to a zone that isn't the
    // one on screen.
    const bool scheduled = ctx.scheduleActive && ctx.nextScheduledZone != 0;
    const uint8_t displayedZone =
        ctx.wateringActive ? ctx.activeZone : (scheduled ? ctx.nextScheduledZone : 0);

    switch (event) {
        case UiInputEvent::Clockwise:
            pendingZone_ = nextZone(displayedZone, true, ctx);
            screen_ = UiScreen::ZonePicker;
            break;
        case UiInputEvent::CounterClockwise:
            pendingZone_ = nextZone(displayedZone, false, ctx);
            screen_ = UiScreen::ZonePicker;
            break;
        case UiInputEvent::Click:
            // While watering, a click with no prior rotation means "another run of the zone I'm
            // looking at" — the running zone is what the screen is showing, so skipping the zone
            // picker has to land on that zone, not on PUMP. Idle keeps the pump-only default:
            // there the screen shows PUMP too (see renderNormal's else branch), so a bare click
            // still matches what's displayed.
            pendingZone_ = ctx.wateringActive ? ctx.activeZone : 0;
            pendingDuration_ = ctx.defaultManualDuration;
            screen_ = UiScreen::DurationPicker;
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return {};
}

UiIntent UiStateMachine::handleZonePicker(UiInputEvent event, const Context& ctx) {
    switch (event) {
        case UiInputEvent::Clockwise:
            pendingZone_ = nextZone(pendingZone_, true, ctx);
            break;
        case UiInputEvent::CounterClockwise:
            pendingZone_ = nextZone(pendingZone_, false, ctx);
            break;
        case UiInputEvent::Click:
            pendingDuration_ = ctx.defaultManualDuration;
            screen_ = UiScreen::DurationPicker;
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return {};
}

UiIntent UiStateMachine::handleDurationPicker(UiInputEvent event, const Context& ctx) {
    UiIntent intent;
    switch (event) {
        case UiInputEvent::Clockwise:
            pendingDuration_ = adjustManualDuration(pendingDuration_, RotationDirection::Clockwise);
            break;
        case UiInputEvent::CounterClockwise:
            pendingDuration_ =
                adjustManualDuration(pendingDuration_, RotationDirection::CounterClockwise);
            break;
        case UiInputEvent::Click:
            intent.type = UiIntentType::StartWatering;
            intent.zone = pendingZone_;
            intent.duration = pendingDuration_;
            // Flags to main.ino whether this request is going behind an already-running watering
            // (queued) rather than starting immediately — main.ino echoes it back via
            // showQueueAddResult() after actually pushing the request, which is what advances
            // screen_ to QueueAddResult; a queue-full drop still needs that screen (to say so), so
            // this deliberately doesn't jump to Normal itself when queued, same reasoning as
            // handleCalibrateConfirm leaving CalibrateConfirm on screen for showCalibrateResult to
            // override.
            intent.boolValue = ctx.wateringActive;
            if (!ctx.wateringActive) {
                screen_ = UiScreen::Normal;
            }
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::Normal;  // Cancel, discard.
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleStopConfirm(UiInputEvent event) {
    UiIntent intent;
    switch (event) {
        case UiInputEvent::Clockwise:
        case UiInputEvent::CounterClockwise:
            stopConfirmYes_ = !stopConfirmYes_;
            break;
        case UiInputEvent::Click:
            if (stopConfirmYes_) {
                intent.type = stopAllPending_ ? UiIntentType::StopAllWatering : UiIntentType::AbortWatering;
            }
            stopConfirmYes_ = false;
            stopAllPending_ = false;
            screen_ = UiScreen::Normal;
            break;
        case UiInputEvent::LongClick:
            stopConfirmYes_ = false;
            stopAllPending_ = false;
            screen_ = UiScreen::Normal;
            break;
    }
    return intent;
}

// Shared by FactoryResetConfirm/RestartConfirm/CalibrateConfirm — all three are the same Yes/No
// gesture (CW/CCW toggles the selection, Click on YES emits confirmedIntentType and Click on NO or
// LongClick cancels), differing only in which flag they toggle and what they emit. Confirmed does
// NOT transition screen_ itself (unlike cancel) — the caller (main.ino's handleUiIntent) is
// expected to perform the action and then force the next screen (e.g. showCalibrateResult()),
// same reasoning as handleDeadEndResult below. StopConfirm stays separate: it also branches on
// stopAllPending_ to pick between two intent types, which doesn't fit this single-intent shape.
UiIntent UiStateMachine::handleYesNoConfirm(UiInputEvent event, bool& yesSelected, UiIntentType confirmedIntentType) {
    UiIntent intent;
    switch (event) {
        case UiInputEvent::Clockwise:
        case UiInputEvent::CounterClockwise:
            yesSelected = !yesSelected;
            break;
        case UiInputEvent::Click:
            if (yesSelected) {
                intent.type = confirmedIntentType;
            } else {
                screen_ = UiScreen::Normal;
            }
            yesSelected = false;
            break;
        case UiInputEvent::LongClick:
            yesSelected = false;
            screen_ = UiScreen::Normal;
            break;
    }
    return intent;
}

// Shared by CalibrateResult and QueueAddResult — both are a single dead-end "OK" action, not a
// navigable list: Clockwise/CounterClockwise are no-ops (nothing to scroll to), and either Click
// or LongClick dismisses back to Normal. main.ino is expected to have already called
// showCalibrateResult()/showQueueAddResult() with the screen's content before the intent
// round-trip that got here lands. ScheduleActionResult stays separate: it restores a specific
// (screen, index) instead of always landing on Normal, which doesn't fit this shape.
UiIntent UiStateMachine::handleDeadEndResult(UiInputEvent event) {
    if (event == UiInputEvent::Click || event == UiInputEvent::LongClick) {
        screen_ = UiScreen::Normal;
        selectedIndex_ = 0;
    }
    return {};
}

UiStateMachine::MenuMainLayout UiStateMachine::menuMainLayout(bool wateringActive) {
    if (wateringActive) {
        return {8, 0, 1, 2, 3, 4, 5, 6, 7};
    }
    return {5, 0, 0, 0, 0, 1, 2, 3, 4};
}

UiIntent UiStateMachine::handleMenuMain(UiInputEvent event, const Context& ctx) {
    // Item order, matching IrrigatorUi::renderMenuMain: [Queue, Stop, Stop all] (watering only),
    // Schedule, Test all zones, Settings, Info, BACK.
    const MenuMainLayout layout = menuMainLayout(ctx.wateringActive);
    UiIntent intent;

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, layout.itemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, layout.itemCount, false);
            break;
        case UiInputEvent::Click: {
            if (ctx.wateringActive && selectedIndex_ == layout.queueIndex) {
                screen_ = UiScreen::MenuQueue;
                selectedIndex_ = 0;
            } else if (ctx.wateringActive && selectedIndex_ == layout.stopIndex) {
                stopConfirmYes_ = false;
                stopAllPending_ = false;
                screen_ = UiScreen::StopConfirm;
            } else if (ctx.wateringActive && selectedIndex_ == layout.stopAllIndex) {
                stopConfirmYes_ = false;
                stopAllPending_ = true;
                screen_ = UiScreen::StopConfirm;
            } else if (selectedIndex_ == layout.scheduleIndex) {
                screen_ = UiScreen::MenuScheduleRoot;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == layout.testAllIndex) {
                screen_ = UiScreen::MenuTestAll;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == layout.settingsIndex) {
                screen_ = UiScreen::MenuSettings;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == layout.infoIndex) {
                screen_ = UiScreen::MenuInfo;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == layout.backIndex) {
                screen_ = UiScreen::Normal;
                selectedIndex_ = 0;
            }
            break;
        }
        case UiInputEvent::LongClick:
            screen_ = UiScreen::Normal;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

// Read-only, same shape as MenuInfo: the queue can only be viewed here (per spec, on-device we can
// only "stop all" blindly), so only the trailing BACK row is clickable — the data rows just scroll
// past. With an empty queue, queueMenuItemCount(0) == 1, so BACK is the only row and stepIndex's
// count==1 wraparound keeps selectedIndex_ pinned at 0 regardless of rotation.
UiIntent UiStateMachine::handleMenuQueue(UiInputEvent event, const Context& ctx) {
    const uint16_t itemCount = queueMenuItemCount(ctx.queueSize);
    const uint16_t backIndex = static_cast<uint16_t>(itemCount - 1);
    UiIntent intent;

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, false);
            break;
        case UiInputEvent::Click:
            if (selectedIndex_ == backIndex) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
            }
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleMenuSchedule(UiInputEvent event, const Context& ctx) {
    const uint16_t itemCount = scheduleItemCount(ctx.scheduleRecordCount);
    UiIntent intent;

    if (editing_) {
        const uint8_t fieldIndex = static_cast<uint8_t>(selectedIndex_ % kScheduleFieldsPerRecord);
        const uint8_t recordIndex = static_cast<uint8_t>(selectedIndex_ / kScheduleFieldsPerRecord);
        return handleMenuScheduleEdit(event, fieldIndex, recordIndex);
    }

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, false);
            break;
        case UiInputEvent::Click: {
            const uint16_t backIndex = static_cast<uint16_t>(itemCount - 1);
            if (ctx.scheduleRecordCount == 0 || selectedIndex_ == backIndex) {
                screen_ = UiScreen::MenuScheduleRoot;
                selectedIndex_ = kMenuScheduleRootIndexViewEdit;
                break;
            }
            // Every row is clickable: it either opens edit mode for that field, or (Active) is
            // a direct toggle with no separate edit step.
            const uint8_t fieldIndex = static_cast<uint8_t>(selectedIndex_ % kScheduleFieldsPerRecord);
            switch (fieldIndex) {
                case kScheduleFieldZone:
                    pendingScheduleZone_ = ctx.selectedScheduleZone;
                    editing_ = true;
                    break;
                case kScheduleFieldDay:
                    pendingScheduleDay_ = ctx.selectedScheduleDay;
                    editing_ = true;
                    break;
                case kScheduleFieldTime:
                    pendingScheduleHour_ = ctx.selectedScheduleHour;
                    pendingScheduleMinute_ = ctx.selectedScheduleMinute;
                    editingScheduleMinuteStep_ = false;
                    editing_ = true;
                    break;
                case kScheduleFieldDuration:
                    pendingDuration_ = ctx.selectedScheduleDurationMin;
                    editing_ = true;
                    break;
                case kScheduleFieldActive:
                    intent.type = UiIntentType::SetScheduleActive;
                    intent.scheduleRecordIndex =
                        static_cast<uint8_t>(selectedIndex_ / kScheduleFieldsPerRecord);
                    intent.boolValue = !ctx.selectedScheduleActive;
                    break;
                default:
                    break;
            }
            break;
        }
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuScheduleRoot;
            selectedIndex_ = kMenuScheduleRootIndexViewEdit;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleMenuScheduleEdit(UiInputEvent event,
                                                uint8_t fieldIndex,
                                                uint8_t recordIndex) {
    UiIntent intent;

    if (fieldIndex == kScheduleFieldZone) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingScheduleZone_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleZone_ - 1), board::ZONE_COUNT, true) +
                    1);
                break;
            case UiInputEvent::CounterClockwise:
                pendingScheduleZone_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleZone_ - 1), board::ZONE_COUNT, false) +
                    1);
                break;
            case UiInputEvent::Click:
                intent.type = UiIntentType::SetScheduleZone;
                intent.scheduleRecordIndex = recordIndex;
                intent.zone = pendingScheduleZone_;
                editing_ = false;
                break;
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    if (fieldIndex == kScheduleFieldDay) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingScheduleDay_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleDay_ - 1), 7, true) + 1);
                break;
            case UiInputEvent::CounterClockwise:
                pendingScheduleDay_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleDay_ - 1), 7, false) + 1);
                break;
            case UiInputEvent::Click:
                intent.type = UiIntentType::SetScheduleDay;
                intent.scheduleRecordIndex = recordIndex;
                intent.dayOfWeek = pendingScheduleDay_;
                editing_ = false;
                break;
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    if (fieldIndex == kScheduleFieldTime) {
        if (!editingScheduleMinuteStep_) {
            switch (event) {
                case UiInputEvent::Clockwise:
                    pendingScheduleHour_ = stepIndex(pendingScheduleHour_, 24, true);
                    break;
                case UiInputEvent::CounterClockwise:
                    pendingScheduleHour_ = stepIndex(pendingScheduleHour_, 24, false);
                    break;
                case UiInputEvent::Click:
                    editingScheduleMinuteStep_ = true;  // Hour confirmed; now edit the minute.
                    break;
                case UiInputEvent::LongClick:
                    editing_ = false;
                    break;
            }
            return intent;
        }

        switch (event) {
            case UiInputEvent::Clockwise:
                pendingScheduleMinute_ = stepIndex(pendingScheduleMinute_, 60, true);
                break;
            case UiInputEvent::CounterClockwise:
                pendingScheduleMinute_ = stepIndex(pendingScheduleMinute_, 60, false);
                break;
            case UiInputEvent::Click:
                intent.type = UiIntentType::SetScheduleTime;
                intent.scheduleRecordIndex = recordIndex;
                intent.hour = pendingScheduleHour_;
                intent.minute = pendingScheduleMinute_;
                editing_ = false;
                editingScheduleMinuteStep_ = false;
                break;
            case UiInputEvent::LongClick:
                editing_ = false;
                editingScheduleMinuteStep_ = false;
                break;
        }
        return intent;
    }

    if (fieldIndex == kScheduleFieldDuration) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingDuration_ =
                    adjustScheduledDuration(pendingDuration_, RotationDirection::Clockwise);
                break;
            case UiInputEvent::CounterClockwise:
                pendingDuration_ =
                    adjustScheduledDuration(pendingDuration_, RotationDirection::CounterClockwise);
                break;
            case UiInputEvent::Click:
                intent.type = UiIntentType::SetScheduleDuration;
                intent.scheduleRecordIndex = recordIndex;
                intent.duration = pendingDuration_;
                editing_ = false;
                break;
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    // Active never enters editing_ (it's a direct toggle) — defensive fallback only.
    editing_ = false;
    return intent;
}

UiIntent UiStateMachine::handleMenuScheduleRoot(UiInputEvent event) {
    UiIntent intent;
    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kMenuScheduleRootItemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kMenuScheduleRootItemCount, false);
            break;
        case UiInputEvent::Click:
            if (selectedIndex_ == kMenuScheduleRootIndexViewEdit) {
                screen_ = UiScreen::MenuSchedule;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == kMenuScheduleRootIndexAdd) {
                pendingScheduleZone_ = 1;
                pendingScheduleDay_ = 1;
                pendingScheduleHour_ = 6;
                pendingScheduleMinute_ = 0;
                pendingDuration_ = board::SCHEDULE_DURATION_MIN_MINUTES;
                editingScheduleMinuteStep_ = false;
                screen_ = UiScreen::ScheduleAdd;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == kMenuScheduleRootIndexDelete) {
                screen_ = UiScreen::ScheduleDelete;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == kMenuScheduleRootIndexBack) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
            }
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleScheduleAdd(UiInputEvent event) {
    if (editing_) {
        return handleScheduleAddEdit(event, static_cast<uint8_t>(selectedIndex_));
    }

    UiIntent intent;
    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kScheduleAddItemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kScheduleAddItemCount, false);
            break;
        case UiInputEvent::Click:
            switch (selectedIndex_) {
                case kScheduleAddIndexZone:
                case kScheduleAddIndexDay:
                case kScheduleAddIndexDuration:
                    editing_ = true;
                    break;
                case kScheduleAddIndexTime:
                    editingScheduleMinuteStep_ = false;
                    editing_ = true;
                    break;
                case kScheduleAddIndexSave:
                    intent.type = UiIntentType::AddScheduleRecord;
                    intent.zone = pendingScheduleZone_;
                    intent.dayOfWeek = pendingScheduleDay_;
                    intent.hour = pendingScheduleHour_;
                    intent.minute = pendingScheduleMinute_;
                    intent.duration = pendingDuration_;
                    scheduleResultReturnScreen_ = UiScreen::MenuScheduleRoot;
                    scheduleResultReturnIndex_ = kMenuScheduleRootIndexAdd;
                    break;
                case kScheduleAddIndexBack:
                    screen_ = UiScreen::MenuScheduleRoot;
                    selectedIndex_ = kMenuScheduleRootIndexViewEdit;
                    break;
                default:
                    break;
            }
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuScheduleRoot;
            selectedIndex_ = kMenuScheduleRootIndexViewEdit;
            break;
    }
    return intent;
}

// Structurally parallel to handleMenuScheduleEdit (same zone/day/time/duration stepping), but
// Click never emits an intent — it only stages the pending value and exits editing_, since
// ScheduleAdd bundles all fields into one AddScheduleRecord intent on Save rather than committing
// each field immediately.
UiIntent UiStateMachine::handleScheduleAddEdit(UiInputEvent event, uint8_t fieldIndex) {
    UiIntent intent;

    if (fieldIndex == kScheduleAddIndexZone) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingScheduleZone_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleZone_ - 1), board::ZONE_COUNT, true) +
                    1);
                break;
            case UiInputEvent::CounterClockwise:
                pendingScheduleZone_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleZone_ - 1), board::ZONE_COUNT, false) +
                    1);
                break;
            case UiInputEvent::Click:
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    if (fieldIndex == kScheduleAddIndexDay) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingScheduleDay_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleDay_ - 1), 7, true) + 1);
                break;
            case UiInputEvent::CounterClockwise:
                pendingScheduleDay_ = static_cast<uint8_t>(
                    stepIndex(static_cast<uint8_t>(pendingScheduleDay_ - 1), 7, false) + 1);
                break;
            case UiInputEvent::Click:
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    if (fieldIndex == kScheduleAddIndexTime) {
        if (!editingScheduleMinuteStep_) {
            switch (event) {
                case UiInputEvent::Clockwise:
                    pendingScheduleHour_ = stepIndex(pendingScheduleHour_, 24, true);
                    break;
                case UiInputEvent::CounterClockwise:
                    pendingScheduleHour_ = stepIndex(pendingScheduleHour_, 24, false);
                    break;
                case UiInputEvent::Click:
                    editingScheduleMinuteStep_ = true;  // Hour confirmed; now edit the minute.
                    break;
                case UiInputEvent::LongClick:
                    editing_ = false;
                    break;
            }
            return intent;
        }

        switch (event) {
            case UiInputEvent::Clockwise:
                pendingScheduleMinute_ = stepIndex(pendingScheduleMinute_, 60, true);
                break;
            case UiInputEvent::CounterClockwise:
                pendingScheduleMinute_ = stepIndex(pendingScheduleMinute_, 60, false);
                break;
            case UiInputEvent::Click:
            case UiInputEvent::LongClick:
                editing_ = false;
                editingScheduleMinuteStep_ = false;
                break;
        }
        return intent;
    }

    if (fieldIndex == kScheduleAddIndexDuration) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingDuration_ = adjustScheduledDuration(pendingDuration_, RotationDirection::Clockwise);
                break;
            case UiInputEvent::CounterClockwise:
                pendingDuration_ =
                    adjustScheduledDuration(pendingDuration_, RotationDirection::CounterClockwise);
                break;
            case UiInputEvent::Click:
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    editing_ = false;
    return intent;
}

UiIntent UiStateMachine::handleScheduleDelete(UiInputEvent event, const Context& ctx) {
    // uint16_t: at ctx.scheduleRecordCount=255 (SCHEDULE_MAX_RECORDS), +1 for BACK is 256, which
    // overflows uint8_t.
    const uint16_t itemCount = static_cast<uint16_t>(ctx.scheduleRecordCount) + 1;
    UiIntent intent;

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, false);
            break;
        case UiInputEvent::Click: {
            const uint16_t backIndex = static_cast<uint16_t>(itemCount - 1);
            if (ctx.scheduleRecordCount == 0 || selectedIndex_ == backIndex) {
                screen_ = UiScreen::MenuScheduleRoot;
                selectedIndex_ = kMenuScheduleRootIndexViewEdit;
                break;
            }
            // selectedIndex_ deliberately left untouched — it doubles as the record index, so
            // returning here later (NO, or OK after a successful delete) lands on the same row.
            // Safe to narrow: a data row's index here is always < scheduleRecordCount <= 255.
            scheduleDeleteRecordIndex_ = static_cast<uint8_t>(selectedIndex_);
            scheduleDeleteConfirmYes_ = false;
            screen_ = UiScreen::ScheduleDeleteConfirm;
            break;
        }
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuScheduleRoot;
            selectedIndex_ = kMenuScheduleRootIndexViewEdit;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleScheduleDeleteConfirm(UiInputEvent event) {
    UiIntent intent;
    switch (event) {
        case UiInputEvent::Clockwise:
        case UiInputEvent::CounterClockwise:
            scheduleDeleteConfirmYes_ = !scheduleDeleteConfirmYes_;
            break;
        case UiInputEvent::Click:
            if (scheduleDeleteConfirmYes_) {
                intent.type = UiIntentType::DeleteScheduleRecord;
                intent.scheduleRecordIndex = scheduleDeleteRecordIndex_;
                scheduleResultReturnScreen_ = UiScreen::ScheduleDelete;
                scheduleResultReturnIndex_ = scheduleDeleteRecordIndex_;
            } else {
                screen_ = UiScreen::ScheduleDelete;
                selectedIndex_ = scheduleDeleteRecordIndex_;
            }
            scheduleDeleteConfirmYes_ = false;
            break;
        case UiInputEvent::LongClick:
            // Cancel back to the same item, same as No — unlike FactoryResetConfirm/
            // CalibrateConfirm's LongClick-to-Normal, this confirm sits 3 levels deep (Schedule >
            // Delete > this), so stranding the user back at Normal would lose their place badly.
            scheduleDeleteConfirmYes_ = false;
            screen_ = UiScreen::ScheduleDelete;
            selectedIndex_ = scheduleDeleteRecordIndex_;
            break;
    }
    return intent;
}

// Dead-end result screen, same shape as handleCalibrateResult: CW/CCW are no-ops, Click/LongClick
// both dismiss — but instead of a fixed target, this restores whatever (screen, index) the click
// that emitted the Add/Delete intent recorded in scheduleResultReturnScreen_/Index_.
UiIntent UiStateMachine::handleScheduleActionResult(UiInputEvent event) {
    if (event == UiInputEvent::Click || event == UiInputEvent::LongClick) {
        screen_ = scheduleResultReturnScreen_;
        selectedIndex_ = scheduleResultReturnIndex_;
    }
    return {};
}

UiIntent UiStateMachine::handleMenuSettings(UiInputEvent event, const Context& ctx) {
    UiIntent intent;

    // selectedIndex_ is left untouched by entering edit mode, so it still tells us which of the
    // editable int16 rows is being adjusted right now. All of them are percentages, so the step
    // is 10.
    if (editing_) {
        int16_t* pending = nullptr;
        constexpr int16_t step = 10;
        UiIntentType setIntentType = UiIntentType::None;
        switch (selectedIndex_) {
            case kSettingsIndexPercentage:
                pending = &pendingPercentage_;
                setIntentType = UiIntentType::SetWateringPercentage;
                break;
            case kSettingsIndexUpperLimit:
                pending = &pendingUpperLimit_;
                setIntentType = UiIntentType::SetUpperLimit;
                break;
            case kSettingsIndexLowerLimit:
                pending = &pendingLowerLimit_;
                setIntentType = UiIntentType::SetLowerLimit;
                break;
            case kSettingsIndexRainForecastProbability:
                pending = &pendingRainForecastProbability_;
                setIntentType = UiIntentType::SetRainForecastProbability;
                break;
            default:
                break;
        }
        switch (event) {
            case UiInputEvent::Clockwise:
                *pending = static_cast<int16_t>(*pending + step);
                break;
            case UiInputEvent::CounterClockwise:
                *pending = static_cast<int16_t>(*pending - step);
                break;
            case UiInputEvent::Click:
                intent.type = setIntentType;
                intent.percentage = *pending;
                editing_ = false;
                break;
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kSettingsItemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kSettingsItemCount, false);
            break;
        case UiInputEvent::Click:
            if (selectedIndex_ == kSettingsIndexAutoAdjustment) {
                intent.type = UiIntentType::ToggleAutoAdjustment;
                intent.boolValue = !ctx.autoAdjustmentEnabled;
            } else if (selectedIndex_ == kSettingsIndexPercentage) {
                pendingPercentage_ = ctx.wateringPercentage;
                editing_ = true;
            } else if (selectedIndex_ == kSettingsIndexUpperLimit) {
                pendingUpperLimit_ = ctx.upperLimitPercent;
                editing_ = true;
            } else if (selectedIndex_ == kSettingsIndexLowerLimit) {
                pendingLowerLimit_ = ctx.lowerLimitPercent;
                editing_ = true;
            } else if (selectedIndex_ == kSettingsIndexRainForecastProbability) {
                pendingRainForecastProbability_ = ctx.rainForecastProbabilityPercent;
                editing_ = true;
            } else if (selectedIndex_ == kSettingsIndexScheduleActive) {
                intent.type = UiIntentType::ToggleScheduleActive;
                intent.boolValue = !ctx.scheduleActive;
            } else if (selectedIndex_ == kSettingsIndexAlertPumpUc) {
                intent.type = UiIntentType::ToggleAlertPumpUc;
                intent.boolValue = !ctx.alertPumpUc;
            } else if (selectedIndex_ == kSettingsIndexAlertPumpOc) {
                intent.type = UiIntentType::ToggleAlertPumpOc;
                intent.boolValue = !ctx.alertPumpOc;
            } else if (selectedIndex_ == kSettingsIndexAlertValveUc) {
                intent.type = UiIntentType::ToggleAlertValveUc;
                intent.boolValue = !ctx.alertValveUc;
            } else if (selectedIndex_ == kSettingsIndexAlertValveOc) {
                intent.type = UiIntentType::ToggleAlertValveOc;
                intent.boolValue = !ctx.alertValveOc;
            } else if (selectedIndex_ == kSettingsIndexRestart) {
                restartConfirmYes_ = false;
                screen_ = UiScreen::RestartConfirm;
            } else if (selectedIndex_ == kSettingsIndexCalibrate) {
                calibrateConfirmYes_ = false;
                screen_ = UiScreen::CalibrateConfirm;
            } else if (selectedIndex_ == kSettingsIndexBack) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
            }
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleMenuInfo(UiInputEvent event) {
    UiIntent intent;

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kInfoItemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kInfoItemCount, false);
            break;
        case UiInputEvent::Click:
            if (selectedIndex_ == kInfoIndexBack) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
            }
            // WiFi/IP/MQTT/Topic/City/Coordinates/LastUpdate/FW rows are display-only, not clickable.
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleMenuTestAll(UiInputEvent event) {
    UiIntent intent;

    if (editing_) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingDuration_ = adjustManualDuration(pendingDuration_, RotationDirection::Clockwise);
                break;
            case UiInputEvent::CounterClockwise:
                pendingDuration_ =
                    adjustManualDuration(pendingDuration_, RotationDirection::CounterClockwise);
                break;
            case UiInputEvent::Click:
                testAllZonesDurationMin_ = pendingDuration_;
                editing_ = false;
                break;
            case UiInputEvent::LongClick:
                editing_ = false;
                break;
        }
        return intent;
    }

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kTestAllItemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kTestAllItemCount, false);
            break;
        case UiInputEvent::Click:
            if (selectedIndex_ == kTestAllIndexRun) {
                intent.type = UiIntentType::RunAllZonesTest;
                intent.duration = testAllZonesDurationMin_;
                screen_ = UiScreen::Normal;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == kTestAllIndexDuration) {
                pendingDuration_ = testAllZonesDurationMin_;
                editing_ = true;
            } else if (selectedIndex_ == kTestAllIndexBack) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
            }
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

}  // namespace irrigator::domain
