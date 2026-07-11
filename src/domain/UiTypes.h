#pragma once

#include <stdint.h>

namespace irrigator::domain {

enum class UiInputEvent : uint8_t { Clockwise, CounterClockwise, Click, LongClick };

enum class UiScreen : uint8_t {
    DisplayOff,
    Normal,
    ZonePicker,
    DurationPicker,
    StopConfirm,
    MenuMain,
    MenuSchedule,
    MenuSettings,
};

enum class UiIntentType : uint8_t {
    None,
    StartWatering,
    AbortWatering,
    ToggleScheduleActive,
    SetWateringPercentage,
    SetScheduleDuration,
    SetScheduleActive,
};

struct UiIntent {
    UiIntentType type = UiIntentType::None;
    uint8_t zone = 0;
    uint16_t duration = 0;
    int16_t percentage = 0;
    uint8_t scheduleRecordIndex = 0;
    bool boolValue = false;
};

}  // namespace irrigator::domain
