#include "UiStateMachine.h"

#include "DurationAdjuster.h"
#include "board_config.h"

namespace irrigator::domain {

uint8_t UiStateMachine::stepIndex(uint8_t current, uint8_t count, bool forward) {
    if (count == 0) {
        return 0;
    }
    if (forward) {
        return static_cast<uint8_t>((current + 1) % count);
    }
    return static_cast<uint8_t>((current + count - 1) % count);
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
            return handleDurationPicker(event);
        case UiScreen::StopConfirm:
            return handleStopConfirm(event);
        case UiScreen::MenuMain:
            return handleMenuMain(event, ctx);
        case UiScreen::MenuSchedule:
            return handleMenuSchedule(event, ctx);
        case UiScreen::MenuSettings:
            return handleMenuSettings(event, ctx);
        case UiScreen::DisplayOff:
        default:
            return {};
    }
}

UiIntent UiStateMachine::handleNormal(UiInputEvent event, const Context& ctx) {
    if (ctx.wateringActive) {
        if (event == UiInputEvent::LongClick) {
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
        }
        return {};  // Ticks ignored while watering, aside from the timeout re-arm above.
    }

    switch (event) {
        case UiInputEvent::Clockwise:
            pendingZone_ = 1;
            screen_ = UiScreen::ZonePicker;
            break;
        case UiInputEvent::CounterClockwise:
            pendingZone_ = board::ZONE_COUNT;
            screen_ = UiScreen::ZonePicker;
            break;
        case UiInputEvent::Click:
            pendingZone_ = 0;  // Pump-only: the implicit default candidate.
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
            pendingZone_ = static_cast<uint8_t>((pendingZone_ + 1) % (board::ZONE_COUNT + 1));
            break;
        case UiInputEvent::CounterClockwise:
            pendingZone_ =
                static_cast<uint8_t>((pendingZone_ + board::ZONE_COUNT) % (board::ZONE_COUNT + 1));
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

UiIntent UiStateMachine::handleDurationPicker(UiInputEvent event) {
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
            screen_ = UiScreen::Normal;
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
                intent.type = UiIntentType::AbortWatering;
            }
            stopConfirmYes_ = false;
            screen_ = UiScreen::Normal;
            break;
        case UiInputEvent::LongClick:
            stopConfirmYes_ = false;
            screen_ = UiScreen::Normal;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleMenuMain(UiInputEvent event, const Context& ctx) {
    const uint8_t itemCount = static_cast<uint8_t>(ctx.wateringActive ? 4 : 3);

    switch (event) {
        case UiInputEvent::Clockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, false);
            break;
        case UiInputEvent::Click: {
            const uint8_t scheduleIndex = static_cast<uint8_t>(ctx.wateringActive ? 1 : 0);
            const uint8_t settingsIndex = static_cast<uint8_t>(ctx.wateringActive ? 2 : 1);
            const uint8_t backIndex = static_cast<uint8_t>(ctx.wateringActive ? 3 : 2);
            if (ctx.wateringActive && selectedIndex_ == 0) {
                stopConfirmYes_ = false;
                screen_ = UiScreen::StopConfirm;
            } else if (selectedIndex_ == scheduleIndex) {
                screen_ = UiScreen::MenuSchedule;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == settingsIndex) {
                screen_ = UiScreen::MenuSettings;
                selectedIndex_ = 0;
            } else if (selectedIndex_ == backIndex) {
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
    return {};
}

UiIntent UiStateMachine::handleMenuSchedule(UiInputEvent event, const Context& ctx) {
    const uint8_t itemCount =
        static_cast<uint8_t>(ctx.scheduleRecordCount * kScheduleFieldsPerRecord + 1);
    UiIntent intent;

    if (editing_) {
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
                intent.scheduleRecordIndex =
                    static_cast<uint8_t>(selectedIndex_ / kScheduleFieldsPerRecord);
                intent.duration = pendingDuration_;
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
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, itemCount, false);
            break;
        case UiInputEvent::Click: {
            const uint8_t backIndex = static_cast<uint8_t>(itemCount - 1);
            if (ctx.scheduleRecordCount == 0 || selectedIndex_ == backIndex) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
                break;
            }
            const uint8_t fieldIndex = static_cast<uint8_t>(selectedIndex_ % kScheduleFieldsPerRecord);
            if (fieldIndex == kScheduleFieldDuration) {
                pendingDuration_ = ctx.selectedScheduleDurationMin;
                editing_ = true;
            } else if (fieldIndex == kScheduleFieldActive) {
                intent.type = UiIntentType::SetScheduleActive;
                intent.scheduleRecordIndex =
                    static_cast<uint8_t>(selectedIndex_ / kScheduleFieldsPerRecord);
                intent.boolValue = !ctx.selectedScheduleActive;
            }
            // Zone/day/time fields: view-only in this pass, click is a no-op.
            break;
        }
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

UiIntent UiStateMachine::handleMenuSettings(UiInputEvent event, const Context& ctx) {
    UiIntent intent;
    constexpr uint8_t kItemCount = 5;
    constexpr uint8_t kPercentageIndex = 0;
    constexpr uint8_t kScheduleActiveIndex = 1;
    constexpr uint8_t kBackIndex = 4;

    if (editing_) {
        switch (event) {
            case UiInputEvent::Clockwise:
                pendingPercentage_ = static_cast<int16_t>(pendingPercentage_ + 10);
                break;
            case UiInputEvent::CounterClockwise:
                pendingPercentage_ = static_cast<int16_t>(pendingPercentage_ - 10);
                break;
            case UiInputEvent::Click:
                intent.type = UiIntentType::SetWateringPercentage;
                intent.percentage = pendingPercentage_;
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
            selectedIndex_ = stepIndex(selectedIndex_, kItemCount, true);
            break;
        case UiInputEvent::CounterClockwise:
            selectedIndex_ = stepIndex(selectedIndex_, kItemCount, false);
            break;
        case UiInputEvent::Click:
            if (selectedIndex_ == kPercentageIndex) {
                pendingPercentage_ = ctx.wateringPercentage;
                editing_ = true;
            } else if (selectedIndex_ == kScheduleActiveIndex) {
                intent.type = UiIntentType::ToggleScheduleActive;
                intent.boolValue = !ctx.scheduleActive;
            } else if (selectedIndex_ == kBackIndex) {
                screen_ = UiScreen::MenuMain;
                selectedIndex_ = 0;
            }
            // IP / MQTT IP rows are explicitly not clickable.
            break;
        case UiInputEvent::LongClick:
            screen_ = UiScreen::MenuMain;
            selectedIndex_ = 0;
            break;
    }
    return intent;
}

}  // namespace irrigator::domain
