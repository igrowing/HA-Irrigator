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
    MenuQueue,
    MenuScheduleRoot,
    MenuSchedule,
    ScheduleAdd,
    ScheduleDelete,
    ScheduleDeleteConfirm,
    ScheduleActionResult,
    MenuSettings,
    MenuInfo,
    MenuTestAll,
    RestartConfirm,
    FactoryResetConfirm,
    FactoryResetResult,
    CalibrateConfirm,
    CalibrateResult,
    QueueAddResult,
};

enum class UiIntentType : uint8_t {
    None,
    StartWatering,
    AbortWatering,
    StopAllWatering,
    ToggleScheduleActive,
    ToggleAutoAdjustment,
    ToggleAlertPumpUc,
    ToggleAlertPumpOc,
    ToggleAlertValveUc,
    ToggleAlertValveOc,
    SetWateringPercentage,
    SetUpperLimit,
    SetLowerLimit,
    SetRainForecastProbability,
    SetScheduleZone,
    SetScheduleDay,
    SetScheduleTime,
    SetScheduleDuration,
    SetScheduleActive,
    AddScheduleRecord,
    DeleteScheduleRecord,
    RunAllZonesTest,
    RestartDevice,
    FactoryReset,
    CalibrateCurrentSensors,
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
