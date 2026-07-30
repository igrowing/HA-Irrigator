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
    MenuInfo,
    MenuTestAll,
    RestartConfirm,
    FactoryResetConfirm,
    FactoryResetResult,
};

enum class UiIntentType : uint8_t {
    None,
    StartWatering,
    AbortWatering,
    StopAllWatering,
    ToggleScheduleActive,
    SetWateringPercentage,
    SetUpperLimit,
    SetLowerLimit,
    SetRainSkipProbability,
    SetRainSkipAmount,
    SetScheduleZone,
    SetScheduleDay,
    SetScheduleTime,
    SetScheduleDuration,
    SetScheduleActive,
    RunAllZonesTest,
    RestartDevice,
    FactoryReset,
};

struct UiIntent {
    UiIntentType type = UiIntentType::None;
    uint8_t zone = 0;
    uint16_t duration = 0;
    int16_t percentage = 0;
    uint8_t scheduleRecordIndex = 0;
    uint8_t dayOfWeek = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    bool boolValue = false;
};

}  // namespace irrigator::domain
