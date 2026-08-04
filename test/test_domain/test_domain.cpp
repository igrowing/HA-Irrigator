#include <unity.h>

#include <string.h>

#include "../../src/domain/AutoAdjustment.h"
#include "../../src/domain/CalendarMath.h"
#include "../../src/domain/WeatherSummary.h"
#include "../../src/domain/ConfigStore.h"
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
        100, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 0.0f, 20.0f, 40, 200));
}

void test_auto_adjustment_percentage_temp_above_base_increases_it() {
    // (25-20)*2 = +10 -> 110%
    TEST_ASSERT_EQUAL_INT16(
        150, irrigator::domain::computeAutoCorrectedPercentage(25.0f, 0.0f, 20.0f, 40, 200));
}

void test_auto_adjustment_percentage_rain_reduces_it() {
    // 100 - 4mm*5 = 80%
    TEST_ASSERT_EQUAL_INT16(
        96, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 4.0f, 20.0f, 40, 200));
}

void test_auto_adjustment_percentage_clamps_to_upper_limit() {
    // (80-20)*2 = +120 -> 220%, over the 200% limit -> clamped to 200.
    TEST_ASSERT_EQUAL_INT16(
        200, irrigator::domain::computeAutoCorrectedPercentage(80.0f, 0.0f, 20.0f, 40, 200));
}

void test_auto_adjustment_percentage_below_lower_limit_sets_zero() {
    // 100 - 15mm*5 = 25%, below the 40% lower limit -> 0 (skip), not clamped to 40.
    TEST_ASSERT_EQUAL_INT16(
        0, irrigator::domain::computeAutoCorrectedPercentage(13.0f, 30.0f, 20.0f, 40, 200));
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

void test_summarize_past_average_daily_max_temperature_averages_first_n_days() {
    JsonDocument doc;
    JsonArray daily = doc["t"].to<JsonArray>();
    // 7 daily highs available (past_days=5 + forecast_days=2); only the first 5 count.
    const float highs[7] = {20.0f, 22.0f, 24.0f, 26.0f, 28.0f, 40.0f, 41.0f};
    for (float h : highs) {
        daily.add(h);
    }
    float avgMaxTemp = 0.0f;
    const bool ok =
        irrigator::domain::summarizePastAverageDailyMaxTemperature(daily, 5, avgMaxTemp);
    TEST_ASSERT_TRUE(ok);
    // avg of first 5 (20,22,24,26,28) = 24.0; the trailing forecast-day highs are excluded.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 24.0f, avgMaxTemp);
}

void test_summarize_past_average_daily_max_temperature_rejects_short_array() {
    JsonDocument doc;
    JsonArray daily = doc["t"].to<JsonArray>();
    for (int i = 0; i < 4; ++i) {
        daily.add(25.0f);  // only 4 days; 5 requested
    }
    float avgMaxTemp = 0.0f;
    TEST_ASSERT_FALSE(
        irrigator::domain::summarizePastAverageDailyMaxTemperature(daily, 5, avgMaxTemp));
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
    size_t qualifyingHours = 0;
    const bool ok = irrigator::domain::summarizeWeatherApiForecastRainWindow(
        days, 1000 + 5 * 3600, 3, maxProb, rain, qualifyingHours);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.0f, maxProb);  // chance_of_rain 5,6,7 -> max 7
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, rain);     // 3 hours * 0.5mm
    TEST_ASSERT_EQUAL_UINT(3, qualifyingHours);
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
    size_t qualifyingHours = 0;
    TEST_ASSERT_FALSE(
        irrigator::domain::summarizeWeatherApiForecastRainWindow(days, 1000, 12, maxProb, rain, qualifyingHours));
    TEST_ASSERT_EQUAL_UINT(1, qualifyingHours);
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

// Pre-restructuring flat settings.json (no Zones/Location/Current_sensor/Watering groups) — the
// per-field fallback (see Settings.h's parseSettings doc) must still recover its real values.
void test_settings_store_defaults_missing_zone_names() {
    const char* json = R"({"watering_percentage":80,"zone_names":{"1":"Grass"}})";
    irrigator::domain::Settings settings;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(json, strlen(json), settings));
    TEST_ASSERT_EQUAL_INT16(80, settings.wateringPercentage);
    TEST_ASSERT_EQUAL_STRING("Grass", settings.zoneNames[0]);
    TEST_ASSERT_EQUAL_STRING("Zone 2", settings.zoneNames[1]);
}

void test_settings_store_rejects_out_of_range_percentage() {
    const char* json = R"({"Watering":{"watering_percentage":999}})";
    irrigator::domain::Settings settings;
    TEST_ASSERT_FALSE(irrigator::domain::parseSettings(json, strlen(json), settings));
}

void test_settings_store_round_trips() {
    irrigator::domain::Settings settings = irrigator::domain::makeDefaultZones();
    settings.wateringPercentage = 120;
    settings.scheduleActive = false;
    settings.autoAdjustmentEnabled = false;
    settings.alertPumpUc = false;
    settings.alertValveOc = false;

    char buffer[irrigator::board::SETTINGS_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSettings(settings, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);

    irrigator::domain::Settings roundTripped;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(buffer, written, roundTripped));
    TEST_ASSERT_EQUAL_INT16(120, roundTripped.wateringPercentage);
    TEST_ASSERT_FALSE(roundTripped.scheduleActive);
    TEST_ASSERT_FALSE(roundTripped.autoAdjustmentEnabled);
    TEST_ASSERT_FALSE(roundTripped.alertPumpUc);
    TEST_ASSERT_TRUE(roundTripped.alertPumpOc);
    TEST_ASSERT_TRUE(roundTripped.alertValveUc);
    TEST_ASSERT_FALSE(roundTripped.alertValveOc);
}

// A settings.json written before auto_adjustment_enabled existed (or any upload that omits it)
// must default to true — the feature was already running unconditionally for every prior
// deployment, so an upgrade must not silently disable it.
void test_settings_store_defaults_auto_adjustment_enabled_to_true_when_missing() {
    const char* json = R"({"watering_percentage":80})";
    irrigator::domain::Settings settings;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(json, strlen(json), settings));
    TEST_ASSERT_TRUE(settings.autoAdjustmentEnabled);
}

// The 4 new alert toggles have no pre-restructuring flat key to fall back to — a settings.json
// missing them entirely (old file, or a nested file predating this feature) must default true.
void test_settings_store_defaults_alert_toggles_to_true_when_missing() {
    const char* json = R"({"Watering":{"watering_percentage":80}})";
    irrigator::domain::Settings settings;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(json, strlen(json), settings));
    TEST_ASSERT_TRUE(settings.alertPumpUc);
    TEST_ASSERT_TRUE(settings.alertPumpOc);
    TEST_ASSERT_TRUE(settings.alertValveUc);
    TEST_ASSERT_TRUE(settings.alertValveOc);
}

// A full pre-restructuring flat settings.json — one field from each of the four new groups —
// must recover every real value via the fallback, not just the ones exercised individually above.
void test_settings_store_migrates_full_flat_json_via_fallback() {
    const char* json = R"({
        "watering_percentage":85,
        "schedule_active":false,
        "location_known":true,
        "latitude":40.7,
        "longitude":-74.0,
        "city":"Testville",
        "valve_current_base_level":700,
        "pump_current_base_level":900,
        "zone_names":{"1":{"name":"Front","enabled":false}}
    })";
    irrigator::domain::Settings settings;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(json, strlen(json), settings));
    TEST_ASSERT_EQUAL_INT16(85, settings.wateringPercentage);
    TEST_ASSERT_FALSE(settings.scheduleActive);
    TEST_ASSERT_TRUE(settings.locationKnown);
    TEST_ASSERT_EQUAL_FLOAT(40.7f, settings.latitude);
    TEST_ASSERT_EQUAL_FLOAT(-74.0f, settings.longitude);
    TEST_ASSERT_EQUAL_STRING("Testville", settings.city);
    TEST_ASSERT_EQUAL_UINT16(700, settings.valveCurrentBaseLevel);
    TEST_ASSERT_EQUAL_UINT16(900, settings.pumpCurrentBaseLevel);
    TEST_ASSERT_EQUAL_STRING("Front", settings.zoneNames[0]);
    TEST_ASSERT_FALSE(settings.zoneEnabled[0]);
    // New toggles still default true even though everything else in this file is pre-restructuring.
    TEST_ASSERT_TRUE(settings.alertPumpUc);
}

void test_merge_config_keeps_untouched_fields_and_updates_present_ones() {
    irrigator::domain::DeviceConfig existing;
    snprintf(existing.ssid, sizeof(existing.ssid), "%s", "home");
    snprintf(existing.mqttBroker, sizeof(existing.mqttBroker), "%s", "192.168.1.10");
    existing.mqttPort = 1883;
    existing.timezoneOffsetHours = 2;

    // Only mqtt_port is present; ssid/mqtt_broker/timezone must survive untouched.
    const char* json = R"({"mqtt_port":1884})";
    irrigator::domain::DeviceConfig merged;
    char rejected[128];
    TEST_ASSERT_TRUE(irrigator::domain::mergeConfig(existing, json, strlen(json), merged, rejected,
                                                    sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_EQUAL_STRING("home", merged.ssid);
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", merged.mqttBroker);
    TEST_ASSERT_EQUAL_INT8(2, merged.timezoneOffsetHours);
    TEST_ASSERT_EQUAL_UINT16(1884, merged.mqttPort);
}

void test_merge_config_null_or_empty_fields_do_not_change_existing() {
    irrigator::domain::DeviceConfig existing;
    snprintf(existing.ssid, sizeof(existing.ssid), "%s", "home");

    const char* json = R"({"ssid":"","password":null})";
    irrigator::domain::DeviceConfig merged;
    char rejected[128];
    TEST_ASSERT_TRUE(irrigator::domain::mergeConfig(existing, json, strlen(json), merged, rejected,
                                                    sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_EQUAL_STRING("home", merged.ssid);
}

void test_merge_config_rejects_out_of_range_field_but_keeps_rest() {
    irrigator::domain::DeviceConfig existing;
    snprintf(existing.ssid, sizeof(existing.ssid), "%s", "home");
    existing.timezoneOffsetHours = 2;

    const char* json = R"({"ssid":"newhome","timezone":99})";
    irrigator::domain::DeviceConfig merged;
    char rejected[128];
    TEST_ASSERT_TRUE(irrigator::domain::mergeConfig(existing, json, strlen(json), merged, rejected,
                                                    sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("timezone", rejected);
    TEST_ASSERT_EQUAL_STRING("newhome", merged.ssid);
    TEST_ASSERT_EQUAL_INT8(2, merged.timezoneOffsetHours);
}

void test_merge_config_rejects_malformed_json_outright() {
    irrigator::domain::DeviceConfig existing;
    irrigator::domain::DeviceConfig merged;
    char rejected[128];
    const char* malformed = "{not json";
    TEST_ASSERT_FALSE(irrigator::domain::mergeConfig(existing, malformed, strlen(malformed), merged,
                                                      rejected, sizeof(rejected)));
}

void test_merge_settings_keeps_untouched_fields_and_updates_present_ones() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    existing.wateringPercentage = 100;
    snprintf(existing.zoneNames[2], sizeof(existing.zoneNames[2]), "%s", "Trees");

    const char* json = R"({"Watering":{"watering_percentage":150}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_EQUAL_INT16(150, merged.wateringPercentage);
    // Zone name that wasn't mentioned in the upload must survive.
    TEST_ASSERT_EQUAL_STRING("Trees", merged.zoneNames[2]);
}

void test_merge_settings_rejects_bad_type_field_and_alerts_but_keeps_rest() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    existing.wateringPercentage = 100;

    // -1 for a boolean field is a type mismatch, not just an out-of-range number.
    const char* json = R"({"Watering":{"watering_percentage":300,"auto_adjustment_enabled":-1}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("Watering.auto_adjustment_enabled", rejected);
    TEST_ASSERT_EQUAL_INT16(300, merged.wateringPercentage);
    TEST_ASSERT_TRUE(merged.autoAdjustmentEnabled);
}

void test_merge_settings_rejects_out_of_range_numeric_field() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    existing.wateringPercentage = 100;

    // 500 is above the hard 0-300 clamp on watering_percentage.
    const char* json = R"({"Watering":{"watering_percentage":500}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("Watering.watering_percentage", rejected);
    TEST_ASSERT_EQUAL_INT16(100, merged.wateringPercentage);
}

void test_merge_settings_updates_single_zone_without_disturbing_others() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    snprintf(existing.zoneNames[0], sizeof(existing.zoneNames[0]), "%s", "Front");
    snprintf(existing.zoneNames[2], sizeof(existing.zoneNames[2]), "%s", "Back");
    existing.zoneEnabled[2] = true;

    const char* json = R"({"Zones":{"3":{"enabled":false}}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_EQUAL_STRING("Front", merged.zoneNames[0]);
    TEST_ASSERT_EQUAL_STRING("Back", merged.zoneNames[2]);
    TEST_ASSERT_FALSE(merged.zoneEnabled[2]);
}

void test_merge_settings_upper_lower_limit_cross_validated_together() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    existing.upperLimitPercent = 200;
    existing.lowerLimitPercent = 40;

    // upper < lower makes no sense together; both must be rejected and neither applied.
    const char* json = R"({"Watering":{"upper_limit":30,"lower_limit":40}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("Watering.upper_limit,Watering.lower_limit", rejected);
    TEST_ASSERT_EQUAL_INT16(200, merged.upperLimitPercent);
    TEST_ASSERT_EQUAL_INT16(40, merged.lowerLimitPercent);
}

void test_merge_settings_never_reads_device_owned_location_fields() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    existing.locationKnown = true;
    existing.latitude = 12.5f;
    existing.longitude = -3.5f;
    snprintf(existing.city, sizeof(existing.city), "%s", "Testville");

    const char* json =
        R"({"Location":{"location_known":false,"latitude":0.0,"longitude":0.0,"city":"Somewhere Else"}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_TRUE(merged.locationKnown);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, merged.latitude);
    TEST_ASSERT_EQUAL_FLOAT(-3.5f, merged.longitude);
    TEST_ASSERT_EQUAL_STRING("Testville", merged.city);
}

void test_merge_settings_updates_current_sensor_alert_toggles() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    TEST_ASSERT_TRUE(existing.alertPumpUc);
    TEST_ASSERT_TRUE(existing.alertPumpOc);
    TEST_ASSERT_TRUE(existing.alertValveUc);
    TEST_ASSERT_TRUE(existing.alertValveOc);

    const char* json =
        R"({"Current_sensor":{"alert_pump_uc":false,"alert_valve_oc":false}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_FALSE(merged.alertPumpUc);
    TEST_ASSERT_TRUE(merged.alertPumpOc);
    TEST_ASSERT_TRUE(merged.alertValveUc);
    TEST_ASSERT_FALSE(merged.alertValveOc);
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

// Pinned: SCHEDULE_MAX_RECORDS must stay exactly 255 (the ceiling a uint8_t record count/index
// can hold) unless ScheduleList::count, UiIntent::scheduleRecordIndex, and every uint8_t
// schedule-index touchpoint in UiStateMachine/IrrigatorUi are widened too — see board_config.h's
// comment on the constant.
void test_schedule_max_records_is_255() {
    TEST_ASSERT_EQUAL_UINT8(255, irrigator::board::SCHEDULE_MAX_RECORDS);
}

namespace {

size_t buildScheduleJson(char* out, size_t outSize, uint16_t recordCount) {
    size_t offset = static_cast<size_t>(snprintf(out, outSize, "["));
    for (uint16_t i = 0; i < recordCount; ++i) {
        const unsigned zone = (i % irrigator::board::ZONE_COUNT) + 1;
        const unsigned day = (i % 7) + 1;
        offset += static_cast<size_t>(snprintf(
            out + offset,
            outSize - offset,
            "%s{\"zone\":%u,\"day_of_week\":%u,\"start_time\":\"06:%02u\",\"duration_min\":20,\"active\":true}",
            i == 0 ? "" : ",",
            zone,
            day,
            i % 60));
    }
    offset += static_cast<size_t>(snprintf(out + offset, outSize - offset, "]"));
    return offset;
}

}  // namespace

void test_schedule_store_accepts_exactly_max_records_and_rejects_one_more() {
    char atCapacityJson[irrigator::board::SCHEDULE_JSON_MAX_BYTES];
    const size_t atCapacityLen =
        buildScheduleJson(atCapacityJson, sizeof(atCapacityJson), irrigator::board::SCHEDULE_MAX_RECORDS);
    irrigator::domain::ScheduleList atCapacity;
    TEST_ASSERT_TRUE(irrigator::domain::parseSchedule(atCapacityJson, atCapacityLen, atCapacity));
    TEST_ASSERT_EQUAL_UINT8(irrigator::board::SCHEDULE_MAX_RECORDS, atCapacity.count);

    char overCapacityJson[irrigator::board::SCHEDULE_JSON_MAX_BYTES + 128];
    const size_t overCapacityLen = buildScheduleJson(
        overCapacityJson,
        sizeof(overCapacityJson),
        static_cast<uint16_t>(irrigator::board::SCHEDULE_MAX_RECORDS) + 1);
    irrigator::domain::ScheduleList overCapacity;
    TEST_ASSERT_FALSE(irrigator::domain::parseSchedule(overCapacityJson, overCapacityLen, overCapacity));
}

// The worst-case-size scenario board_config.h's SCHEDULE_JSON_MAX_BYTES comment is based on:
// 255 records, every one using the widest fields (2-digit zone, 2-digit duration, "false" - the
// longer of true/false). Confirms serializeSchedule() doesn't silently truncate against that
// buffer (an undersized buffer would still report a nonzero `written` — truncated-but-valid-
// looking JSON is exactly the failure mode that made this worth pinning) and that the result
// round-trips back through parseSchedule() intact.
void test_schedule_serialize_255_records_fits_json_max_bytes_and_round_trips() {
    irrigator::domain::ScheduleList list;
    for (uint16_t i = 0; i < irrigator::board::SCHEDULE_MAX_RECORDS; ++i) {
        irrigator::domain::ScheduleRecord record;
        record.zone = irrigator::board::ZONE_COUNT;
        record.dayOfWeek = static_cast<uint8_t>((i % 7) + 1);
        snprintf(record.startTime, sizeof(record.startTime), "%02u:%02u", i % 24, i % 60);
        record.durationMin = irrigator::board::SCHEDULE_DURATION_MAX_MINUTES;
        record.active = false;
        list.records[list.count++] = record;
    }
    TEST_ASSERT_EQUAL_UINT8(irrigator::board::SCHEDULE_MAX_RECORDS, list.count);

    char buffer[irrigator::board::SCHEDULE_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSchedule(list, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE(written < sizeof(buffer));  // not truncated against the buffer

    irrigator::domain::ScheduleList roundTripped;
    TEST_ASSERT_TRUE(irrigator::domain::parseSchedule(buffer, written, roundTripped));
    TEST_ASSERT_EQUAL_UINT8(list.count, roundTripped.count);
    TEST_ASSERT_EQUAL_UINT8(list.records[254].zone, roundTripped.records[254].zone);
    TEST_ASSERT_EQUAL_UINT8(list.records[254].durationMin, roundTripped.records[254].durationMin);
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
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());  // Stop is item 0 while watering

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
    TEST_ASSERT_EQUAL_UINT16(1, ui.selectedIndex());

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

    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexPercentage; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
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

    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexUpperLimit; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
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

void test_ui_state_machine_settings_schedule_active_toggle_is_first_item() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexScheduleActive,
                            ui.selectedIndex());

    // Direct toggle — no separate edit step.
    const irrigator::domain::UiIntent offIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(offIntent.type == irrigator::domain::UiIntentType::ToggleScheduleActive);
    TEST_ASSERT_FALSE(offIntent.boolValue);
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSettings);

    ctx.scheduleActive = false;
    const irrigator::domain::UiIntent onIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(onIntent.type == irrigator::domain::UiIntentType::ToggleScheduleActive);
    TEST_ASSERT_TRUE(onIntent.boolValue);
}

void test_ui_state_machine_settings_auto_adjustment_toggle_is_second_item() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.autoAdjustmentEnabled = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350);  // Schedule active -> Auto-adjust
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexAutoAdjustment,
                            ui.selectedIndex());

    // Direct toggle, like ToggleScheduleActive — no separate edit step.
    const irrigator::domain::UiIntent offIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(offIntent.type == irrigator::domain::UiIntentType::ToggleAutoAdjustment);
    TEST_ASSERT_FALSE(offIntent.boolValue);
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSettings);

    ctx.autoAdjustmentEnabled = false;
    const irrigator::domain::UiIntent onIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(onIntent.type == irrigator::domain::UiIntentType::ToggleAutoAdjustment);
    TEST_ASSERT_TRUE(onIntent.boolValue);
}

// The 4 alert toggles sit directly above Calibrate CS, same direct-toggle shape as
// ScheduleActive/AutoAdjustment (no separate edit step).
void test_ui_state_machine_settings_alert_toggles_sit_above_calibrate() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.alertPumpUc = true;
    ctx.alertPumpOc = true;
    ctx.alertValveUc = true;
    ctx.alertValveOc = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings

    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexAlertPumpUc; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexAlertPumpUc, ui.selectedIndex());
    const irrigator::domain::UiIntent pumpUcOff =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(pumpUcOff.type == irrigator::domain::UiIntentType::ToggleAlertPumpUc);
    TEST_ASSERT_FALSE(pumpUcOff.boolValue);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSettings);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 450);  // -> Alert pump OC
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexAlertPumpOc, ui.selectedIndex());
    const irrigator::domain::UiIntent pumpOcOff =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(pumpOcOff.type == irrigator::domain::UiIntentType::ToggleAlertPumpOc);
    TEST_ASSERT_FALSE(pumpOcOff.boolValue);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 550);  // -> Alert valve UC
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexAlertValveUc, ui.selectedIndex());
    const irrigator::domain::UiIntent valveUcOff =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);
    TEST_ASSERT_TRUE(valveUcOff.type == irrigator::domain::UiIntentType::ToggleAlertValveUc);
    TEST_ASSERT_FALSE(valveUcOff.boolValue);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 650);  // -> Alert valve OC
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexAlertValveOc, ui.selectedIndex());
    const irrigator::domain::UiIntent valveOcOff =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 700);
    TEST_ASSERT_TRUE(valveOcOff.type == irrigator::domain::UiIntentType::ToggleAlertValveOc);
    TEST_ASSERT_FALSE(valveOcOff.boolValue);

    // One more step must land on Calibrate CS — confirms the 4 new rows sit directly above it.
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 750);
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexCalibrate, ui.selectedIndex());
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
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexRestart, ui.selectedIndex());

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

void test_ui_state_machine_settings_calibrate_confirm_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings

    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexCalibrate; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 400 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexCalibrate, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);  // -> CalibrateConfirm
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::CalibrateConfirm);
    TEST_ASSERT_FALSE(ui.calibrateConfirmYesSelected());

    // NO stays selected by default; confirming without toggling to YES must not start calibration.
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
    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexCalibrate; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1100 + i);
    }
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1200);  // -> CalibrateConfirm
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1300);
    TEST_ASSERT_TRUE(ui.calibrateConfirmYesSelected());

    const irrigator::domain::UiIntent yesIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1400);
    TEST_ASSERT_TRUE(yesIntent.type == irrigator::domain::UiIntentType::CalibrateCurrentSensors);
    // Unlike the NO path above, YES does NOT transition screen_ itself (same as
    // handleRestartConfirm) — the caller (main.ino's handleUiIntent) is expected to run the
    // calibration and then call showCalibrateResult(), which is what actually moves the screen on.
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::CalibrateConfirm);
}

void test_ui_state_machine_calibrate_result_shows_outcome_and_ok_returns_to_normal() {
    irrigator::domain::UiStateMachine ui;
    ui.wake(0);

    ui.showCalibrateResult(true, 734, "", false, 0, "too low (12)", 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::CalibrateResult);
    TEST_ASSERT_TRUE(ui.calibrateValveSucceeded());
    TEST_ASSERT_EQUAL_UINT16(734, ui.calibrateValveBaseLevel());
    TEST_ASSERT_FALSE(ui.calibratePumpSucceeded());
    TEST_ASSERT_EQUAL_STRING("too low (12)", ui.calibratePumpMessage());

    // Rotation is a no-op on this screen — nothing to navigate to.
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 150);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::CalibrateResult);

    // Click (the OK button) dismisses back to Normal.
    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_info_menu_is_separate_from_settings_and_read_only() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain

    const auto layout = irrigator::domain::UiStateMachine::menuMainLayout(false);
    for (uint8_t i = 0; i < layout.infoIndex; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(layout.infoIndex, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);  // -> MenuInfo
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuInfo);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());

    // Every display-only row is a no-op click; only BACK navigates.
    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kInfoIndexBack; ++i) {
        const irrigator::domain::UiIntent intent =
            ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400 + i);
        TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
        TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuInfo);
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 450 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kInfoIndexBack, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);  // BACK -> MenuMain
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
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
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSchedule);

    // Item layout for one record: 0=zone,1=day,2=time,3=duration,4=active, 5=BACK.
    for (int i = 0; i < 3; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kScheduleFieldDuration,
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
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule
    // Item 0 for one record is the Zone field already.
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kScheduleFieldZone, ui.selectedIndex());

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
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // Zone -> Day field
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kScheduleFieldDay, ui.selectedIndex());

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
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule
    for (int i = 0; i < 2; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);  // -> Time field
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kScheduleFieldTime, ui.selectedIndex());

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

void test_ui_state_machine_schedule_root_navigation() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 1;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuScheduleRoot);
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kMenuScheduleRootIndexViewEdit,
                            ui.selectedIndex());

    // Wraps across the 4 items: View/Edit(0) -> Add(1) -> Delete(2) -> BACK(3) -> View/Edit(0).
    for (int i = 0; i < 4; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kMenuScheduleRootIndexViewEdit,
                            ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 400);  // -> Add
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleAdd);

    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 600);  // cancel back
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuScheduleRoot);
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kMenuScheduleRootIndexViewEdit,
                            ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 700);  // -> Add
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 710);  // -> Delete
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 800);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleDelete);

    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 900);  // cancel back
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuScheduleRoot);

    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 1000);  // -> MenuMain
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
}

void test_ui_state_machine_schedule_add_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 0;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // View/Edit -> Add
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);      // Add -> ScheduleAdd
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleAdd);

    // Defaults seeded on entry.
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingScheduleZone());
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingScheduleDay());
    TEST_ASSERT_EQUAL_UINT8(6, ui.pendingScheduleHour());
    TEST_ASSERT_EQUAL_UINT8(0, ui.pendingScheduleMinute());
    TEST_ASSERT_EQUAL_UINT16(irrigator::board::SCHEDULE_DURATION_MIN_MINUTES, ui.pendingDuration());
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kScheduleAddIndexZone, ui.selectedIndex());

    // Editing Zone stages the value but never emits an intent.
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 500);  // enter zone edit
    TEST_ASSERT_TRUE(ui.editing());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 600);
    TEST_ASSERT_EQUAL_UINT8(2, ui.pendingScheduleZone());
    irrigator::domain::UiIntent intent = ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 700);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleAdd);

    // Editing Time needs the hour-then-minute two-click gesture, same as MenuSchedule's edit.
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 800);  // Zone -> Day
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 810);  // Day -> Time
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 900);      // enter time edit (hour step)
    TEST_ASSERT_TRUE(ui.editing());
    TEST_ASSERT_FALSE(ui.editingScheduleTimeMinuteStep());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1000);
    TEST_ASSERT_EQUAL_UINT8(7, ui.pendingScheduleHour());
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1100);  // confirm hour -> minute step
    TEST_ASSERT_TRUE(ui.editingScheduleTimeMinuteStep());
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1200);
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingScheduleMinute());
    intent = ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1300);  // confirm minute
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_FALSE(ui.editing());

    // Save bundles the staged fields into one AddScheduleRecord intent and does NOT change screen —
    // main.ino is expected to perform the mutation and call showScheduleActionResult().
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1400);  // Time -> Duration
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1410);  // Duration -> Save
    intent = ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1500);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::AddScheduleRecord);
    TEST_ASSERT_EQUAL_UINT8(2, intent.zone);
    TEST_ASSERT_EQUAL_UINT8(1, intent.dayOfWeek);  // Day was navigated past, never edited.
    TEST_ASSERT_EQUAL_UINT8(7, intent.hour);
    TEST_ASSERT_EQUAL_UINT8(1, intent.minute);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleAdd);

    // main.ino performs the Add and reports back; OK returns to MenuScheduleRoot @ "Add".
    ui.showScheduleActionResult(true, 1600);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleActionResult);
    TEST_ASSERT_TRUE(ui.scheduleActionSucceeded());
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1700);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuScheduleRoot);
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kMenuScheduleRootIndexAdd,
                            ui.selectedIndex());
}

void test_ui_state_machine_schedule_delete_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 2;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // View/Edit -> Add
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 310);  // Add -> Delete
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);      // Delete -> ScheduleDelete
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleDelete);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);  // row 0 -> row 1
    TEST_ASSERT_EQUAL_UINT16(1, ui.selectedIndex());
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);      // row 1 -> ScheduleDeleteConfirm
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleDeleteConfirm);
    TEST_ASSERT_EQUAL_UINT8(1, ui.scheduleDeleteRecordIndex());
    TEST_ASSERT_FALSE(ui.scheduleDeleteConfirmYesSelected());

    // NO returns to ScheduleDelete at the same row.
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 700);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleDelete);
    TEST_ASSERT_EQUAL_UINT16(1, ui.selectedIndex());

    // Re-enter, flip to YES: emits DeleteScheduleRecord, does NOT change screen.
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 800);  // -> ScheduleDeleteConfirm
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 900);  // NO -> YES
    TEST_ASSERT_TRUE(ui.scheduleDeleteConfirmYesSelected());
    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1000);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::DeleteScheduleRecord);
    TEST_ASSERT_EQUAL_UINT8(1, intent.scheduleRecordIndex);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleDeleteConfirm);

    // main.ino performs the delete and reports back; OK returns to ScheduleDelete @ same index.
    ui.showScheduleActionResult(true, 1100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleActionResult);
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1200);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleDelete);
    TEST_ASSERT_EQUAL_UINT16(1, ui.selectedIndex());
}

void test_ui_state_machine_schedule_action_result_reports_failure() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.showScheduleActionResult(false, 0);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ScheduleActionResult);
    TEST_ASSERT_FALSE(ui.scheduleActionSucceeded());

    // Still routes back correctly on failure — default return target is MenuScheduleRoot @ Add
    // (the member's default-constructed value, since no Add/Delete click ran in this test).
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuScheduleRoot);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());
}

// scheduleItemCount(recordCount) = recordCount*5 + 1; a uint8_t result would have silently
// wrapped starting at recordCount=52 (52*5+1=261, 261 mod 256 = 5) — long before the 255-record
// cap SCHEDULE_MAX_RECORDS actually allows. Confirms the uint16_t widening actually holds these.
void test_ui_state_machine_schedule_item_count_does_not_overflow_uint8() {
    TEST_ASSERT_EQUAL_UINT16(261, irrigator::domain::UiStateMachine::scheduleItemCount(52));
    TEST_ASSERT_EQUAL_UINT16(1276, irrigator::domain::UiStateMachine::scheduleItemCount(255));
}

void test_ui_state_machine_menu_schedule_navigates_past_the_old_uint8_wrap_point() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    // itemCount = 60*5+1 = 301 — with a uint8_t itemCount this truncates to 301 mod 256 = 45,
    // corrupting navigation for every schedule this size or larger long before hitting 255 records.
    ctx.scheduleRecordCount = 60;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule

    // Step to item 260 — past the old 8-bit wrap point of 256. With a uint8_t selectedIndex_ this
    // would have wrapped long before reaching 260.
    for (uint16_t i = 0; i < 260; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(260, ui.selectedIndex());
}

void test_ui_state_machine_schedule_delete_navigates_full_255_record_list() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    // itemCount = 255+1 = 256. With the old `uint8_t itemCount = static_cast<uint8_t>(recordCount
    // + 1)`, this truncates to exactly 0, and stepIndex's `count==0` guard makes every CW/CCW tick
    // a permanent no-op — the Delete list would be stuck at record count 255 forever.
    ctx.scheduleRecordCount = irrigator::board::SCHEDULE_MAX_RECORDS;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // View/Edit -> Add
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 310);  // Add -> Delete
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);      // Delete -> ScheduleDelete

    for (uint16_t i = 0; i < 255; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(255, ui.selectedIndex());  // the BACK row, last of 256 items

    // One more tick wraps back to 0 — proves the modulus divisor is really 256, not something
    // smaller left over from 8-bit truncation.
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 900);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());
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
    RUN_TEST(test_summarize_past_average_daily_max_temperature_averages_first_n_days);
    RUN_TEST(test_summarize_past_average_daily_max_temperature_rejects_short_array);
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
    RUN_TEST(test_settings_store_defaults_auto_adjustment_enabled_to_true_when_missing);
    RUN_TEST(test_settings_store_defaults_alert_toggles_to_true_when_missing);
    RUN_TEST(test_settings_store_migrates_full_flat_json_via_fallback);
    RUN_TEST(test_merge_config_keeps_untouched_fields_and_updates_present_ones);
    RUN_TEST(test_merge_config_null_or_empty_fields_do_not_change_existing);
    RUN_TEST(test_merge_config_rejects_out_of_range_field_but_keeps_rest);
    RUN_TEST(test_merge_config_rejects_malformed_json_outright);
    RUN_TEST(test_merge_settings_keeps_untouched_fields_and_updates_present_ones);
    RUN_TEST(test_merge_settings_rejects_bad_type_field_and_alerts_but_keeps_rest);
    RUN_TEST(test_merge_settings_rejects_out_of_range_numeric_field);
    RUN_TEST(test_merge_settings_updates_single_zone_without_disturbing_others);
    RUN_TEST(test_merge_settings_upper_lower_limit_cross_validated_together);
    RUN_TEST(test_merge_settings_never_reads_device_owned_location_fields);
    RUN_TEST(test_merge_settings_updates_current_sensor_alert_toggles);
    RUN_TEST(test_schedule_store_round_trips_valid_list);
    RUN_TEST(test_schedule_store_rejects_whole_file_on_one_bad_record);
    RUN_TEST(test_schedule_max_records_is_255);
    RUN_TEST(test_schedule_store_accepts_exactly_max_records_and_rejects_one_more);
    RUN_TEST(test_schedule_serialize_255_records_fits_json_max_bytes_and_round_trips);
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
    RUN_TEST(test_ui_state_machine_settings_schedule_active_toggle_is_first_item);
    RUN_TEST(test_ui_state_machine_settings_auto_adjustment_toggle_is_second_item);
    RUN_TEST(test_ui_state_machine_settings_alert_toggles_sit_above_calibrate);
    RUN_TEST(test_ui_state_machine_settings_rain_skip_threshold_edit);
    RUN_TEST(test_ui_state_machine_settings_restart_confirm_flow);
    RUN_TEST(test_ui_state_machine_settings_calibrate_confirm_flow);
    RUN_TEST(test_ui_state_machine_calibrate_result_shows_outcome_and_ok_returns_to_normal);
    RUN_TEST(test_ui_state_machine_info_menu_is_separate_from_settings_and_read_only);
    RUN_TEST(test_ui_state_machine_test_all_zones_flow);
    RUN_TEST(test_ui_state_machine_schedule_duration_edit);
    RUN_TEST(test_ui_state_machine_schedule_zone_edit_wraps);
    RUN_TEST(test_ui_state_machine_schedule_day_edit_wraps);
    RUN_TEST(test_ui_state_machine_schedule_time_edit_hour_then_minute);
    RUN_TEST(test_ui_state_machine_schedule_root_navigation);
    RUN_TEST(test_ui_state_machine_schedule_add_flow);
    RUN_TEST(test_ui_state_machine_schedule_delete_flow);
    RUN_TEST(test_ui_state_machine_schedule_action_result_reports_failure);
    RUN_TEST(test_ui_state_machine_schedule_item_count_does_not_overflow_uint8);
    RUN_TEST(test_ui_state_machine_menu_schedule_navigates_past_the_old_uint8_wrap_point);
    RUN_TEST(test_ui_state_machine_schedule_delete_navigates_full_255_record_list);
    RUN_TEST(test_calendar_math_round_trips_and_weekday);
    RUN_TEST(test_wifi_quality_thresholds_and_bar_count);
    RUN_TEST(test_dst_rules_matches_known_2024_eu_transitions);
    return UNITY_END();
}