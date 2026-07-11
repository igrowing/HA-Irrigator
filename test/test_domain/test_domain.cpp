#include <unity.h>

#include <string.h>

#include "../../src/domain/CalendarMath.h"
#include "../../src/domain/ConfigStore.h"
#include "../../src/domain/CurrentFaultMonitor.h"
#include "../../src/domain/DstRules.h"
#include "../../src/domain/DurationAdjuster.h"
#include "../../src/domain/IrrigationStateMachine.h"
#include "../../src/domain/RainSkip.h"
#include "../../src/domain/Schedule.h"
#include "../../src/domain/ScheduleEngine.h"
#include "../../src/domain/Settings.h"
#include "../../src/domain/UiStateMachine.h"
#include "../../src/domain/WateringQueue.h"
#include "board_config.h"

using irrigator::domain::RotationDirection;

void setUp() {}
void tearDown() {}

void test_manual_duration_adjusts_by_formula_and_clamps() {
    TEST_ASSERT_EQUAL_UINT16(2, irrigator::domain::adjustManualDuration(1, RotationDirection::Clockwise));
    TEST_ASSERT_EQUAL_UINT16(10,
                             irrigator::domain::adjustManualDuration(
                                 12, RotationDirection::CounterClockwise));
    TEST_ASSERT_EQUAL_UINT16(120,
                             irrigator::domain::adjustManualDuration(120, RotationDirection::Clockwise));
    TEST_ASSERT_EQUAL_UINT16(1,
                             irrigator::domain::adjustManualDuration(
                                 1, RotationDirection::CounterClockwise));
}

void test_scheduled_duration_uses_schedule_clamp() {
    TEST_ASSERT_EQUAL_UINT16(60,
                             irrigator::domain::adjustScheduledDuration(60,
                                                                        RotationDirection::Clockwise));
    TEST_ASSERT_EQUAL_UINT16(1,
                             irrigator::domain::adjustScheduledDuration(
                                 1, RotationDirection::CounterClockwise));
}

void test_watering_percentage_rounds_to_nearest_minute() {
    TEST_ASSERT_EQUAL_UINT16(15, irrigator::domain::applyWateringPercentage(10, 150));
    TEST_ASSERT_EQUAL_UINT16(8, irrigator::domain::applyWateringPercentage(10, 75));
    TEST_ASSERT_EQUAL_UINT16(0, irrigator::domain::applyWateringPercentage(10, 0));
}

void test_start_time_validation() {
    TEST_ASSERT_TRUE(irrigator::domain::isValidStartTime("00:00"));
    TEST_ASSERT_TRUE(irrigator::domain::isValidStartTime("23:59"));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("24:00"));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("12:60"));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("7:30"));
}

void test_schedule_record_validation() {
    irrigator::domain::ScheduleRecord valid = {1, 1, "06:30", 20, true};
    TEST_ASSERT_TRUE(irrigator::domain::validateScheduleRecord(valid));

    irrigator::domain::ScheduleRecord invalidZone = {12, 1, "06:30", 20, true};
    TEST_ASSERT_FALSE(irrigator::domain::validateScheduleRecord(invalidZone));

    irrigator::domain::ScheduleRecord invalidDuration = {1, 1, "06:30", 61, true};
    TEST_ASSERT_FALSE(irrigator::domain::validateScheduleRecord(invalidDuration));
}

void test_watering_queue_capacity_and_fifo() {
    irrigator::domain::WateringQueue<2> queue;
    TEST_ASSERT_TRUE(queue.push({1, 5, irrigator::domain::RunReason::Manual}));
    TEST_ASSERT_TRUE(queue.push({2, 10, irrigator::domain::RunReason::Mqtt}));
    TEST_ASSERT_FALSE(queue.push({3, 15, irrigator::domain::RunReason::Schedule}));

    irrigator::domain::WateringRequest request;
    TEST_ASSERT_TRUE(queue.pop(request));
    TEST_ASSERT_EQUAL_UINT8(1, request.zone);
    TEST_ASSERT_TRUE(queue.pop(request));
    TEST_ASSERT_EQUAL_UINT8(2, request.zone);
    TEST_ASSERT_FALSE(queue.pop(request));
}

void test_rain_skip_self_expires() {
    irrigator::domain::RainSkip rainSkip;
    rainSkip.setSkipUntil(2000);
    TEST_ASSERT_TRUE(rainSkip.shouldSkipScheduledRun(1999));
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(2000));
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(2001));
}

void test_current_fault_alert_only_once_per_cycle() {
    irrigator::domain::CurrentFaultMonitor monitor({20, 800});
    monitor.beginActiveCycle();
    TEST_ASSERT_FALSE(monitor.checkAndConsumeAlert(200));
    TEST_ASSERT_TRUE(monitor.checkAndConsumeAlert(10));
    TEST_ASSERT_FALSE(monitor.checkAndConsumeAlert(10));
    monitor.endActiveCycle();
    monitor.beginActiveCycle();
    TEST_ASSERT_TRUE(monitor.checkAndConsumeAlert(900));
}

void test_zone_run_starts_pump_before_watering_then_gracefully_stops() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({1, 1, irrigator::domain::RunReason::Manual}, 1000));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::StartingPump),
                            static_cast<uint8_t>(machine.state()));

    machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Watering),
                            static_cast<uint8_t>(machine.state()));

    machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS + 60000UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpGracePeriod),
                            static_cast<uint8_t>(machine.state()));

    machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS + 60000UL +
                 irrigator::board::PUMP_GRACEFUL_SHUTDOWN_MS);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Idle),
                            static_cast<uint8_t>(machine.state()));
}

void test_pump_only_does_not_enter_grace_period() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({0, 1, irrigator::domain::RunReason::Manual}, 0));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpOnly),
                            static_cast<uint8_t>(machine.state()));
    machine.tick(60000UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Idle),
                            static_cast<uint8_t>(machine.state()));
}

void test_config_store_round_trips_valid_json() {
    const char* json =
        R"({"ssid":"home","password":"secret","mqtt_broker":"192.168.1.10","mqtt_port":1884,)"
        R"("mqtt_user":"u","mqtt_password":"p","timezone":2})";
    irrigator::domain::DeviceConfig config;
    TEST_ASSERT_TRUE(irrigator::domain::parseConfig(json, strlen(json), config));
    TEST_ASSERT_EQUAL_STRING("home", config.ssid);
    TEST_ASSERT_EQUAL_UINT16(1884, config.mqttPort);
    TEST_ASSERT_EQUAL_INT8(2, config.timezoneOffsetHours);

    char buffer[256];
    const size_t written = irrigator::domain::serializeConfig(config, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);

    irrigator::domain::DeviceConfig roundTripped;
    TEST_ASSERT_TRUE(irrigator::domain::parseConfig(buffer, written, roundTripped));
    TEST_ASSERT_EQUAL_STRING(config.ssid, roundTripped.ssid);
    TEST_ASSERT_EQUAL_UINT16(config.mqttPort, roundTripped.mqttPort);
}

void test_config_store_rejects_missing_required_fields() {
    irrigator::domain::DeviceConfig config;
    const char* missingSsid = R"({"mqtt_broker":"192.168.1.10"})";
    TEST_ASSERT_FALSE(irrigator::domain::parseConfig(missingSsid, strlen(missingSsid), config));

    const char* badTimezone = R"({"ssid":"home","mqtt_broker":"b","timezone":99})";
    TEST_ASSERT_FALSE(irrigator::domain::parseConfig(badTimezone, strlen(badTimezone), config));

    const char* malformed = "{not json";
    TEST_ASSERT_FALSE(irrigator::domain::parseConfig(malformed, strlen(malformed), config));
}

void test_settings_store_defaults_missing_zone_names() {
    const char* json = R"({"watering_percentage":80,"zone_names":{"1":"Grass"}})";
    irrigator::domain::Settings settings;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(json, strlen(json), settings));
    TEST_ASSERT_EQUAL_INT16(80, settings.wateringPercentage);
    TEST_ASSERT_EQUAL_STRING("Grass", settings.zoneNames[0]);
    TEST_ASSERT_EQUAL_STRING("Zone 2", settings.zoneNames[1]);
}

void test_settings_store_rejects_out_of_range_percentage() {
    const char* json = R"({"watering_percentage":999})";
    irrigator::domain::Settings settings;
    TEST_ASSERT_FALSE(irrigator::domain::parseSettings(json, strlen(json), settings));
}

void test_settings_store_round_trips() {
    irrigator::domain::Settings settings = irrigator::domain::makeDefaultSettings();
    settings.wateringPercentage = 120;
    settings.scheduleActive = false;

    char buffer[512];
    const size_t written = irrigator::domain::serializeSettings(settings, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);

    irrigator::domain::Settings roundTripped;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(buffer, written, roundTripped));
    TEST_ASSERT_EQUAL_INT16(120, roundTripped.wateringPercentage);
    TEST_ASSERT_FALSE(roundTripped.scheduleActive);
}

void test_schedule_store_round_trips_valid_list() {
    const char* json =
        R"([{"zone":1,"day_of_week":1,"start_time":"06:30","duration_min":20,"active":true}])";
    irrigator::domain::ScheduleList list;
    TEST_ASSERT_TRUE(irrigator::domain::parseSchedule(json, strlen(json), list));
    TEST_ASSERT_EQUAL_UINT8(1, list.count);
    TEST_ASSERT_EQUAL_UINT8(1, list.records[0].zone);

    char buffer[512];
    const size_t written = irrigator::domain::serializeSchedule(list, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);

    irrigator::domain::ScheduleList roundTripped;
    TEST_ASSERT_TRUE(irrigator::domain::parseSchedule(buffer, written, roundTripped));
    TEST_ASSERT_EQUAL_UINT8(1, roundTripped.count);
}

void test_schedule_store_rejects_whole_file_on_one_bad_record() {
    const char* json =
        R"([{"zone":1,"day_of_week":1,"start_time":"06:30","duration_min":20,"active":true},)"
        R"({"zone":12,"day_of_week":1,"start_time":"06:30","duration_min":20,"active":true}])";
    irrigator::domain::ScheduleList list;
    TEST_ASSERT_FALSE(irrigator::domain::parseSchedule(json, strlen(json), list));
}

namespace {

irrigator::domain::ScheduleList makeOneRecordSchedule() {
    irrigator::domain::ScheduleList list;
    list.records[0] = {1, 3, "06:30", 20, true};
    list.count = 1;
    return list;
}

}  // namespace

void test_schedule_engine_fires_matching_record_once_per_minute() {
    irrigator::domain::ScheduleEngine engine;
    const irrigator::domain::ScheduleList schedule = makeOneRecordSchedule();
    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t firstCount =
        engine.tick(now, 100000, schedule, true, noSkip, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(1, firstCount);
    TEST_ASSERT_EQUAL_UINT8(1, due[0].zone);
    TEST_ASSERT_EQUAL_UINT16(20, due[0].durationMin);

    const uint8_t secondCount =
        engine.tick(now, 100000, schedule, true, noSkip, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(0, secondCount);

    const irrigator::domain::WallClock later{3, 6, 31};
    const uint8_t thirdCount =
        engine.tick(later, 100060, schedule, true, noSkip, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(0, thirdCount);
}

void test_schedule_engine_respects_schedule_active_flag() {
    irrigator::domain::ScheduleEngine engine;
    const irrigator::domain::ScheduleList schedule = makeOneRecordSchedule();
    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t count =
        engine.tick(now, 200000, schedule, false, noSkip, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

void test_schedule_engine_respects_rain_skip() {
    irrigator::domain::ScheduleEngine engine;
    const irrigator::domain::ScheduleList schedule = makeOneRecordSchedule();
    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip skipping;
    skipping.setSkipUntil(9999999999LL);
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t count =
        engine.tick(now, 300000, schedule, true, skipping, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

void test_schedule_engine_applies_watering_percentage() {
    irrigator::domain::ScheduleEngine engine;
    const irrigator::domain::ScheduleList schedule = makeOneRecordSchedule();
    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t count =
        engine.tick(now, 400000, schedule, true, noSkip, 50, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_EQUAL_UINT16(10, due[0].durationMin);
}

void test_schedule_engine_skips_zero_duration_after_percentage() {
    irrigator::domain::ScheduleEngine engine;
    const irrigator::domain::ScheduleList schedule = makeOneRecordSchedule();
    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t count =
        engine.tick(now, 500000, schedule, true, noSkip, 0, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(0, count);
}

void test_ui_state_machine_wakes_on_input_and_shows_normal() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DisplayOff);

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1000);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_active_interaction_timeout_returns_to_display_off() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1000);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    ui.tick(1000 + irrigator::board::ACTIVE_INTERACTION_TIMEOUT_MS - 1);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    ui.tick(1000 + irrigator::board::ACTIVE_INTERACTION_TIMEOUT_MS);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DisplayOff);
}

void test_ui_state_machine_zone_and_duration_picker_flow_emits_start_watering() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.defaultManualDuration = 5;

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ZonePicker);
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingZone());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);
    TEST_ASSERT_EQUAL_UINT8(2, ui.pendingZone());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DurationPicker);
    TEST_ASSERT_EQUAL_UINT16(5, ui.pendingDuration());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 400);
    TEST_ASSERT_EQUAL_UINT16(6, ui.pendingDuration());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::StartWatering);
    TEST_ASSERT_EQUAL_UINT8(2, intent.zone);
    TEST_ASSERT_EQUAL_UINT16(6, intent.duration);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_long_click_cancels_duration_picker() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 100);  // zone=0, -> DurationPicker
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DurationPicker);

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 200);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_watering_active_ignores_ticks_but_long_click_opens_menu() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);  // ticks ignored

    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 200);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
}

void test_ui_state_machine_stop_confirm_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    TEST_ASSERT_EQUAL_UINT8(0, ui.selectedIndex());  // Stop is item 0 while watering

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);  // -> StopConfirm
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::StopConfirm);
    TEST_ASSERT_FALSE(ui.stopConfirmYesSelected());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);
    TEST_ASSERT_TRUE(ui.stopConfirmYesSelected());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::AbortWatering);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_settings_percentage_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringPercentage = 100;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSettings);

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter percentage edit
    TEST_ASSERT_TRUE(ui.editing());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);
    TEST_ASSERT_EQUAL_INT16(110, ui.pendingPercentage());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::SetWateringPercentage);
    TEST_ASSERT_EQUAL_INT16(110, intent.percentage);
    TEST_ASSERT_FALSE(ui.editing());
}

void test_ui_state_machine_schedule_duration_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 1;
    ctx.selectedScheduleDurationMin = 20;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuSchedule
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSchedule);

    // Item layout for one record: 0=zone,1=day,2=time,3=duration,4=active, 5=BACK.
    for (int i = 0; i < 3; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(irrigator::domain::UiStateMachine::kScheduleFieldDuration,
                            ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter duration edit
    TEST_ASSERT_TRUE(ui.editing());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);
    TEST_ASSERT_EQUAL_UINT16(23, ui.pendingDuration());  // adjustScheduledDuration(20, CW)

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::SetScheduleDuration);
    TEST_ASSERT_EQUAL_UINT8(0, intent.scheduleRecordIndex);
    TEST_ASSERT_EQUAL_UINT16(23, intent.duration);
}

void test_ui_state_machine_schedule_zone_edit_wraps() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 1;
    ctx.selectedScheduleZone = irrigator::board::ZONE_COUNT;  // 11: next CW tick should wrap to 1
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuSchedule
    // Item 0 for one record is the Zone field already.
    TEST_ASSERT_EQUAL_UINT8(irrigator::domain::UiStateMachine::kScheduleFieldZone, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);  // enter zone edit
    TEST_ASSERT_TRUE(ui.editing());
    TEST_ASSERT_EQUAL_UINT8(irrigator::board::ZONE_COUNT, ui.pendingScheduleZone());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 400);
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingScheduleZone());  // wrapped past 11 back to 1

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::SetScheduleZone);
    TEST_ASSERT_EQUAL_UINT8(0, intent.scheduleRecordIndex);
    TEST_ASSERT_EQUAL_UINT8(1, intent.zone);
    TEST_ASSERT_FALSE(ui.editing());
}

void test_ui_state_machine_schedule_day_edit_wraps() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 1;
    ctx.selectedScheduleDay = 7;  // Sunday: next CW tick should wrap to Monday (1)
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // Zone -> Day field
    TEST_ASSERT_EQUAL_UINT8(irrigator::domain::UiStateMachine::kScheduleFieldDay, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter day edit
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingScheduleDay());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::SetScheduleDay);
    TEST_ASSERT_EQUAL_UINT8(1, intent.dayOfWeek);
}

void test_ui_state_machine_schedule_time_edit_hour_then_minute() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 1;
    ctx.selectedScheduleHour = 23;
    ctx.selectedScheduleMinute = 59;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);
    for (int i = 0; i < 2; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);  // -> Time field
    }
    TEST_ASSERT_EQUAL_UINT8(irrigator::domain::UiStateMachine::kScheduleFieldTime, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter time edit (hour step)
    TEST_ASSERT_TRUE(ui.editing());
    TEST_ASSERT_FALSE(ui.editingScheduleTimeMinuteStep());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);
    TEST_ASSERT_EQUAL_UINT8(0, ui.pendingScheduleHour());  // wrapped past 23 to 0

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);  // confirm hour -> minute step
    TEST_ASSERT_TRUE(ui.editing());
    TEST_ASSERT_TRUE(ui.editingScheduleTimeMinuteStep());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 700);
    TEST_ASSERT_EQUAL_UINT8(0, ui.pendingScheduleMinute());  // wrapped past 59 to 0

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 800);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::SetScheduleTime);
    TEST_ASSERT_EQUAL_UINT8(0, intent.hour);
    TEST_ASSERT_EQUAL_UINT8(0, intent.minute);
    TEST_ASSERT_FALSE(ui.editing());
}

void test_calendar_math_round_trips_and_weekday() {
    const int64_t days = irrigator::domain::daysFromCivil(2024, 3, 31);
    const irrigator::domain::CivilDate civil = irrigator::domain::civilFromDays(days);
    TEST_ASSERT_EQUAL_INT(2024, civil.year);
    TEST_ASSERT_EQUAL_UINT(3, civil.month);
    TEST_ASSERT_EQUAL_UINT(31, civil.day);
    TEST_ASSERT_EQUAL_INT(0, irrigator::domain::weekdayFromDays(days));  // Sunday
}

void test_dst_rules_matches_known_2024_eu_transitions() {
    using irrigator::domain::epochFromUtc;
    using irrigator::domain::isEuDstActive;

    // EU DST 2024: forward March 31 01:00 UTC, back October 27 01:00 UTC (public record).
    TEST_ASSERT_FALSE(isEuDstActive(epochFromUtc(2024, 3, 31, 0, 59, 59)));
    TEST_ASSERT_TRUE(isEuDstActive(epochFromUtc(2024, 3, 31, 1, 0, 0)));
    TEST_ASSERT_TRUE(isEuDstActive(epochFromUtc(2024, 10, 27, 0, 59, 59)));
    TEST_ASSERT_FALSE(isEuDstActive(epochFromUtc(2024, 10, 27, 1, 0, 0)));

    TEST_ASSERT_TRUE(isEuDstActive(epochFromUtc(2024, 6, 15, 12, 0, 0)));
    TEST_ASSERT_FALSE(isEuDstActive(epochFromUtc(2024, 1, 15, 12, 0, 0)));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_manual_duration_adjusts_by_formula_and_clamps);
    RUN_TEST(test_scheduled_duration_uses_schedule_clamp);
    RUN_TEST(test_watering_percentage_rounds_to_nearest_minute);
    RUN_TEST(test_start_time_validation);
    RUN_TEST(test_schedule_record_validation);
    RUN_TEST(test_watering_queue_capacity_and_fifo);
    RUN_TEST(test_rain_skip_self_expires);
    RUN_TEST(test_current_fault_alert_only_once_per_cycle);
    RUN_TEST(test_zone_run_starts_pump_before_watering_then_gracefully_stops);
    RUN_TEST(test_pump_only_does_not_enter_grace_period);
    RUN_TEST(test_config_store_round_trips_valid_json);
    RUN_TEST(test_config_store_rejects_missing_required_fields);
    RUN_TEST(test_settings_store_defaults_missing_zone_names);
    RUN_TEST(test_settings_store_rejects_out_of_range_percentage);
    RUN_TEST(test_settings_store_round_trips);
    RUN_TEST(test_schedule_store_round_trips_valid_list);
    RUN_TEST(test_schedule_store_rejects_whole_file_on_one_bad_record);
    RUN_TEST(test_schedule_engine_fires_matching_record_once_per_minute);
    RUN_TEST(test_schedule_engine_respects_schedule_active_flag);
    RUN_TEST(test_schedule_engine_respects_rain_skip);
    RUN_TEST(test_schedule_engine_applies_watering_percentage);
    RUN_TEST(test_schedule_engine_skips_zero_duration_after_percentage);
    RUN_TEST(test_ui_state_machine_wakes_on_input_and_shows_normal);
    RUN_TEST(test_ui_state_machine_active_interaction_timeout_returns_to_display_off);
    RUN_TEST(test_ui_state_machine_zone_and_duration_picker_flow_emits_start_watering);
    RUN_TEST(test_ui_state_machine_long_click_cancels_duration_picker);
    RUN_TEST(test_ui_state_machine_watering_active_ignores_ticks_but_long_click_opens_menu);
    RUN_TEST(test_ui_state_machine_stop_confirm_flow);
    RUN_TEST(test_ui_state_machine_settings_percentage_edit);
    RUN_TEST(test_ui_state_machine_schedule_duration_edit);
    RUN_TEST(test_ui_state_machine_schedule_zone_edit_wraps);
    RUN_TEST(test_ui_state_machine_schedule_day_edit_wraps);
    RUN_TEST(test_ui_state_machine_schedule_time_edit_hour_then_minute);
    RUN_TEST(test_calendar_math_round_trips_and_weekday);
    RUN_TEST(test_dst_rules_matches_known_2024_eu_transitions);
    return UNITY_END();
}