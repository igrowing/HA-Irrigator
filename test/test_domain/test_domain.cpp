#include <unity.h>

#include <string.h>

#include "../../src/domain/AutoAdjustment.h"
#include "../../src/domain/CalendarMath.h"
#include "../../src/domain/WeatherSummary.h"
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
#include "../../src/domain/WifiQuality.h"
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

void test_auto_adjustment_percentage_base_case_is_100() {
    TEST_ASSERT_EQUAL_INT16(
        100, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 0.0f, 20.0f, 2.0f, 5.0f, 40, 200));
}

void test_auto_adjustment_percentage_temp_above_base_increases_it() {
    // (25-20)*2 = +10 -> 110%
    TEST_ASSERT_EQUAL_INT16(
        110, irrigator::domain::computeAutoCorrectedPercentage(25.0f, 0.0f, 20.0f, 2.0f, 5.0f, 40, 200));
}

void test_auto_adjustment_percentage_rain_reduces_it() {
    // 100 - 4mm*5 = 80%
    TEST_ASSERT_EQUAL_INT16(
        80, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 4.0f, 20.0f, 2.0f, 5.0f, 40, 200));
}

void test_auto_adjustment_percentage_clamps_to_upper_limit() {
    // (80-20)*2 = +120 -> 220%, over the 200% limit -> clamped to 200.
    TEST_ASSERT_EQUAL_INT16(
        200, irrigator::domain::computeAutoCorrectedPercentage(80.0f, 0.0f, 20.0f, 2.0f, 5.0f, 40, 200));
}

void test_auto_adjustment_percentage_below_lower_limit_sets_zero() {
    // 100 - 15mm*5 = 25%, below the 40% lower limit -> 0 (skip), not clamped to 40.
    TEST_ASSERT_EQUAL_INT16(
        0, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 15.0f, 20.0f, 2.0f, 5.0f, 40, 200));
}

void test_should_skip_for_rain_probability_inclusive_amount_exclusive() {
    TEST_ASSERT_FALSE(irrigator::domain::shouldSkipForRain(69.9f, 10.0f, 70, 4));  // Probability under 70%.
    TEST_ASSERT_TRUE(irrigator::domain::shouldSkipForRain(70.0f, 4.1f, 70, 4));    // Probability >=70% now counts.
    TEST_ASSERT_FALSE(irrigator::domain::shouldSkipForRain(70.0f, 4.0f, 70, 4));   // Amount exactly 4.0mm still doesn't.
    TEST_ASSERT_TRUE(irrigator::domain::shouldSkipForRain(100.0f, 4.1f, 70, 4));
}

void test_should_skip_for_rain_uses_configured_thresholds() {
    // Same inputs, different (lower) configured thresholds -> now triggers where it didn't above.
    TEST_ASSERT_TRUE(irrigator::domain::shouldSkipForRain(50.0f, 2.5f, 50, 2));
    TEST_ASSERT_FALSE(irrigator::domain::shouldSkipForRain(50.0f, 2.5f, 70, 4));
}

void test_open_meteo_now_index_is_days_back_times_24_plus_hour() {
    const int64_t epoch = irrigator::domain::epochFromUtc(2026, 7, 28, 14, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(5 * 24 + 14, static_cast<uint32_t>(irrigator::domain::openMeteoNowIndex(epoch, 5)));
}

void test_open_meteo_now_index_at_midnight() {
    const int64_t epoch = irrigator::domain::epochFromUtc(2026, 7, 28, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(5 * 24, static_cast<uint32_t>(irrigator::domain::openMeteoNowIndex(epoch, 5)));
}

void test_summarize_past_average_temperature_averages_trailing_window() {
    JsonDocument doc;
    JsonArray temp = doc["t"].to<JsonArray>();
    for (int i = 0; i < 30; ++i) {
        temp.add(10.0f + i);  // 10, 11, 12, ... 39
    }
    float avgTemp = 0.0f;
    // nowIndex=24, pastWeatherDays=1 -> window is indices [0,24).
    const bool ok = irrigator::domain::summarizePastAverageTemperature(temp, 24, 1, avgTemp);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.5f, avgTemp);  // avg of 10..33 = 21.5
}

void test_summarize_past_average_temperature_rejects_insufficient_index() {
    JsonDocument doc;
    JsonArray temp = doc["t"].to<JsonArray>();
    for (int i = 0; i < 10; ++i) {
        temp.add(20.0f);
    }
    float avgTemp = 0.0f;
    // nowIndex=10 is less than the 24-hour window pastWeatherDays=1 needs before it.
    TEST_ASSERT_FALSE(irrigator::domain::summarizePastAverageTemperature(temp, 10, 1, avgTemp));
}

void test_summarize_past_average_temperature_rejects_short_array() {
    JsonDocument doc;
    JsonArray temp = doc["t"].to<JsonArray>();
    for (int i = 0; i < 20; ++i) {
        temp.add(20.0f);  // only 20 entries
    }
    float avgTemp = 0.0f;
    // nowIndex=24 has enough index headroom, but the array itself is too short to cover [0,24).
    TEST_ASSERT_FALSE(irrigator::domain::summarizePastAverageTemperature(temp, 24, 1, avgTemp));
}

void test_summarize_past_rainfall_sums_first_n_days() {
    JsonDocument doc;
    JsonArray daily = doc["d"].to<JsonArray>();
    for (int i = 0; i < 7; ++i) {
        daily.add(2.0f);  // 2mm/day, 7 days available
    }
    float rainPast = 0.0f;
    const bool ok = irrigator::domain::summarizePastRainfall(daily, 5, rainPast);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, rainPast);  // only the first 5 days: 5*2mm = 10mm
}

void test_summarize_past_rainfall_rejects_short_array() {
    JsonDocument doc;
    JsonArray daily = doc["d"].to<JsonArray>();
    daily.add(1.0f);  // only 1 day; 5 requested
    float rainPast = 0.0f;
    TEST_ASSERT_FALSE(irrigator::domain::summarizePastRainfall(daily, 5, rainPast));
}

void test_summarize_forecast_rain_window_computes_max_and_sum() {
    JsonDocument doc;
    JsonArray prob = doc["p"].to<JsonArray>();
    JsonArray precip = doc["r"].to<JsonArray>();
    for (int i = 0; i < 24; ++i) {
        prob.add(static_cast<float>(i));  // 0, 1, 2, ...
        precip.add(1.0f);                 // constant 1mm/hour
    }
    float maxProb = 0.0f, rainWindow = 0.0f;
    const bool ok = irrigator::domain::summarizeForecastRainWindow(prob, precip, 2, 3, maxProb, rainWindow);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, maxProb);     // indices 2,3,4 -> probs 2,3,4 -> max 4
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.0f, rainWindow);  // 3 hours * 1mm
}

void test_summarize_forecast_rain_window_rejects_short_array() {
    JsonDocument doc;
    JsonArray prob = doc["p"].to<JsonArray>();
    prob.add(1.0f);
    JsonArray precip = doc["r"].to<JsonArray>();
    float maxProb = 0.0f, rainWindow = 0.0f;
    TEST_ASSERT_FALSE(irrigator::domain::summarizeForecastRainWindow(prob, precip, 0, 12, maxProb, rainWindow));
}

void test_summarize_weather_api_forecast_rain_window_matches_from_now_epoch() {
    JsonDocument doc;
    JsonArray days = doc.to<JsonArray>();
    JsonObject day0 = days.add<JsonObject>();
    JsonArray hours0 = day0["hour"].to<JsonArray>();
    for (int i = 0; i < 24; ++i) {
        JsonObject h = hours0.add<JsonObject>();
        h["time_epoch"] = 1000 + i * 3600;
        h["chance_of_rain"] = i;
        h["precip_mm"] = 0.5f;
    }

    float maxProb = 0.0f, rain = 0.0f;
    const bool ok = irrigator::domain::summarizeWeatherApiForecastRainWindow(
        days, 1000 + 5 * 3600, 3, maxProb, rain);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.0f, maxProb);  // chance_of_rain 5,6,7 -> max 7
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, rain);     // 3 hours * 0.5mm
}

void test_summarize_weather_api_forecast_rain_window_fails_when_not_enough_hours() {
    JsonDocument doc;
    JsonArray days = doc.to<JsonArray>();
    JsonObject day0 = days.add<JsonObject>();
    JsonArray hours0 = day0["hour"].to<JsonArray>();
    JsonObject h = hours0.add<JsonObject>();
    h["time_epoch"] = 1000;
    h["chance_of_rain"] = 0;
    h["precip_mm"] = 0.0f;

    float maxProb = 0.0f, rain = 0.0f;
    TEST_ASSERT_FALSE(irrigator::domain::summarizeWeatherApiForecastRainWindow(days, 1000, 12, maxProb, rain));
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

// void test_zone_run_starts_pump_before_watering_then_gracefully_stops() {
//     irrigator::domain::IrrigationStateMachine machine;
//     TEST_ASSERT_TRUE(machine.start({1, 1, irrigator::domain::RunReason::Manual}, 1000));
//     TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::StartingPump),
//                             static_cast<uint8_t>(machine.state()));

//     machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS);
//     TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Watering),
//                             static_cast<uint8_t>(machine.state()));

//     machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS + 60000UL);
//     TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpGracePeriod),
//                             static_cast<uint8_t>(machine.state()));

//     machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS + 60000UL +
//                  irrigator::board::PUMP_GRACEFUL_SHUTDOWN_MS);
//     TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Idle),
//                             static_cast<uint8_t>(machine.state()));
// }

void test_pump_only_does_not_enter_grace_period() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({0, 1, irrigator::domain::RunReason::Manual}, 0));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpOnly),
                            static_cast<uint8_t>(machine.state()));
    machine.tick(60000UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Idle),
                            static_cast<uint8_t>(machine.state()));
}

void test_abort_while_watering_still_gives_pump_its_graceful_shutdown() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({1, 30, irrigator::domain::RunReason::Manual}, 1000));
    machine.tick(1000 + irrigator::board::PUMP_PRESTART_MS);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Watering),
                            static_cast<uint8_t>(machine.state()));

    const uint32_t abortMs = 1000 + irrigator::board::PUMP_PRESTART_MS + 5000UL;
    machine.abort(abortMs);
    // Valve closes (implied by leaving Watering) immediately, but the pump doesn't — same
    // PumpGracePeriod a natural end-of-duration completion would produce.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpGracePeriod),
                            static_cast<uint8_t>(machine.state()));
    TEST_ASSERT_TRUE(machine.isActive());

    machine.tick(abortMs + irrigator::board::PUMP_GRACEFUL_SHUTDOWN_MS - 1);
    TEST_ASSERT_TRUE(machine.isActive());  // still winding down
    machine.tick(abortMs + irrigator::board::PUMP_GRACEFUL_SHUTDOWN_MS);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Idle),
                            static_cast<uint8_t>(machine.state()));
}

// void test_abort_during_pump_prestart_also_gives_pump_its_graceful_shutdown() {
//     irrigator::domain::IrrigationStateMachine machine;
//     TEST_ASSERT_TRUE(machine.start({1, 30, irrigator::domain::RunReason::Manual}, 1000));
//     TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::StartingPump),
//                             static_cast<uint8_t>(machine.state()));
// 
//     // Aborted before the valve ever opened — the pump was still commanded on, so it still gets
//     // the graceful tail rather than being cut immediately.
//     machine.abort(1200);
//     TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpGracePeriod),
//                             static_cast<uint8_t>(machine.state()));
// }

void test_zone_watering_survives_large_uptime_start_time() {
    // Regression: wateringStartedAtMs_ must be stamped with nowMs, not left at 0, or elapsed
    // time is computed from device boot (millis()) instead of from watering start — on real
    // hardware nowMs is already large by the time a run starts, so the run looks instantly
    // "expired" and jumps straight to PumpGracePeriod without ever watering.
    irrigator::domain::IrrigationStateMachine machine;
    const uint32_t nowMs = 3600000UL;  // 1 hour of uptime before this run starts
    TEST_ASSERT_TRUE(machine.start({1, 3, irrigator::domain::RunReason::Manual}, nowMs));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Watering),
                            static_cast<uint8_t>(machine.state()));

    const auto shot = machine.snapshot(nowMs);
    TEST_ASSERT_EQUAL_UINT32(3UL * 60UL * 1000UL, shot.remainingMs);

    machine.tick(nowMs + 60000UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Watering),
                            static_cast<uint8_t>(machine.state()));
}

void test_abort_pump_only_stops_immediately() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({0, 30, irrigator::domain::RunReason::Manual}, 0));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpOnly),
                            static_cast<uint8_t>(machine.state()));

    machine.abort(5000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::Idle),
                            static_cast<uint8_t>(machine.state()));
    TEST_ASSERT_FALSE(machine.isActive());
}

void test_abort_during_existing_grace_period_cuts_it_short() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({1, 1, irrigator::domain::RunReason::Manual}, 0));
    machine.tick(irrigator::board::PUMP_PRESTART_MS);
    machine.tick(irrigator::board::PUMP_PRESTART_MS + 60000UL);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(irrigator::domain::IrrigationState::PumpGracePeriod),
                            static_cast<uint8_t>(machine.state()));

    // Explicitly stopping while the pump is already just lingering from a prior end cuts that
    // tail short instead of re-arming a fresh one.
    machine.abort(irrigator::board::PUMP_PRESTART_MS + 60000UL + 1000UL);
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

void test_config_store_allows_mqtt_fields_blank() {
    const char* json = R"({"ssid":"home","password":"secret","timezone":2})";
    irrigator::domain::DeviceConfig config;
    TEST_ASSERT_TRUE(irrigator::domain::parseConfig(json, strlen(json), config));
    TEST_ASSERT_EQUAL_STRING("home", config.ssid);
    TEST_ASSERT_EQUAL_STRING("", config.mqttBroker);
    TEST_ASSERT_EQUAL_STRING("", config.mqttUser);
    TEST_ASSERT_EQUAL_STRING("", config.mqttPassword);

    // A broker given without a usable port is still rejected -- MQTT being optional doesn't
    // relax validation once the caller opts into it.
    const char* brokerWithoutPort = R"({"ssid":"home","mqtt_broker":"192.168.1.10","mqtt_port":0})";
    TEST_ASSERT_FALSE(
        irrigator::domain::parseConfig(brokerWithoutPort, strlen(brokerWithoutPort), config));
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
    irrigator::domain::Settings settings = irrigator::domain::makeDefaultZones();
    settings.wateringPercentage = 120;
    settings.scheduleActive = false;

    char buffer[irrigator::board::SETTINGS_JSON_MAX_BYTES];
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

void test_ui_state_machine_zone_picker_skips_disabled_zones() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.zoneEnabled[1] = false;   // zone 2 disabled
    ctx.zoneEnabled[10] = false;  // zone 11 disabled

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingZone());

    // Ticking forward from zone 1 must skip disabled zone 2 and land on zone 3.
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);
    TEST_ASSERT_EQUAL_UINT8(3, ui.pendingZone());

    // Entering the picker counter-clockwise must skip disabled zone 11 and land on zone 10.
    irrigator::domain::UiStateMachine ui2;
    ui2.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui2.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 100);
    TEST_ASSERT_EQUAL_UINT8(10, ui2.pendingZone());

    // Ticking backward past zone 1 wraps through PUMP and skips disabled zone 11.
    ui2.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 200);  // -> 9
    ui2.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 300);  // -> 8
    for (int i = 0; i < 7; ++i) {
        ui2.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 400 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(0, ui2.pendingZone());  // PUMP
    ui2.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 500);
    TEST_ASSERT_EQUAL_UINT8(10, ui2.pendingZone());  // skipped disabled zone 11, landed on 10
}

void test_ui_state_machine_zone_picker_falls_back_to_pump_when_all_zones_disabled() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    for (bool& enabled : ctx.zoneEnabled) {
        enabled = false;
    }

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_EQUAL_UINT8(0, ui.pendingZone());  // no enabled zone reachable -> PUMP
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
    TEST_ASSERT_FALSE(ui.stopAllPending());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);
    TEST_ASSERT_TRUE(ui.stopConfirmYesSelected());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::AbortWatering);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_stop_all_requires_confirmation_then_stops_and_clears() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Stop -> Stop all
    TEST_ASSERT_EQUAL_UINT8(1, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);  // -> StopConfirm
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::StopConfirm);
    TEST_ASSERT_FALSE(ui.stopConfirmYesSelected());
    TEST_ASSERT_TRUE(ui.stopAllPending());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 400);
    TEST_ASSERT_TRUE(ui.stopConfirmYesSelected());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::StopAllWatering);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
    TEST_ASSERT_FALSE(ui.stopAllPending());  // cleared, so a later plain Stop isn't misrouted
}

void test_ui_state_machine_stop_all_confirm_no_cancels_without_intent() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Stop -> Stop all
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> StopConfirm

    // NO stays selected by default; confirming without toggling to YES must not stop anything.
    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_settings_percentage_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringPercentage = 100;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
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

void test_ui_state_machine_settings_upper_lower_limit_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.upperLimitPercent = 200;
    ctx.lowerLimitPercent = 40;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350);  // Percentage -> Upper limit
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);      // enter upper-limit edit
    TEST_ASSERT_TRUE(ui.editing());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 450);
    TEST_ASSERT_EQUAL_INT16(210, ui.pendingUpperLimit());
    const irrigator::domain::UiIntent upperIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(upperIntent.type == irrigator::domain::UiIntentType::SetUpperLimit);
    TEST_ASSERT_EQUAL_INT16(210, upperIntent.percentage);
    TEST_ASSERT_FALSE(ui.editing());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 550);  // Upper limit -> Lower limit
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);      // enter lower-limit edit
    ui.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 650);
    TEST_ASSERT_EQUAL_INT16(30, ui.pendingLowerLimit());
    const irrigator::domain::UiIntent lowerIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 700);
    TEST_ASSERT_TRUE(lowerIntent.type == irrigator::domain::UiIntentType::SetLowerLimit);
    TEST_ASSERT_EQUAL_INT16(30, lowerIntent.percentage);
}

void test_ui_state_machine_settings_rain_skip_threshold_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.rainSkipProbabilityPercent = 70;
    ctx.rainSkipAmountMm = 4;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings

    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexRainSkipProbability; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter rain-skip-probability edit
    TEST_ASSERT_TRUE(ui.editing());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 450);
    TEST_ASSERT_EQUAL_INT16(80, ui.pendingRainSkipProbability());
    const irrigator::domain::UiIntent probIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(probIntent.type == irrigator::domain::UiIntentType::SetRainSkipProbability);
    TEST_ASSERT_EQUAL_INT16(80, probIntent.percentage);
    TEST_ASSERT_FALSE(ui.editing());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 550);  // -> rain-skip amount
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);      // enter rain-skip-amount edit
    ui.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 650);
    TEST_ASSERT_EQUAL_INT16(3, ui.pendingRainSkipAmount());  // Amount steps by 1, not 10.
    const irrigator::domain::UiIntent amountIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 700);
    TEST_ASSERT_TRUE(amountIntent.type == irrigator::domain::UiIntentType::SetRainSkipAmount);
    TEST_ASSERT_EQUAL_INT16(3, amountIntent.percentage);
}

void test_ui_state_machine_settings_restart_confirm_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSettings);

    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexRestart; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 400 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(irrigator::domain::UiStateMachine::kSettingsIndexRestart, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);  // -> RestartConfirm
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::RestartConfirm);
    TEST_ASSERT_FALSE(ui.restartConfirmYesSelected());

    // NO stays selected by default; confirming without toggling to YES must not restart.
    const irrigator::domain::UiIntent noIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);
    TEST_ASSERT_TRUE(noIntent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    // Re-enter and confirm with YES this time.
    ui.wake(700);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 800);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 900);
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 950);
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1000);  // -> MenuSettings
    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexRestart; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1100 + i);
    }
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1200);  // -> RestartConfirm
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1300);
    TEST_ASSERT_TRUE(ui.restartConfirmYesSelected());

    const irrigator::domain::UiIntent yesIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1400);
    TEST_ASSERT_TRUE(yesIntent.type == irrigator::domain::UiIntentType::RestartDevice);
}

void test_ui_state_machine_test_all_zones_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);      // -> MenuTestAll
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuTestAll);
    TEST_ASSERT_EQUAL_UINT16(5, ui.testAllZonesDurationMin());  // default

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);  // Run -> Dur./zone
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);      // enter duration edit
    TEST_ASSERT_TRUE(ui.editing());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 700);
    TEST_ASSERT_EQUAL_UINT16(6, ui.pendingDuration());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 800);  // confirm duration
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_EQUAL_UINT16(6, ui.testAllZonesDurationMin());

    ui.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 900);  // Dur./zone -> Run
    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1000);  // Run
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::RunAllZonesTest);
    TEST_ASSERT_EQUAL_UINT16(6, intent.duration);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
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

void test_wifi_quality_thresholds_and_bar_count() {
    using irrigator::domain::toString;
    using irrigator::domain::wifiQualityBarCount;
    using irrigator::domain::wifiQualityFromRssi;
    using irrigator::domain::WifiQuality;

    TEST_ASSERT_TRUE(WifiQuality::Excellent == wifiQualityFromRssi(-40));
    TEST_ASSERT_TRUE(WifiQuality::Excellent == wifiQualityFromRssi(-55));
    TEST_ASSERT_TRUE(WifiQuality::Good == wifiQualityFromRssi(-56));
    TEST_ASSERT_TRUE(WifiQuality::Good == wifiQualityFromRssi(-65));
    TEST_ASSERT_TRUE(WifiQuality::Fair == wifiQualityFromRssi(-66));
    TEST_ASSERT_TRUE(WifiQuality::Fair == wifiQualityFromRssi(-75));
    TEST_ASSERT_TRUE(WifiQuality::Poor == wifiQualityFromRssi(-76));
    TEST_ASSERT_TRUE(WifiQuality::Poor == wifiQualityFromRssi(-85));
    TEST_ASSERT_TRUE(WifiQuality::Miserable == wifiQualityFromRssi(-86));
    TEST_ASSERT_TRUE(WifiQuality::Miserable == wifiQualityFromRssi(-127));

    TEST_ASSERT_EQUAL_STRING("excellent", toString(WifiQuality::Excellent));
    TEST_ASSERT_EQUAL_STRING("good", toString(WifiQuality::Good));
    TEST_ASSERT_EQUAL_STRING("fair", toString(WifiQuality::Fair));
    TEST_ASSERT_EQUAL_STRING("poor", toString(WifiQuality::Poor));
    TEST_ASSERT_EQUAL_STRING("miserable", toString(WifiQuality::Miserable));

    TEST_ASSERT_EQUAL_UINT8(4, wifiQualityBarCount(WifiQuality::Excellent));
    TEST_ASSERT_EQUAL_UINT8(0, wifiQualityBarCount(WifiQuality::Miserable));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_manual_duration_adjusts_by_formula_and_clamps);
    RUN_TEST(test_scheduled_duration_uses_schedule_clamp);
    RUN_TEST(test_watering_percentage_rounds_to_nearest_minute);
    RUN_TEST(test_auto_adjustment_percentage_base_case_is_100);
    RUN_TEST(test_auto_adjustment_percentage_temp_above_base_increases_it);
    RUN_TEST(test_auto_adjustment_percentage_rain_reduces_it);
    RUN_TEST(test_auto_adjustment_percentage_clamps_to_upper_limit);
    RUN_TEST(test_auto_adjustment_percentage_below_lower_limit_sets_zero);
    RUN_TEST(test_should_skip_for_rain_probability_inclusive_amount_exclusive);
    RUN_TEST(test_should_skip_for_rain_uses_configured_thresholds);
    RUN_TEST(test_open_meteo_now_index_is_days_back_times_24_plus_hour);
    RUN_TEST(test_open_meteo_now_index_at_midnight);
    RUN_TEST(test_summarize_past_average_temperature_averages_trailing_window);
    RUN_TEST(test_summarize_past_average_temperature_rejects_insufficient_index);
    RUN_TEST(test_summarize_past_average_temperature_rejects_short_array);
    RUN_TEST(test_summarize_past_rainfall_sums_first_n_days);
    RUN_TEST(test_summarize_past_rainfall_rejects_short_array);
    RUN_TEST(test_summarize_forecast_rain_window_computes_max_and_sum);
    RUN_TEST(test_summarize_forecast_rain_window_rejects_short_array);
    RUN_TEST(test_summarize_weather_api_forecast_rain_window_matches_from_now_epoch);
    RUN_TEST(test_summarize_weather_api_forecast_rain_window_fails_when_not_enough_hours);
    RUN_TEST(test_start_time_validation);
    RUN_TEST(test_schedule_record_validation);
    RUN_TEST(test_watering_queue_capacity_and_fifo);
    RUN_TEST(test_rain_skip_self_expires);
    RUN_TEST(test_current_fault_alert_only_once_per_cycle);
    // RUN_TEST(test_zone_run_starts_pump_before_watering_then_gracefully_stops);
    RUN_TEST(test_pump_only_does_not_enter_grace_period);
    RUN_TEST(test_abort_while_watering_still_gives_pump_its_graceful_shutdown);
    RUN_TEST(test_zone_watering_survives_large_uptime_start_time);
    // RUN_TEST(test_abort_during_pump_prestart_also_gives_pump_its_graceful_shutdown);
    RUN_TEST(test_abort_pump_only_stops_immediately);
    RUN_TEST(test_abort_during_existing_grace_period_cuts_it_short);
    RUN_TEST(test_config_store_round_trips_valid_json);
    RUN_TEST(test_config_store_rejects_missing_required_fields);
    RUN_TEST(test_config_store_allows_mqtt_fields_blank);
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
    RUN_TEST(test_ui_state_machine_zone_picker_skips_disabled_zones);
    RUN_TEST(test_ui_state_machine_zone_picker_falls_back_to_pump_when_all_zones_disabled);
    RUN_TEST(test_ui_state_machine_long_click_cancels_duration_picker);
    RUN_TEST(test_ui_state_machine_watering_active_ignores_ticks_but_long_click_opens_menu);
    RUN_TEST(test_ui_state_machine_stop_confirm_flow);
    RUN_TEST(test_ui_state_machine_stop_all_requires_confirmation_then_stops_and_clears);
    RUN_TEST(test_ui_state_machine_stop_all_confirm_no_cancels_without_intent);
    RUN_TEST(test_ui_state_machine_settings_percentage_edit);
    RUN_TEST(test_ui_state_machine_settings_upper_lower_limit_edit);
    RUN_TEST(test_ui_state_machine_settings_rain_skip_threshold_edit);
    RUN_TEST(test_ui_state_machine_settings_restart_confirm_flow);
    RUN_TEST(test_ui_state_machine_test_all_zones_flow);
    RUN_TEST(test_ui_state_machine_schedule_duration_edit);
    RUN_TEST(test_ui_state_machine_schedule_zone_edit_wraps);
    RUN_TEST(test_ui_state_machine_schedule_day_edit_wraps);
    RUN_TEST(test_ui_state_machine_schedule_time_edit_hour_then_minute);
    RUN_TEST(test_calendar_math_round_trips_and_weekday);
    RUN_TEST(test_wifi_quality_thresholds_and_bar_count);
    RUN_TEST(test_dst_rules_matches_known_2024_eu_transitions);
    return UNITY_END();
}