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

// The generic adjuster the manual/scheduled wrappers both delegate to — its own bounds/step rules,
// exercised directly rather than only through the two board_config-fixed ranges above.
void test_adjust_duration_step_grows_with_magnitude() {
    // step = current/10 + 1, so the rotary moves in coarser increments the higher the value gets —
    // one tick near 0 is +1, near 100 it's +11.
    TEST_ASSERT_EQUAL_UINT16(2, irrigator::domain::adjustDuration(1, RotationDirection::Clockwise, 1, 1000));
    TEST_ASSERT_EQUAL_UINT16(111,
                             irrigator::domain::adjustDuration(100, RotationDirection::Clockwise, 1, 1000));
    TEST_ASSERT_EQUAL_UINT16(89,
                             irrigator::domain::adjustDuration(100, RotationDirection::CounterClockwise, 1, 1000));
}

void test_adjust_duration_pulls_out_of_range_current_into_range_first() {
    // A `current` outside [min,max] (e.g. a stale value from a wider range) is clamped before the
    // step is applied, so one tick lands inside the range rather than stepping from nonsense.
    TEST_ASSERT_EQUAL_UINT16(12,
                             irrigator::domain::adjustDuration(3, RotationDirection::Clockwise, 10, 100));
    TEST_ASSERT_EQUAL_UINT16(89,
                             irrigator::domain::adjustDuration(500, RotationDirection::CounterClockwise, 10, 100));
}

void test_adjust_duration_saturates_at_both_bounds() {
    TEST_ASSERT_EQUAL_UINT16(100,
                             irrigator::domain::adjustDuration(95, RotationDirection::Clockwise, 10, 100));
    // A step that would undershoot `min` lands exactly on `min` instead of wrapping/underflowing.
    TEST_ASSERT_EQUAL_UINT16(10,
                             irrigator::domain::adjustDuration(11, RotationDirection::CounterClockwise, 10, 100));
    // step >= current is the uint16 underflow guard: 1 - 1 would be 0, below min.
    TEST_ASSERT_EQUAL_UINT16(1,
                             irrigator::domain::adjustDuration(1, RotationDirection::CounterClockwise, 1, 100));
}

// An inverted range has no valid value to move to, so `current` is returned untouched rather than
// clamped to one of two contradictory bounds.
void test_adjust_duration_inverted_range_is_a_no_op() {
    TEST_ASSERT_EQUAL_UINT16(50,
                             irrigator::domain::adjustDuration(50, RotationDirection::Clockwise, 100, 10));
    TEST_ASSERT_EQUAL_UINT16(50,
                             irrigator::domain::adjustDuration(50, RotationDirection::CounterClockwise, 100, 10));
}

void test_watering_percentage_edge_cases() {
    // Zero duration and a non-positive percentage both mean "don't water", not "water a little".
    TEST_ASSERT_EQUAL_UINT16(0, irrigator::domain::applyWateringPercentage(0, 150));
    TEST_ASSERT_EQUAL_UINT16(0, irrigator::domain::applyWateringPercentage(10, -50));
    // 100% is the identity case the schedule engine relies on when auto-adjustment is off.
    TEST_ASSERT_EQUAL_UINT16(20, irrigator::domain::applyWateringPercentage(20, 100));
    // The product is computed in uint32 and saturates instead of wrapping back around to a short run.
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, irrigator::domain::applyWateringPercentage(60000, 300));
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
        94, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 4.0f, 20.0f, 40, 200));
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

// The skip/clamp comparisons are both strict (`<` lower, `>` upper), so a percentage landing
// exactly on a limit is a legal value, not a boundary that trips the special case. At the default
// 10%/degree from a 20C base these land on exact float values: 14C -> 40, 30C -> 200.
void test_auto_adjustment_percentage_exactly_on_lower_limit_is_kept_not_zeroed() {
    TEST_ASSERT_EQUAL_INT16(
        40, irrigator::domain::computeAutoCorrectedPercentage(14.0f, 0.0f, 20.0f, 40, 200));
    // A hair below is the skip case, confirming 40 above really was the boundary and not a clamp.
    TEST_ASSERT_EQUAL_INT16(
        0, irrigator::domain::computeAutoCorrectedPercentage(13.9f, 0.0f, 20.0f, 40, 200));
}

void test_auto_adjustment_percentage_exactly_on_upper_limit_is_not_clamped_down() {
    TEST_ASSERT_EQUAL_INT16(
        200, irrigator::domain::computeAutoCorrectedPercentage(30.0f, 0.0f, 20.0f, 40, 200));
}

// A lower limit of 0 disables the skip branch entirely (nothing can be < 0 once the negative floor
// applies), so extreme rain reduces the run to 0% by arithmetic rather than by the skip rule —
// same output, but it's the path a user who sets lower_limit=0 actually takes.
void test_auto_adjustment_percentage_zero_lower_limit_floors_at_zero() {
    TEST_ASSERT_EQUAL_INT16(
        0, irrigator::domain::computeAutoCorrectedPercentage(20.0f, 1000.0f, 20.0f, 0, 200));
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

void test_open_meteo_now_index_with_no_past_days_is_just_the_hour() {
    const int64_t epoch = irrigator::domain::epochFromUtc(2026, 7, 28, 9, 30, 0);
    TEST_ASSERT_EQUAL_UINT32(9, static_cast<uint32_t>(irrigator::domain::openMeteoNowIndex(epoch, 0)));
}

// The window is half-open [nowIndex, nowIndex+hours), so it may end exactly at the last element;
// needing one element past the end is what fails.
void test_summarize_forecast_rain_window_accepts_window_ending_exactly_at_array_end() {
    JsonDocument doc;
    JsonArray prob = doc["p"].to<JsonArray>();
    JsonArray precip = doc["r"].to<JsonArray>();
    for (int i = 0; i < 12; ++i) {
        prob.add(10.0f);
        precip.add(0.25f);
    }

    float maxProb = 0.0f, rainWindow = 0.0f;
    TEST_ASSERT_TRUE(
        irrigator::domain::summarizeForecastRainWindow(prob, precip, 6, 6, maxProb, rainWindow));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, maxProb);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, rainWindow);  // 6 hours * 0.25mm

    TEST_ASSERT_FALSE(
        irrigator::domain::summarizeForecastRainWindow(prob, precip, 7, 6, maxProb, rainWindow));
}

// The two hourly arrays come from the same response but are length-checked independently — a
// response carrying probabilities but a truncated precipitation array must fail the whole summary
// rather than reporting a rain total short by the missing hours.
void test_summarize_forecast_rain_window_rejects_when_only_the_amount_array_is_short() {
    JsonDocument doc;
    JsonArray prob = doc["p"].to<JsonArray>();
    for (int i = 0; i < 24; ++i) {
        prob.add(50.0f);
    }
    JsonArray precip = doc["r"].to<JsonArray>();
    for (int i = 0; i < 3; ++i) {
        precip.add(1.0f);
    }

    float maxProb = 0.0f, rainWindow = 0.0f;
    TEST_ASSERT_FALSE(
        irrigator::domain::summarizeForecastRainWindow(prob, precip, 0, 12, maxProb, rainWindow));
}

// WeatherAPI splits its hours across day objects, so a 12h window routinely straddles a day
// boundary — the flattening must continue into the next day, and must still stop dead at
// windowHours rather than draining whatever hours remain in that day.
void test_summarize_weather_api_forecast_rain_window_spans_day_boundary_and_stops_at_window() {
    JsonDocument doc;
    JsonArray days = doc.to<JsonArray>();
    const float chances[2][3] = {{10.0f, 20.0f, 30.0f}, {90.0f, 40.0f, 99.0f}};
    for (int d = 0; d < 2; ++d) {
        JsonObject day = days.add<JsonObject>();
        JsonArray hours = day["hour"].to<JsonArray>();
        for (int i = 0; i < 3; ++i) {
            JsonObject h = hours.add<JsonObject>();
            h["time_epoch"] = 1000 + (d * 3 + i) * 3600;
            h["chance_of_rain"] = chances[d][i];
            h["precip_mm"] = 1.0f;
        }
    }

    float maxProb = 0.0f, rain = 0.0f;
    size_t qualifyingHours = 0;
    TEST_ASSERT_TRUE(irrigator::domain::summarizeWeatherApiForecastRainWindow(
        days, 1000, 5, maxProb, rain, qualifyingHours));
    TEST_ASSERT_EQUAL_UINT(5, qualifyingHours);
    // 90 (day 1, hour 0) is inside the window; the higher 99 sits in the 6th hour and must not count.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, maxProb);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, rain);
}

// The "found 0 qualifying" case WeatherSummary.h calls out explicitly: every hour is in the past
// (or, in the wild, missing time_epoch and defaulting to 0), which reads very differently from a
// genuinely short response — outQualifyingHours is what lets the caller tell them apart.
void test_summarize_weather_api_forecast_rain_window_reports_zero_qualifying_hours() {
    JsonDocument doc;
    JsonArray days = doc.to<JsonArray>();
    JsonObject day0 = days.add<JsonObject>();
    JsonArray hours0 = day0["hour"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) {
        JsonObject h = hours0.add<JsonObject>();
        h["time_epoch"] = 1000 + i * 3600;
        h["chance_of_rain"] = 100;
        h["precip_mm"] = 5.0f;
    }

    float maxProb = 0.0f, rain = 0.0f;
    size_t qualifyingHours = 99;
    TEST_ASSERT_FALSE(irrigator::domain::summarizeWeatherApiForecastRainWindow(
        days, 1000 + 100 * 3600, 3, maxProb, rain, qualifyingHours));
    TEST_ASSERT_EQUAL_UINT(0, qualifyingHours);
}

void test_start_time_validation() {
    TEST_ASSERT_TRUE(irrigator::domain::isValidStartTime("00:00"));
    TEST_ASSERT_TRUE(irrigator::domain::isValidStartTime("23:59"));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("24:00"));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("12:60"));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("7:30"));
}

// isValidStartTime indexes value[0..5] positionally, so every malformed-length input has to bail on
// a character check before running off the end of the string — these are the shapes that reach it
// from an uploaded schedule.json (parseSchedule coerces a missing/mistyped start_time to "").
void test_start_time_validation_rejects_malformed_lengths_without_overrunning() {
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime(nullptr));
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime(""));       // what a missing field becomes
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("0630"));   // separator missing
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("06:3"));   // one digit short
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("06:300")); // one character too long
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("ab:cd"));  // right shape, not digits
    TEST_ASSERT_FALSE(irrigator::domain::isValidStartTime("06-30"));  // wrong separator
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

// The ring buffer's wrap-around: pushing after a pop reuses the freed slot, so a queue that has
// cycled more times than its capacity must still hand items back in FIFO order rather than
// reordering them by physical slot.
void test_watering_queue_wraps_around_and_preserves_fifo_order() {
    irrigator::domain::WateringQueue<3> queue;
    TEST_ASSERT_TRUE(queue.push({1, 5, irrigator::domain::RunReason::Manual}));
    TEST_ASSERT_TRUE(queue.push({2, 5, irrigator::domain::RunReason::Manual}));
    TEST_ASSERT_TRUE(queue.push({3, 5, irrigator::domain::RunReason::Manual}));

    irrigator::domain::WateringRequest request;
    TEST_ASSERT_TRUE(queue.pop(request));
    TEST_ASSERT_EQUAL_UINT8(1, request.zone);
    // Slot 0 is free again; zone 4 lands there while remaining logically last.
    TEST_ASSERT_TRUE(queue.push({4, 5, irrigator::domain::RunReason::Manual}));

    for (uint8_t expected = 2; expected <= 4; ++expected) {
        TEST_ASSERT_TRUE(queue.pop(request));
        TEST_ASSERT_EQUAL_UINT8(expected, request.zone);
    }
    TEST_ASSERT_TRUE(queue.empty());
}

// at() reports FIFO position, not physical slot — cmd/queue renders the pending list straight from
// it, so it has to stay head-relative after the buffer has wrapped.
void test_watering_queue_peek_and_size_accessors() {
    irrigator::domain::WateringQueue<3> queue;
    TEST_ASSERT_EQUAL_UINT8(3, queue.capacity());
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_FALSE(queue.full());
    TEST_ASSERT_EQUAL_UINT8(0, queue.size());

    queue.push({1, 5, irrigator::domain::RunReason::Manual});
    queue.push({2, 10, irrigator::domain::RunReason::Schedule});
    queue.push({3, 15, irrigator::domain::RunReason::Mqtt});
    TEST_ASSERT_TRUE(queue.full());
    TEST_ASSERT_EQUAL_UINT8(3, queue.size());
    TEST_ASSERT_EQUAL_UINT8(1, queue.at(0).zone);
    TEST_ASSERT_EQUAL_UINT16(15, queue.at(2).durationMin);

    irrigator::domain::WateringRequest request;
    queue.pop(request);
    queue.push({4, 20, irrigator::domain::RunReason::Manual});
    TEST_ASSERT_EQUAL_UINT8(2, queue.at(0).zone);  // head moved; index 0 is still "next to run"
    TEST_ASSERT_EQUAL_UINT8(4, queue.at(2).zone);
}

// Stop-all drops everything pending in one go (see UiIntentType::StopAllWatering) — clear() has to
// reset the head too, not just the count, or the next push starts mid-buffer and at() misreports.
void test_watering_queue_clear_empties_and_resets_head() {
    irrigator::domain::WateringQueue<3> queue;
    queue.push({1, 5, irrigator::domain::RunReason::Manual});
    queue.push({2, 5, irrigator::domain::RunReason::Manual});
    irrigator::domain::WateringRequest request;
    queue.pop(request);  // head is now 1

    queue.clear();
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_EQUAL_UINT8(0, queue.size());
    TEST_ASSERT_FALSE(queue.pop(request));

    TEST_ASSERT_TRUE(queue.push({7, 5, irrigator::domain::RunReason::Manual}));
    TEST_ASSERT_EQUAL_UINT8(7, queue.at(0).zone);
}

// Zone 0 is the pump-only sentinel throughout (see WateringRequest::zone) — the whole
// pump-only-vs-zone branch in IrrigationStateMachine keys off this one predicate.
void test_watering_request_kind_treats_zone_zero_as_pump_only() {
    const irrigator::domain::WateringRequest pumpOnly{0, 5, irrigator::domain::RunReason::Manual};
    TEST_ASSERT_TRUE(pumpOnly.kind() == irrigator::domain::RunKind::PumpOnly);

    const irrigator::domain::WateringRequest zoneRun{1, 5, irrigator::domain::RunReason::Manual};
    TEST_ASSERT_TRUE(zoneRun.kind() == irrigator::domain::RunKind::ZoneWithPump);

    const irrigator::domain::WateringRequest lastZone{irrigator::board::ZONE_COUNT, 5,
                                                     irrigator::domain::RunReason::Schedule};
    TEST_ASSERT_TRUE(lastZone.kind() == irrigator::domain::RunKind::ZoneWithPump);
}

// These strings go out over MQTT as the run's `reason` — HA templates match on them literally.
void test_run_reason_to_string() {
    TEST_ASSERT_EQUAL_STRING("manual", irrigator::domain::toString(irrigator::domain::RunReason::Manual));
    TEST_ASSERT_EQUAL_STRING("schedule", irrigator::domain::toString(irrigator::domain::RunReason::Schedule));
    TEST_ASSERT_EQUAL_STRING("mqtt", irrigator::domain::toString(irrigator::domain::RunReason::Mqtt));
    TEST_ASSERT_EQUAL_STRING("abort", irrigator::domain::toString(irrigator::domain::RunReason::Abort));
}

void test_rain_skip_self_expires() {
    irrigator::domain::RainSkip rainSkip;
    rainSkip.setSkipUntil(2000);
    TEST_ASSERT_TRUE(rainSkip.shouldSkipScheduledRun(1999));
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(2000));
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(2001));
}

// A never-armed RainSkip must never skip, including at epoch 0 — the >0 guard is what stops the
// default-constructed 0 deadline from reading as "skip everything before 1970".
void test_rain_skip_unset_never_skips_and_clear_disarms() {
    irrigator::domain::RainSkip rainSkip;
    TEST_ASSERT_TRUE(rainSkip.skipUntil() == 0);
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(0));
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(-1));

    rainSkip.setSkipUntil(5000);
    TEST_ASSERT_TRUE(rainSkip.skipUntil() == 5000);
    TEST_ASSERT_TRUE(rainSkip.shouldSkipScheduledRun(4999));

    // cmd/skip_rain with a cleared deadline (or a manual override) must resume scheduling at once.
    rainSkip.clear();
    TEST_ASSERT_TRUE(rainSkip.skipUntil() == 0);
    TEST_ASSERT_FALSE(rainSkip.shouldSkipScheduledRun(4999));
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

// start()'s three rejection paths, all of which main.ino's enqueueOrStart() relies on returning
// false rather than half-starting a run: an out-of-range zone, a zero-minute run, and a second
// start while one is already in progress.
void test_start_rejects_invalid_requests_and_leaves_state_untouched() {
    irrigator::domain::IrrigationStateMachine machine;

    TEST_ASSERT_FALSE(machine.start(
        {static_cast<uint8_t>(irrigator::board::ZONE_COUNT + 1), 5, irrigator::domain::RunReason::Manual}, 0));
    TEST_ASSERT_TRUE(machine.state() == irrigator::domain::IrrigationState::Idle);

    TEST_ASSERT_FALSE(machine.start({1, 0, irrigator::domain::RunReason::Manual}, 0));
    TEST_ASSERT_TRUE(machine.state() == irrigator::domain::IrrigationState::Idle);
    TEST_ASSERT_FALSE(machine.isActive());
}

void test_start_is_refused_while_a_run_is_already_watering() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({1, 10, irrigator::domain::RunReason::Manual}, 1000));
    TEST_ASSERT_FALSE(machine.canStartImmediately());

    // The second request must be queued by the caller, not silently replace the running zone.
    TEST_ASSERT_FALSE(machine.start({2, 10, irrigator::domain::RunReason::Mqtt}, 2000));
    TEST_ASSERT_EQUAL_UINT8(1, machine.snapshot(2000).activeZone);
}

// canStartImmediately() is deliberately true during PumpGracePeriod so a queued run hands off to
// the still-spinning pump instead of waiting out the full 60s tail (the case "Test all zones" hits
// on every zone transition). The handed-off run must then be timed from its own start, not cut
// short at the point the previous run's grace tail would have expired.
void test_queued_run_hands_off_during_pump_grace_period_without_inheriting_its_deadline() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({1, 1, irrigator::domain::RunReason::Schedule}, 0));
    machine.tick(60000UL);  // 1-minute run elapsed -> pump winding down
    TEST_ASSERT_TRUE(machine.state() == irrigator::domain::IrrigationState::PumpGracePeriod);
    TEST_ASSERT_TRUE(machine.canStartImmediately());

    TEST_ASSERT_TRUE(machine.start({2, 5, irrigator::domain::RunReason::Schedule}, 61000UL));
    TEST_ASSERT_TRUE(machine.state() == irrigator::domain::IrrigationState::Watering);
    TEST_ASSERT_EQUAL_UINT8(2, machine.snapshot(61000UL).activeZone);

    // The old tail would have ended at 60000 + PUMP_GRACEFUL_SHUTDOWN_MS; the new 5-minute run
    // must still be watering well past that point.
    machine.tick(60000UL + irrigator::board::PUMP_GRACEFUL_SHUTDOWN_MS);
    TEST_ASSERT_TRUE(machine.state() == irrigator::domain::IrrigationState::Watering);
    machine.tick(61000UL + 5UL * 60000UL);
    TEST_ASSERT_TRUE(machine.state() == irrigator::domain::IrrigationState::PumpGracePeriod);
}

// snapshot() is what feeds the display countdown and the MQTT state payload, so each state's
// pumpOn/elapsed/remaining combination matters on its own.
void test_snapshot_reports_pump_and_countdown_per_state() {
    irrigator::domain::IrrigationStateMachine machine;

    const auto idle = machine.snapshot(1234);
    TEST_ASSERT_TRUE(idle.state == irrigator::domain::IrrigationState::Idle);
    TEST_ASSERT_FALSE(idle.pumpOn);
    TEST_ASSERT_EQUAL_UINT32(0, idle.elapsedMs);
    TEST_ASSERT_EQUAL_UINT32(0, idle.remainingMs);

    TEST_ASSERT_TRUE(machine.start({3, 2, irrigator::domain::RunReason::Manual}, 10000UL));
    const auto watering = machine.snapshot(10000UL + 30000UL);
    TEST_ASSERT_TRUE(watering.pumpOn);
    TEST_ASSERT_EQUAL_UINT8(3, watering.activeZone);
    TEST_ASSERT_EQUAL_UINT32(30000UL, watering.elapsedMs);
    TEST_ASSERT_EQUAL_UINT32(90000UL, watering.remainingMs);  // 2min run, 30s in

    // Past the nominal end but before tick() has run: remaining floors at 0 rather than underflowing.
    TEST_ASSERT_EQUAL_UINT32(0, machine.snapshot(10000UL + 200000UL).remainingMs);

    machine.tick(10000UL + 2UL * 60000UL);
    const auto grace = machine.snapshot(10000UL + 2UL * 60000UL);
    TEST_ASSERT_TRUE(grace.state == irrigator::domain::IrrigationState::PumpGracePeriod);
    TEST_ASSERT_TRUE(grace.pumpOn);  // valve shut, pump still spinning down
    TEST_ASSERT_EQUAL_UINT32(0, grace.remainingMs);
}

void test_pump_only_snapshot_counts_down_against_zone_zero() {
    irrigator::domain::IrrigationStateMachine machine;
    TEST_ASSERT_TRUE(machine.start({0, 3, irrigator::domain::RunReason::Mqtt}, 0));
    const auto shot = machine.snapshot(60000UL);
    TEST_ASSERT_TRUE(shot.state == irrigator::domain::IrrigationState::PumpOnly);
    TEST_ASSERT_EQUAL_UINT8(0, shot.activeZone);
    TEST_ASSERT_TRUE(shot.pumpOn);
    TEST_ASSERT_EQUAL_UINT32(120000UL, shot.remainingMs);
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

// validateConfig is the gate parseConfig and the captive portal both run a candidate through; each
// of its three rules exercised directly, at the boundary, rather than only via a JSON payload.
void test_validate_config_rules_at_their_boundaries() {
    irrigator::domain::DeviceConfig config;
    TEST_ASSERT_FALSE(irrigator::domain::validateConfig(config));  // default: no SSID yet

    snprintf(config.ssid, sizeof(config.ssid), "%s", "home");
    TEST_ASSERT_TRUE(irrigator::domain::validateConfig(config));  // MQTT is optional

    // A broker without a usable port is the one MQTT combination that's rejected.
    snprintf(config.mqttBroker, sizeof(config.mqttBroker), "%s", "192.168.1.10");
    config.mqttPort = 0;
    TEST_ASSERT_FALSE(irrigator::domain::validateConfig(config));
    config.mqttPort = 1;
    TEST_ASSERT_TRUE(irrigator::domain::validateConfig(config));

    config.timezoneOffsetHours = irrigator::board::TIMEZONE_OFFSET_MAX_HOURS;
    TEST_ASSERT_TRUE(irrigator::domain::validateConfig(config));
    config.timezoneOffsetHours = irrigator::board::TIMEZONE_OFFSET_MIN_HOURS;
    TEST_ASSERT_TRUE(irrigator::domain::validateConfig(config));
    config.timezoneOffsetHours = static_cast<int8_t>(irrigator::board::TIMEZONE_OFFSET_MAX_HOURS + 1);
    TEST_ASSERT_FALSE(irrigator::domain::validateConfig(config));
    config.timezoneOffsetHours = static_cast<int8_t>(irrigator::board::TIMEZONE_OFFSET_MIN_HOURS - 1);
    TEST_ASSERT_FALSE(irrigator::domain::validateConfig(config));
}

// A default-constructed Settings has empty zone names, which validateSettings rejects — so
// makeDefaultZones() isn't a convenience, it's the only way to build a valid baseline.
void test_make_default_zones_is_the_valid_baseline_settings() {
    irrigator::domain::Settings bare;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(bare));

    const irrigator::domain::Settings defaults = irrigator::domain::makeDefaultZones();
    TEST_ASSERT_TRUE(irrigator::domain::validateSettings(defaults));
    TEST_ASSERT_EQUAL_STRING("Zone 1", defaults.zoneNames[0]);
    TEST_ASSERT_EQUAL_STRING("Zone 11", defaults.zoneNames[irrigator::board::ZONE_COUNT - 1]);
    for (uint8_t i = 0; i < irrigator::board::ZONE_COUNT; ++i) {
        TEST_ASSERT_TRUE(defaults.zoneEnabled[i]);
    }
}

// Every validateSettings rule, one field at a time off an otherwise-valid baseline — parseSettings
// rejects the whole file on any of these, so a rule silently going missing would let a bad
// settings.json through instead of falling back to defaults.
void test_validate_settings_rejects_each_out_of_range_field() {
    const irrigator::domain::Settings baseline = irrigator::domain::makeDefaultZones();

    irrigator::domain::Settings settings = baseline;
    settings.wateringPercentage = 300;
    TEST_ASSERT_TRUE(irrigator::domain::validateSettings(settings));  // 300 is the inclusive ceiling
    settings.wateringPercentage = 301;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings.wateringPercentage = -1;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));

    settings = baseline;
    settings.defaultManualDuration = 0;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings.defaultManualDuration = static_cast<uint8_t>(irrigator::board::MANUAL_DURATION_MAX_MINUTES + 1);
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));

    settings = baseline;
    settings.zoneNames[4][0] = '\0';  // a blank name would render as an empty menu row
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));

    settings = baseline;
    settings.valveCurrentBaseLevel = 3301;  // above the ADC's full-scale mV range
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings = baseline;
    settings.pumpCurrentBaseLevel = 3301;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));

    settings = baseline;
    settings.latitude = 90.1f;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings = baseline;
    settings.longitude = -180.1f;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));

    settings = baseline;
    settings.upperLimitPercent = 39;  // below lowerLimitPercent's default 40
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings = baseline;
    settings.lowerLimitPercent = -1;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings = baseline;
    settings.upperLimitPercent = 501;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));

    settings = baseline;
    settings.rainSkipProbabilityPercent = 101;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
    settings = baseline;
    settings.rainSkipAmountMm = -1;
    TEST_ASSERT_FALSE(irrigator::domain::validateSettings(settings));
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

// The round-trip above pins the Watering group's scalars; this covers the three groups it doesn't
// (Zones names + enabled, Location, and the Watering group's string field), which is what boot-time
// persist-after-parse actually has to preserve when it rewrites settings.json in the nested shape.
void test_settings_store_round_trips_zones_location_and_api_key() {
    irrigator::domain::Settings settings = irrigator::domain::makeDefaultZones();
    snprintf(settings.zoneNames[0], sizeof(settings.zoneNames[0]), "%s", "Front lawn");
    snprintf(settings.zoneNames[10], sizeof(settings.zoneNames[10]), "%s", "Trees right");
    settings.zoneEnabled[3] = false;
    settings.locationKnown = true;
    settings.latitude = 32.25f;    // exactly representable, so no serializer rounding to reason about
    settings.longitude = -117.5f;
    snprintf(settings.city, sizeof(settings.city), "%s", "Testville");
    snprintf(settings.weatherApiKey, sizeof(settings.weatherApiKey), "%s", "abc123deadbeef");
    settings.defaultManualDuration = 9;
    settings.rainSkipProbabilityPercent = 55;
    settings.rainSkipAmountMm = 7;

    char buffer[irrigator::board::SETTINGS_JSON_MAX_BYTES];
    const size_t written = irrigator::domain::serializeSettings(settings, buffer, sizeof(buffer));
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE(written < sizeof(buffer));  // not truncated against SETTINGS_JSON_MAX_BYTES

    irrigator::domain::Settings roundTripped;
    TEST_ASSERT_TRUE(irrigator::domain::parseSettings(buffer, written, roundTripped));
    TEST_ASSERT_EQUAL_STRING("Front lawn", roundTripped.zoneNames[0]);
    TEST_ASSERT_EQUAL_STRING("Trees right", roundTripped.zoneNames[10]);
    TEST_ASSERT_FALSE(roundTripped.zoneEnabled[3]);
    TEST_ASSERT_TRUE(roundTripped.zoneEnabled[4]);
    TEST_ASSERT_TRUE(roundTripped.locationKnown);
    TEST_ASSERT_EQUAL_FLOAT(32.25f, roundTripped.latitude);
    TEST_ASSERT_EQUAL_FLOAT(-117.5f, roundTripped.longitude);
    TEST_ASSERT_EQUAL_STRING("Testville", roundTripped.city);
    TEST_ASSERT_EQUAL_STRING("abc123deadbeef", roundTripped.weatherApiKey);
    TEST_ASSERT_EQUAL_UINT8(9, roundTripped.defaultManualDuration);
    TEST_ASSERT_EQUAL_INT16(55, roundTripped.rainSkipProbabilityPercent);
    TEST_ASSERT_EQUAL_INT16(7, roundTripped.rainSkipAmountMm);
}

// serializeJson() truncates against an undersized buffer and still reports a nonzero byte count
// (see the 255-record test's note), so a nonzero return is NOT proof of success. Pinning the
// consequence: the truncated output does not parse back, which is what a caller has to rely on.
void test_serialize_into_undersized_buffer_produces_output_that_does_not_parse_back() {
    irrigator::domain::DeviceConfig config;
    snprintf(config.ssid, sizeof(config.ssid), "%s", "a-fairly-long-network-name");
    snprintf(config.mqttBroker, sizeof(config.mqttBroker), "%s", "192.168.1.10");

    char tiny[24];
    const size_t written = irrigator::domain::serializeConfig(config, tiny, sizeof(tiny));
    irrigator::domain::DeviceConfig parsed;
    TEST_ASSERT_FALSE(irrigator::domain::parseConfig(tiny, written, parsed));

    irrigator::domain::ScheduleList list;
    list.records[0] = {1, 1, "06:30", 20, true};
    list.count = 1;
    char tinySchedule[16];
    const size_t scheduleWritten =
        irrigator::domain::serializeSchedule(list, tinySchedule, sizeof(tinySchedule));
    irrigator::domain::ScheduleList parsedList;
    TEST_ASSERT_FALSE(
        irrigator::domain::parseSchedule(tinySchedule, scheduleWritten, parsedList));
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

// The Zones merge accepts the current {"name":..,"enabled":..} object form and the legacy
// plain-string form ("3": "Trees") that predates `enabled` — both still arrive from older HA
// packages, and the string form must leave `enabled` alone rather than resetting it.
void test_merge_settings_accepts_both_zone_shapes() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    existing.zoneEnabled[2] = false;

    const char* json = R"({"Zones":{"1":{"name":"Front"},"3":"Trees"}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_EQUAL_STRING("Front", merged.zoneNames[0]);
    TEST_ASSERT_TRUE(merged.zoneEnabled[0]);  // object form omitted `enabled` -> untouched
    TEST_ASSERT_EQUAL_STRING("Trees", merged.zoneNames[2]);
    TEST_ASSERT_FALSE(merged.zoneEnabled[2]);  // string form can't carry `enabled` -> untouched
}

// zoneNames is char[16], so a longer name would silently truncate into something the user didn't
// choose. It's rejected (and reported per-zone) instead, keeping the existing name.
void test_merge_settings_rejects_oversized_zone_name_per_zone() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    snprintf(existing.zoneNames[2], sizeof(existing.zoneNames[2]), "%s", "Back");

    const char* json = R"({"Zones":{"3":{"name":"a name well past sixteen characters"}}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("Zones.3.name", rejected);
    TEST_ASSERT_EQUAL_STRING("Back", merged.zoneNames[2]);
}

// A zone entry that's neither an object nor a string has nothing usable in it at all — rejected as
// a whole rather than partially interpreted.
void test_merge_settings_rejects_wrongly_typed_zone_entry() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();

    const char* json = R"({"Zones":{"2":42}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("Zones.2", rejected);
    TEST_ASSERT_EQUAL_STRING("Zone 2", merged.zoneNames[1]);
}

void test_merge_settings_updates_weatherapi_key_and_rejects_oversized_one() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();

    const char* json = R"({"Watering":{"weatherapi_key":"0123456789abcdef"}})";
    irrigator::domain::Settings merged;
    char rejected[256];
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existing, json, strlen(json), merged, rejected,
                                                      sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("", rejected);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", merged.weatherApiKey);

    // The field is char[48]; a key that wouldn't fit is rejected rather than stored half-truncated
    // (a truncated key fails every WeatherAPI call with no obvious cause).
    const char* oversized =
        R"({"Watering":{"weatherapi_key":"0123456789012345678901234567890123456789012345678"}})";
    irrigator::domain::Settings mergedOversized;
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(merged, oversized, strlen(oversized),
                                                      mergedOversized, rejected, sizeof(rejected)));
    TEST_ASSERT_EQUAL_STRING("Watering.weatherapi_key", rejected);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", mergedOversized.weatherApiKey);
}

// The rejected-fields buffer is a best-effort alert payload: a null pointer, a zero size, or a
// buffer too small for the field names must all truncate quietly rather than fail the merge (or
// write past the end of the caller's array).
void test_merge_tolerates_absent_or_undersized_rejected_fields_buffer() {
    irrigator::domain::DeviceConfig existingConfig;
    snprintf(existingConfig.ssid, sizeof(existingConfig.ssid), "%s", "home");
    const char* badTimezone = R"({"ssid":"newhome","timezone":99})";
    irrigator::domain::DeviceConfig mergedConfig;

    TEST_ASSERT_TRUE(irrigator::domain::mergeConfig(existingConfig, badTimezone, strlen(badTimezone),
                                                    mergedConfig, nullptr, 0));
    TEST_ASSERT_EQUAL_STRING("newhome", mergedConfig.ssid);

    // Undersized: "timezone" needs 9 bytes with its terminator, so it lands truncated but
    // terminated, and the merge still applies everything that passed.
    char cramped[5] = {'x', 'x', 'x', 'x', 'x'};
    TEST_ASSERT_TRUE(irrigator::domain::mergeConfig(existingConfig, badTimezone, strlen(badTimezone),
                                                    mergedConfig, cramped, sizeof(cramped)));
    TEST_ASSERT_TRUE(strlen(cramped) < sizeof(cramped));
    TEST_ASSERT_EQUAL_STRING("newhome", mergedConfig.ssid);

    irrigator::domain::Settings existingSettings = irrigator::domain::makeDefaultZones();
    const char* badPercentage = R"({"Watering":{"watering_percentage":500}})";
    irrigator::domain::Settings mergedSettings;
    TEST_ASSERT_TRUE(irrigator::domain::mergeSettings(existingSettings, badPercentage,
                                                      strlen(badPercentage), mergedSettings, nullptr,
                                                      0));
    TEST_ASSERT_EQUAL_INT16(existingSettings.wateringPercentage, mergedSettings.wateringPercentage);
}

void test_merge_settings_rejects_malformed_json_outright() {
    irrigator::domain::Settings existing = irrigator::domain::makeDefaultZones();
    irrigator::domain::Settings merged;
    char rejected[64];
    const char* malformed = "{not json";
    TEST_ASSERT_FALSE(irrigator::domain::mergeSettings(existing, malformed, strlen(malformed), merged,
                                                       rejected, sizeof(rejected)));
    // A JSON array parses fine but isn't an object to merge from either.
    const char* notAnObject = "[1,2,3]";
    TEST_ASSERT_FALSE(irrigator::domain::mergeSettings(existing, notAnObject, strlen(notAnObject),
                                                       merged, rejected, sizeof(rejected)));
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

// Every reason a record is passed over, in one schedule: inactive, wrong day, wrong hour, wrong
// minute. Only the last record matches, which also proves matching doesn't stop at the first miss.
void test_schedule_engine_skips_inactive_and_non_matching_records() {
    irrigator::domain::ScheduleEngine engine;
    irrigator::domain::ScheduleList schedule;
    schedule.records[0] = {1, 3, "06:30", 20, false};  // inactive
    schedule.records[1] = {2, 4, "06:30", 20, true};   // wrong day
    schedule.records[2] = {3, 3, "07:30", 20, true};   // wrong hour
    schedule.records[3] = {4, 3, "06:31", 20, true};   // wrong minute
    schedule.records[4] = {5, 3, "06:30", 20, true};   // the only match
    schedule.count = 5;

    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t count =
        engine.tick(now, 600000, schedule, true, noSkip, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(1, count);
    TEST_ASSERT_EQUAL_UINT8(5, due[0].zone);
    TEST_ASSERT_TRUE(due[0].reason == irrigator::domain::RunReason::Schedule);
}

// Several zones legitimately share one start time (staggered by the queue downstream), so a single
// minute can produce more than one due request.
void test_schedule_engine_returns_every_record_matching_the_same_minute() {
    irrigator::domain::ScheduleEngine engine;
    irrigator::domain::ScheduleList schedule;
    schedule.records[0] = {1, 3, "06:30", 20, true};
    schedule.records[1] = {2, 3, "06:30", 30, true};
    schedule.records[2] = {3, 3, "06:30", 40, true};
    schedule.count = 3;

    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t count =
        engine.tick(now, 700000, schedule, true, noSkip, 100, due, irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(3, count);
    TEST_ASSERT_EQUAL_UINT8(1, due[0].zone);
    TEST_ASSERT_EQUAL_UINT8(3, due[2].zone);
    TEST_ASSERT_EQUAL_UINT16(40, due[2].durationMin);
}

// dueCapacity is the caller's array bound (main.ino's runScheduleEngine sizes it deliberately) —
// more matches than that must stop at the bound rather than writing past the end of it.
void test_schedule_engine_never_writes_past_due_capacity() {
    irrigator::domain::ScheduleEngine engine;
    irrigator::domain::ScheduleList schedule;
    schedule.records[0] = {1, 3, "06:30", 20, true};
    schedule.records[1] = {2, 3, "06:30", 20, true};
    schedule.records[2] = {3, 3, "06:30", 20, true};
    schedule.count = 3;

    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[3] = {};

    const uint8_t count = engine.tick(now, 800000, schedule, true, noSkip, 100, due, 2);
    TEST_ASSERT_EQUAL_UINT8(2, count);
    TEST_ASSERT_EQUAL_UINT8(1, due[0].zone);
    TEST_ASSERT_EQUAL_UINT8(2, due[1].zone);
    TEST_ASSERT_EQUAL_UINT8(0, due[2].zone);  // untouched
}

// The once-per-minute edge trigger must re-arm as the clock advances — the existing "fires once"
// test only shows a later minute *not* firing (because nothing matched it); this shows a later
// minute firing again for a record that does match it.
void test_schedule_engine_fires_again_in_the_next_matching_minute() {
    irrigator::domain::ScheduleEngine engine;
    irrigator::domain::ScheduleList schedule;
    schedule.records[0] = {1, 3, "06:30", 20, true};
    schedule.records[1] = {2, 3, "06:31", 25, true};
    schedule.count = 2;

    irrigator::domain::RainSkip noSkip;
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    const uint8_t first = engine.tick({3, 6, 30}, 900000, schedule, true, noSkip, 100, due,
                                      irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(1, first);
    TEST_ASSERT_EQUAL_UINT8(1, due[0].zone);

    const uint8_t second = engine.tick({3, 6, 31}, 900060, schedule, true, noSkip, 100, due,
                                       irrigator::board::SCHEDULE_MAX_RECORDS);
    TEST_ASSERT_EQUAL_UINT8(1, second);
    TEST_ASSERT_EQUAL_UINT8(2, due[0].zone);
    TEST_ASSERT_EQUAL_UINT16(25, due[0].durationMin);
}

// A rain skip that expires mid-window must not swallow later runs: the check is re-evaluated
// against nowEpochSeconds every minute, not latched when the skip was armed.
void test_schedule_engine_resumes_once_the_rain_skip_deadline_passes() {
    irrigator::domain::ScheduleEngine engine;
    const irrigator::domain::ScheduleList schedule = makeOneRecordSchedule();
    const irrigator::domain::WallClock now{3, 6, 30};
    irrigator::domain::RainSkip skip;
    skip.setSkipUntil(1000000);
    irrigator::domain::WateringRequest due[irrigator::board::SCHEDULE_MAX_RECORDS];

    TEST_ASSERT_EQUAL_UINT8(0, engine.tick(now, 999960, schedule, true, skip, 100, due,
                                           irrigator::board::SCHEDULE_MAX_RECORDS));
    TEST_ASSERT_EQUAL_UINT8(1, engine.tick(now, 1000020, schedule, true, skip, 100, due,
                                           irrigator::board::SCHEDULE_MAX_RECORDS));
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

void test_ui_state_machine_watering_active_long_click_opens_menu() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 200);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
}

// Rotation/click while watering is active queue the next watering rather than being ignored —
// this is the UI-side half of the watering queue (see enqueueOrStart() in main.ino, which already
// pushes every request through the queue regardless of whether it can start immediately).
void test_ui_state_machine_watering_active_rotation_queues_next_watering() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ctx.defaultManualDuration = 5;

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ZonePicker);
    TEST_ASSERT_EQUAL_UINT8(1, ui.pendingZone());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DurationPicker);
    TEST_ASSERT_EQUAL_UINT16(5, ui.pendingDuration());

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);
    TEST_ASSERT_EQUAL_UINT16(6, ui.pendingDuration());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::StartWatering);
    TEST_ASSERT_EQUAL_UINT8(1, intent.zone);
    TEST_ASSERT_EQUAL_UINT16(6, intent.duration);
    TEST_ASSERT_TRUE(intent.boolValue);  // flags main.ino that this went behind the active run
    // Unlike the not-watering flow, this does NOT jump to Normal itself — the caller (main.ino's
    // handleUiIntent) is expected to actually push the request and then call showQueueAddResult(),
    // which is what moves the screen on (same imperative-override pattern as CalibrateConfirm).
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DurationPicker);
}

// While watering, the Normal screen's big line is the *running* zone (IrrigatorUi::renderNormal's
// `running` branch), so both gestures have to move relative to that zone: a bare click means
// "queue zone 7 again" and lands straight on zone 7's duration, and a rotation steps to its
// neighbour — not to PUMP, which is what both used to do.
void test_ui_state_machine_watering_active_gestures_start_from_running_zone() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ctx.activeZone = 7;
    ctx.defaultManualDuration = 5;

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DurationPicker);
    TEST_ASSERT_EQUAL_UINT8(7, ui.pendingZone());
    TEST_ASSERT_EQUAL_UINT16(5, ui.pendingDuration());

    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 200);  // cancel -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ZonePicker);
    TEST_ASSERT_EQUAL_UINT8(8, ui.pendingZone());
}

// The running zone wins over the next scheduled one: with a schedule active, the screen still
// shows what's watering right now, so that's what the knob has to move relative to.
void test_ui_state_machine_watering_active_running_zone_beats_next_scheduled() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ctx.activeZone = 7;
    ctx.scheduleActive = true;
    ctx.nextScheduledZone = 2;

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ZonePicker);
    TEST_ASSERT_EQUAL_UINT8(6, ui.pendingZone());
}

// A pump-only run shows PUMP on the Normal screen (activeZone stays 0), so a bare click keeps the
// pump-only default rather than jumping into a zone.
void test_ui_state_machine_pump_only_run_click_keeps_pump() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ctx.activeZone = 0;
    ctx.defaultManualDuration = 5;

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);  // wake -> Normal
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DurationPicker);
    TEST_ASSERT_EQUAL_UINT8(0, ui.pendingZone());
}

void test_ui_state_machine_queue_add_result_shows_outcome_and_ok_returns_to_normal() {
    irrigator::domain::UiStateMachine ui;
    ui.wake(0);

    ui.showQueueAddResult("2.Front lawn", 6, true, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::QueueAddResult);
    TEST_ASSERT_TRUE(ui.queueAddSucceeded());
    TEST_ASSERT_EQUAL_STRING("2.Front lawn", ui.queueAddZoneLabel());
    TEST_ASSERT_EQUAL_UINT16(6, ui.queueAddDuration());

    // Rotation is a no-op on this screen — nothing to navigate to.
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 150);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::QueueAddResult);

    // Click (the OK button) dismisses back to Normal.
    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

void test_ui_state_machine_stop_confirm_flow() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());  // Queue is item 0 while watering

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 150);  // Queue -> Stop
    TEST_ASSERT_EQUAL_UINT16(1, ui.selectedIndex());

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
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Queue -> Stop
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Stop -> Stop all
    TEST_ASSERT_EQUAL_UINT16(2, ui.selectedIndex());

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
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Queue -> Stop
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Stop -> Stop all
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

// menuMainLayout is the single definition both the renderer and handleMenuMain read; pinning both
// shapes directly means a row insertion can't quietly shift one consumer and not the other (which
// is exactly how BACK broke once before — see kSettingsItemCount's comment).
void test_ui_state_machine_menu_main_layout_shifts_by_three_while_watering() {
    const auto idle = irrigator::domain::UiStateMachine::menuMainLayout(false);
    TEST_ASSERT_EQUAL_UINT8(5, idle.itemCount);
    TEST_ASSERT_EQUAL_UINT8(0, idle.scheduleIndex);
    TEST_ASSERT_EQUAL_UINT8(1, idle.testAllIndex);
    TEST_ASSERT_EQUAL_UINT8(2, idle.settingsIndex);
    TEST_ASSERT_EQUAL_UINT8(3, idle.infoIndex);
    TEST_ASSERT_EQUAL_UINT8(4, idle.backIndex);

    // [Queue, Stop, Stop all] take indices 0, 1 and 2, pushing everything else down by exactly 3.
    const auto watering = irrigator::domain::UiStateMachine::menuMainLayout(true);
    TEST_ASSERT_EQUAL_UINT8(8, watering.itemCount);
    TEST_ASSERT_EQUAL_UINT8(0, watering.queueIndex);
    TEST_ASSERT_EQUAL_UINT8(1, watering.stopIndex);
    TEST_ASSERT_EQUAL_UINT8(2, watering.stopAllIndex);
    TEST_ASSERT_EQUAL_UINT8(3, watering.scheduleIndex);
    TEST_ASSERT_EQUAL_UINT8(4, watering.testAllIndex);
    TEST_ASSERT_EQUAL_UINT8(5, watering.settingsIndex);
    TEST_ASSERT_EQUAL_UINT8(6, watering.infoIndex);
    TEST_ASSERT_EQUAL_UINT8(7, watering.backIndex);
}

// Queue only appears while watering, and clicking it opens a read-only submenu rather than
// confirming an action (unlike Stop/Stop all).
void test_ui_state_machine_menu_main_queue_item_opens_menu_queue() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());  // Queue is item 0 while watering

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);  // -> MenuQueue
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuQueue);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());
}

// With nothing queued behind the active run, MenuQueue is a one-row list (just BACK) — same shape
// as MenuSchedule with zero records — and any click has to leave immediately.
void test_ui_state_machine_menu_queue_empty_shows_only_back() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ctx.queueSize = 0;
    TEST_ASSERT_EQUAL_UINT16(1, irrigator::domain::UiStateMachine::queueMenuItemCount(0));

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Queue -> MenuQueue
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuQueue);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // single item: no movement
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());
}

// With items queued, MenuQueue lists them (read-only — clicking a data row is a no-op) followed by
// a trailing BACK row that actually navigates back.
void test_ui_state_machine_menu_queue_with_items_lists_them_then_back() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = true;
    ctx.queueSize = 3;
    TEST_ASSERT_EQUAL_UINT16(4, irrigator::domain::UiStateMachine::queueMenuItemCount(3));

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Queue -> MenuQueue
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuQueue);

    // Clicking a data row (not BACK) does nothing — the queue is view-only from this screen.
    irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuQueue);

    for (int i = 0; i < 3; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(3, ui.selectedIndex());  // BACK is the 4th row (index 3)

    intent = ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());
}

// The first event off a blank display only wakes it — a rotation must not also land the user in
// the zone picker they never meant to open.
void test_ui_state_machine_first_input_from_display_off_only_wakes() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DisplayOff);

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 1000);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

// The 20s timeout is measured from the last input, not from when the screen was first shown, so
// continued interaction keeps the display alive indefinitely.
void test_ui_state_machine_every_input_rearms_the_interaction_timeout() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 1000);  // wake -> Normal

    ui.tick(15000);
    TEST_ASSERT_TRUE(ui.screen() != irrigator::domain::UiScreen::DisplayOff);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 15000);  // -> MenuMain, re-armed

    ui.tick(1000 + irrigator::board::ACTIVE_INTERACTION_TIMEOUT_MS);  // the original deadline
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);

    ui.tick(15000 + irrigator::board::ACTIVE_INTERACTION_TIMEOUT_MS);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DisplayOff);
}

// Blanking abandons an unsaved edit rather than resuming it on the next wake — otherwise the user
// comes back to a half-adjusted value with no memory of having started it.
void test_ui_state_machine_timeout_abandons_an_in_progress_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringPercentage = 100;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings
    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexPercentage; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter percentage edit
    TEST_ASSERT_TRUE(ui.editing());

    ui.tick(400 + irrigator::board::ACTIVE_INTERACTION_TIMEOUT_MS);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::DisplayOff);
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());

    // Waking lands back on Normal, not on the abandoned edit.
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 100000);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);
}

// The Normal screen shows the next scheduled zone when idle+scheduled, so rotation must move
// relative to that rather than jumping back to PUMP first (see handleNormal's `displayedZone`).
void test_ui_state_machine_normal_rotation_starts_from_the_displayed_scheduled_zone() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleActive = true;
    ctx.nextScheduledZone = 5;

    ui.wake(0);
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::ZonePicker);
    TEST_ASSERT_EQUAL_UINT8(6, ui.pendingZone());

    irrigator::domain::UiStateMachine back;
    back.wake(0);
    back.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 100);
    TEST_ASSERT_EQUAL_UINT8(4, back.pendingZone());

    // With scheduling off there's nothing displayed to move relative to, so it falls back to PUMP.
    ctx.scheduleActive = false;
    irrigator::domain::UiStateMachine unscheduled;
    unscheduled.wake(0);
    unscheduled.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 100);
    TEST_ASSERT_EQUAL_UINT8(1, unscheduled.pendingZone());
}

// The per-record Active row is a direct toggle (no edit step), same shape as the Settings menu's
// boolean rows — it emits the inverted value and leaves the cursor where it was.
void test_ui_state_machine_schedule_active_field_is_a_direct_toggle() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 1;
    ctx.selectedScheduleActive = true;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule
    for (int i = 0; i < irrigator::domain::UiStateMachine::kScheduleFieldActive; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300 + i);
    }
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kScheduleFieldActive, ui.selectedIndex());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::SetScheduleActive);
    TEST_ASSERT_EQUAL_UINT8(0, intent.scheduleRecordIndex);
    TEST_ASSERT_FALSE(intent.boolValue);  // inverted from ctx.selectedScheduleActive
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSchedule);
}

// With no records, MenuSchedule is a one-row list (just BACK) — any click has to leave rather than
// trying to edit a field of a record that doesn't exist.
void test_ui_state_machine_menu_schedule_with_no_records_only_navigates_back() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.scheduleRecordCount = 0;
    TEST_ASSERT_EQUAL_UINT16(1, irrigator::domain::UiStateMachine::scheduleItemCount(0));

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 200);      // Schedule -> MenuScheduleRoot
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 250);      // View/Edit -> MenuSchedule
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuSchedule);

    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 300);  // single item: no movement
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());

    const irrigator::domain::UiIntent intent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuScheduleRoot);
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kMenuScheduleRootIndexViewEdit,
                            ui.selectedIndex());
}

// One counter-clockwise tick off the first row is the fastest way to BACK, and clicking it must
// actually leave — the failure mode kSettingsItemCount's comment describes (a row added to the
// renderer but not here) shows up exactly as this click doing nothing.
void test_ui_state_machine_settings_back_row_is_one_tick_backwards_and_exits() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);  // Schedule -> Test all zones
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);  // Test all zones -> Settings
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);      // -> MenuSettings
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::CounterClockwise, ctx, 350);
    TEST_ASSERT_EQUAL_UINT16(irrigator::domain::UiStateMachine::kSettingsIndexBack, ui.selectedIndex());

    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::MenuMain);
    TEST_ASSERT_EQUAL_UINT16(0, ui.selectedIndex());
}

// The factory-reset confirm is forced in from a hardware BOOT-button hold, not reached by rotary
// navigation — it must override whatever screen (and unsaved edit) was showing.
void test_ui_state_machine_factory_reset_confirm_overrides_any_screen_and_edit() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringPercentage = 100;
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 0);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 100);  // -> MenuMain
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 200);
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 250);
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 300);  // -> MenuSettings
    for (uint8_t i = 0; i < irrigator::domain::UiStateMachine::kSettingsIndexPercentage; ++i) {
        ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 350 + i);
    }
    ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 400);  // enter percentage edit
    TEST_ASSERT_TRUE(ui.editing());

    ui.enterFactoryResetConfirm(500);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::FactoryResetConfirm);
    TEST_ASSERT_FALSE(ui.editing());
    TEST_ASSERT_FALSE(ui.factoryResetConfirmYesSelected());  // NO is always the default
}

void test_ui_state_machine_factory_reset_confirm_no_cancels_and_yes_emits_intent() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;

    ui.enterFactoryResetConfirm(0);
    const irrigator::domain::UiIntent noIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 100);
    TEST_ASSERT_TRUE(noIntent.type == irrigator::domain::UiIntentType::None);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    // LongClick cancels too, without ever selecting anything.
    ui.enterFactoryResetConfirm(200);
    ui.handleInput(irrigator::domain::UiInputEvent::LongClick, ctx, 300);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::Normal);

    ui.enterFactoryResetConfirm(400);
    ui.handleInput(irrigator::domain::UiInputEvent::Clockwise, ctx, 500);
    TEST_ASSERT_TRUE(ui.factoryResetConfirmYesSelected());
    const irrigator::domain::UiIntent yesIntent =
        ui.handleInput(irrigator::domain::UiInputEvent::Click, ctx, 600);
    TEST_ASSERT_TRUE(yesIntent.type == irrigator::domain::UiIntentType::FactoryReset);
    // Same imperative-override pattern as CalibrateConfirm: the caller performs the reset and then
    // forces the result screen, so this stays put in the meantime.
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::FactoryResetConfirm);
}

// FactoryResetResult is a true terminal screen — main.ino restarts the device right after showing
// it, so no rotary input may navigate away and leave a live UI on a wiped configuration.
void test_ui_state_machine_factory_reset_result_is_terminal() {
    irrigator::domain::UiStateMachine ui;
    irrigator::domain::UiStateMachine::Context ctx;

    ui.showFactoryResetResult(true, 0);
    TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::FactoryResetResult);
    TEST_ASSERT_TRUE(ui.factoryResetSucceeded());

    const irrigator::domain::UiInputEvent everyEvent[] = {
        irrigator::domain::UiInputEvent::Clockwise, irrigator::domain::UiInputEvent::CounterClockwise,
        irrigator::domain::UiInputEvent::Click, irrigator::domain::UiInputEvent::LongClick};
    for (const irrigator::domain::UiInputEvent event : everyEvent) {
        const irrigator::domain::UiIntent intent = ui.handleInput(event, ctx, 100);
        TEST_ASSERT_TRUE(intent.type == irrigator::domain::UiIntentType::None);
        TEST_ASSERT_TRUE(ui.screen() == irrigator::domain::UiScreen::FactoryResetResult);
    }

    ui.showFactoryResetResult(false, 200);
    TEST_ASSERT_FALSE(ui.factoryResetSucceeded());
}

// The result screens copy caller-supplied text into fixed 16/24-byte fields. A message longer than
// that must truncate safely (and a null one must become empty) rather than run off the buffer —
// these strings come straight from main.ino's formatting, not from a bounded enum.
void test_ui_state_machine_result_screens_bound_caller_supplied_strings() {
    irrigator::domain::UiStateMachine ui;

    ui.showCalibrateResult(false, 0, "a message far longer than fifteen", false, 0, nullptr, 0);
    TEST_ASSERT_EQUAL_STRING("a message far l", ui.calibrateValveMessage());  // char[16] -> 15 chars
    TEST_ASSERT_EQUAL_STRING("", ui.calibratePumpMessage());

    ui.showQueueAddResult(nullptr, 6, false, 100);
    TEST_ASSERT_EQUAL_STRING("", ui.queueAddZoneLabel());
    TEST_ASSERT_FALSE(ui.queueAddSucceeded());

    ui.showQueueAddResult("11.a zone name that is far too long", 6, true, 200);
    TEST_ASSERT_EQUAL_STRING("11.a zone name that is ", ui.queueAddZoneLabel());  // char[24] -> 23
}

void test_calendar_math_round_trips_and_weekday() {
    const int64_t days = irrigator::domain::daysFromCivil(2024, 3, 31);
    const irrigator::domain::CivilDate civil = irrigator::domain::civilFromDays(days);
    TEST_ASSERT_EQUAL_INT(2024, civil.year);
    TEST_ASSERT_EQUAL_UINT(3, civil.month);
    TEST_ASSERT_EQUAL_UINT(31, civil.day);
    TEST_ASSERT_EQUAL_INT(0, irrigator::domain::weekdayFromDays(days));  // Sunday
}

void test_calendar_math_epoch_day_zero_is_thursday_1970_01_01() {
    TEST_ASSERT_TRUE(irrigator::domain::daysFromCivil(1970, 1, 1) == 0);
    const irrigator::domain::CivilDate civil = irrigator::domain::civilFromDays(0);
    TEST_ASSERT_EQUAL_INT(1970, civil.year);
    TEST_ASSERT_EQUAL_UINT(1, civil.month);
    TEST_ASSERT_EQUAL_UINT(1, civil.day);
    TEST_ASSERT_EQUAL_INT(4, irrigator::domain::weekdayFromDays(0));  // Thursday
}

// Gregorian leap rules: 2024 and 2000 have a Feb 29, 1900 does not. DstRules walks backwards from
// March 31 to find the last Sunday, so a wrong day count here silently moves every DST transition.
void test_calendar_math_handles_gregorian_leap_rules() {
    TEST_ASSERT_TRUE(irrigator::domain::daysFromCivil(2024, 3, 1) -
                         irrigator::domain::daysFromCivil(2024, 2, 28) == 2);
    TEST_ASSERT_TRUE(irrigator::domain::daysFromCivil(2000, 3, 1) -
                         irrigator::domain::daysFromCivil(2000, 2, 28) == 2);
    TEST_ASSERT_TRUE(irrigator::domain::daysFromCivil(1900, 3, 1) -
                         irrigator::domain::daysFromCivil(1900, 2, 28) == 1);

    const irrigator::domain::CivilDate leapDay =
        irrigator::domain::civilFromDays(irrigator::domain::daysFromCivil(2024, 2, 29));
    TEST_ASSERT_EQUAL_INT(2024, leapDay.year);
    TEST_ASSERT_EQUAL_UINT(2, leapDay.month);
    TEST_ASSERT_EQUAL_UINT(29, leapDay.day);
}

// Negative day counts (pre-1970) take a different branch in weekdayFromDays, because C++'s % keeps
// the sign of the dividend — an unclamped RTC read or a bad NTP response lands here.
void test_calendar_math_weekday_for_pre_epoch_days() {
    TEST_ASSERT_TRUE(irrigator::domain::daysFromCivil(1969, 12, 31) == -1);
    TEST_ASSERT_EQUAL_INT(3, irrigator::domain::weekdayFromDays(-1));  // Wed 1969-12-31
    TEST_ASSERT_EQUAL_INT(0, irrigator::domain::weekdayFromDays(-4));  // Sun 1969-12-28, branch edge
    TEST_ASSERT_EQUAL_INT(5, irrigator::domain::weekdayFromDays(-6));  // Fri 1969-12-26
    TEST_ASSERT_EQUAL_INT(6, irrigator::domain::weekdayFromDays(-5));  // Sat 1969-12-27

    const irrigator::domain::CivilDate civil = irrigator::domain::civilFromDays(-1);
    TEST_ASSERT_EQUAL_INT(1969, civil.year);
    TEST_ASSERT_EQUAL_UINT(12, civil.month);
    TEST_ASSERT_EQUAL_UINT(31, civil.day);
}

// formatIso8601Utc is the exact inverse of epochFromUtc and is what stamps outgoing timestamps
// (cmd/skip_rain echoes, last-update reporting); the buffer is spec'd at 20 bytes, exactly the
// 19 characters plus a terminator.
void test_format_iso8601_utc_round_trips_and_fits_20_bytes() {
    char buffer[20];
    irrigator::domain::formatIso8601Utc(irrigator::domain::epochFromUtc(2026, 8, 9, 13, 5, 7), buffer,
                                        sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("2026-08-09T13:05:07", buffer);
    TEST_ASSERT_EQUAL_UINT(19, strlen(buffer));

    irrigator::domain::formatIso8601Utc(0, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00", buffer);

    // Midnight and the last second of a day are the two rollover points.
    irrigator::domain::formatIso8601Utc(irrigator::domain::epochFromUtc(2024, 2, 29, 23, 59, 59),
                                        buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("2024-02-29T23:59:59", buffer);
}

// A negative epoch must floor to the previous day rather than truncating toward zero, or the
// seconds-of-day term goes negative and the formatted time is nonsense.
void test_format_iso8601_utc_handles_pre_epoch_seconds() {
    char buffer[20];
    irrigator::domain::formatIso8601Utc(-1, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("1969-12-31T23:59:59", buffer);

    irrigator::domain::formatIso8601Utc(-86400, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("1969-12-31T00:00:00", buffer);
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

void test_standard_offset_from_geolocation_is_season_independent() {
    using irrigator::domain::epochFromUtc;
    using irrigator::domain::standardUtcOffsetHoursFromCurrent;

    const int64_t summer = epochFromUtc(2024, 6, 15, 12, 0, 0);
    const int64_t winter = epochFromUtc(2024, 1, 15, 12, 0, 0);

    // The property that matters: ip-api reports +3h for Central Europe in summer and +2h in
    // winter, and both must store the same standard +2 — otherwise the offset a device happens to
    // resolve in July drifts by an hour come October, when Clock re-applies DST on top.
    TEST_ASSERT_EQUAL_INT8(2, standardUtcOffsetHoursFromCurrent(3 * 3600, summer));
    TEST_ASSERT_EQUAL_INT8(2, standardUtcOffsetHoursFromCurrent(2 * 3600, winter));

    // Same round trip on the other side of UTC (US Eastern: -4h summer, -5h winter) and at UTC
    // itself (London: +1h summer, 0 winter).
    TEST_ASSERT_EQUAL_INT8(-5, standardUtcOffsetHoursFromCurrent(-4 * 3600, summer));
    TEST_ASSERT_EQUAL_INT8(-5, standardUtcOffsetHoursFromCurrent(-5 * 3600, winter));
    TEST_ASSERT_EQUAL_INT8(0, standardUtcOffsetHoursFromCurrent(3600, summer));
    TEST_ASSERT_EQUAL_INT8(0, standardUtcOffsetHoursFromCurrent(0, winter));
}

void test_standard_offset_from_geolocation_truncates_and_clamps() {
    using irrigator::domain::epochFromUtc;
    using irrigator::domain::standardUtcOffsetHoursFromCurrent;

    const int64_t winter = epochFromUtc(2024, 1, 15, 12, 0, 0);

    // Sub-hour zones truncate toward zero, symmetrically on both sides of UTC (India +05:30,
    // Nepal +05:45, Marquesas -09:30) — the stored field is whole hours.
    TEST_ASSERT_EQUAL_INT8(5, standardUtcOffsetHoursFromCurrent(5 * 3600 + 1800, winter));
    TEST_ASSERT_EQUAL_INT8(5, standardUtcOffsetHoursFromCurrent(5 * 3600 + 2700, winter));
    TEST_ASSERT_EQUAL_INT8(-9, standardUtcOffsetHoursFromCurrent(-(9 * 3600 + 1800), winter));

    // A nonsensical `offset` clamps into range rather than producing a config validateConfig()
    // would reject and a boot that falls back to defaults.
    TEST_ASSERT_EQUAL_INT8(13, standardUtcOffsetHoursFromCurrent(99 * 3600, winter));
    TEST_ASSERT_EQUAL_INT8(-13, standardUtcOffsetHoursFromCurrent(-99 * 3600, winter));
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
    RUN_TEST(test_adjust_duration_step_grows_with_magnitude);
    RUN_TEST(test_adjust_duration_pulls_out_of_range_current_into_range_first);
    RUN_TEST(test_adjust_duration_saturates_at_both_bounds);
    RUN_TEST(test_adjust_duration_inverted_range_is_a_no_op);
    RUN_TEST(test_watering_percentage_edge_cases);
    RUN_TEST(test_auto_adjustment_percentage_base_case_is_100);
    RUN_TEST(test_auto_adjustment_percentage_temp_above_base_increases_it);
    RUN_TEST(test_auto_adjustment_percentage_rain_reduces_it);
    RUN_TEST(test_auto_adjustment_percentage_clamps_to_upper_limit);
    RUN_TEST(test_auto_adjustment_percentage_below_lower_limit_sets_zero);
    RUN_TEST(test_auto_adjustment_percentage_exactly_on_lower_limit_is_kept_not_zeroed);
    RUN_TEST(test_auto_adjustment_percentage_exactly_on_upper_limit_is_not_clamped_down);
    RUN_TEST(test_auto_adjustment_percentage_zero_lower_limit_floors_at_zero);
    RUN_TEST(test_should_skip_for_rain_probability_inclusive_amount_exclusive);
    RUN_TEST(test_should_skip_for_rain_uses_configured_thresholds);
    RUN_TEST(test_open_meteo_now_index_is_days_back_times_24_plus_hour);
    RUN_TEST(test_open_meteo_now_index_at_midnight);
    RUN_TEST(test_open_meteo_now_index_with_no_past_days_is_just_the_hour);
    RUN_TEST(test_summarize_past_average_daily_max_temperature_averages_first_n_days);
    RUN_TEST(test_summarize_past_average_daily_max_temperature_rejects_short_array);
    RUN_TEST(test_summarize_past_rainfall_sums_first_n_days);
    RUN_TEST(test_summarize_past_rainfall_rejects_short_array);
    RUN_TEST(test_summarize_forecast_rain_window_computes_max_and_sum);
    RUN_TEST(test_summarize_forecast_rain_window_rejects_short_array);
    RUN_TEST(test_summarize_forecast_rain_window_accepts_window_ending_exactly_at_array_end);
    RUN_TEST(test_summarize_forecast_rain_window_rejects_when_only_the_amount_array_is_short);
    RUN_TEST(test_summarize_weather_api_forecast_rain_window_matches_from_now_epoch);
    RUN_TEST(test_summarize_weather_api_forecast_rain_window_fails_when_not_enough_hours);
    RUN_TEST(test_summarize_weather_api_forecast_rain_window_spans_day_boundary_and_stops_at_window);
    RUN_TEST(test_summarize_weather_api_forecast_rain_window_reports_zero_qualifying_hours);
    RUN_TEST(test_start_time_validation);
    RUN_TEST(test_start_time_validation_rejects_malformed_lengths_without_overrunning);
    RUN_TEST(test_schedule_record_validation);
    RUN_TEST(test_watering_queue_capacity_and_fifo);
    RUN_TEST(test_watering_queue_wraps_around_and_preserves_fifo_order);
    RUN_TEST(test_watering_queue_peek_and_size_accessors);
    RUN_TEST(test_watering_queue_clear_empties_and_resets_head);
    RUN_TEST(test_watering_request_kind_treats_zone_zero_as_pump_only);
    RUN_TEST(test_run_reason_to_string);
    RUN_TEST(test_rain_skip_self_expires);
    RUN_TEST(test_rain_skip_unset_never_skips_and_clear_disarms);
    // RUN_TEST(test_zone_run_starts_pump_before_watering_then_gracefully_stops);
    RUN_TEST(test_pump_only_does_not_enter_grace_period);
    RUN_TEST(test_abort_while_watering_still_gives_pump_its_graceful_shutdown);
    RUN_TEST(test_zone_watering_survives_large_uptime_start_time);
    // RUN_TEST(test_abort_during_pump_prestart_also_gives_pump_its_graceful_shutdown);
    RUN_TEST(test_abort_pump_only_stops_immediately);
    RUN_TEST(test_abort_during_existing_grace_period_cuts_it_short);
    RUN_TEST(test_start_rejects_invalid_requests_and_leaves_state_untouched);
    RUN_TEST(test_start_is_refused_while_a_run_is_already_watering);
    RUN_TEST(test_queued_run_hands_off_during_pump_grace_period_without_inheriting_its_deadline);
    RUN_TEST(test_snapshot_reports_pump_and_countdown_per_state);
    RUN_TEST(test_pump_only_snapshot_counts_down_against_zone_zero);
    RUN_TEST(test_config_store_round_trips_valid_json);
    RUN_TEST(test_config_store_rejects_missing_required_fields);
    RUN_TEST(test_config_store_allows_mqtt_fields_blank);
    RUN_TEST(test_validate_config_rules_at_their_boundaries);
    RUN_TEST(test_make_default_zones_is_the_valid_baseline_settings);
    RUN_TEST(test_validate_settings_rejects_each_out_of_range_field);
    RUN_TEST(test_settings_store_defaults_missing_zone_names);
    RUN_TEST(test_settings_store_rejects_out_of_range_percentage);
    RUN_TEST(test_settings_store_round_trips);
    RUN_TEST(test_settings_store_round_trips_zones_location_and_api_key);
    RUN_TEST(test_serialize_into_undersized_buffer_produces_output_that_does_not_parse_back);
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
    RUN_TEST(test_merge_settings_accepts_both_zone_shapes);
    RUN_TEST(test_merge_settings_rejects_oversized_zone_name_per_zone);
    RUN_TEST(test_merge_settings_rejects_wrongly_typed_zone_entry);
    RUN_TEST(test_merge_settings_updates_weatherapi_key_and_rejects_oversized_one);
    RUN_TEST(test_merge_tolerates_absent_or_undersized_rejected_fields_buffer);
    RUN_TEST(test_merge_settings_rejects_malformed_json_outright);
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
    RUN_TEST(test_schedule_engine_skips_inactive_and_non_matching_records);
    RUN_TEST(test_schedule_engine_returns_every_record_matching_the_same_minute);
    RUN_TEST(test_schedule_engine_never_writes_past_due_capacity);
    RUN_TEST(test_schedule_engine_fires_again_in_the_next_matching_minute);
    RUN_TEST(test_schedule_engine_resumes_once_the_rain_skip_deadline_passes);
    RUN_TEST(test_ui_state_machine_wakes_on_input_and_shows_normal);
    RUN_TEST(test_ui_state_machine_active_interaction_timeout_returns_to_display_off);
    RUN_TEST(test_ui_state_machine_zone_and_duration_picker_flow_emits_start_watering);
    RUN_TEST(test_ui_state_machine_zone_picker_skips_disabled_zones);
    RUN_TEST(test_ui_state_machine_zone_picker_falls_back_to_pump_when_all_zones_disabled);
    RUN_TEST(test_ui_state_machine_long_click_cancels_duration_picker);
    RUN_TEST(test_ui_state_machine_watering_active_long_click_opens_menu);
    RUN_TEST(test_ui_state_machine_watering_active_rotation_queues_next_watering);
    RUN_TEST(test_ui_state_machine_watering_active_gestures_start_from_running_zone);
    RUN_TEST(test_ui_state_machine_watering_active_running_zone_beats_next_scheduled);
    RUN_TEST(test_ui_state_machine_pump_only_run_click_keeps_pump);
    RUN_TEST(test_ui_state_machine_queue_add_result_shows_outcome_and_ok_returns_to_normal);
    RUN_TEST(test_ui_state_machine_stop_confirm_flow);
    RUN_TEST(test_ui_state_machine_stop_all_requires_confirmation_then_stops_and_clears);
    RUN_TEST(test_ui_state_machine_stop_all_confirm_no_cancels_without_intent);
    RUN_TEST(test_ui_state_machine_menu_main_queue_item_opens_menu_queue);
    RUN_TEST(test_ui_state_machine_menu_queue_empty_shows_only_back);
    RUN_TEST(test_ui_state_machine_menu_queue_with_items_lists_them_then_back);
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
    RUN_TEST(test_ui_state_machine_menu_main_layout_shifts_by_three_while_watering);
    RUN_TEST(test_ui_state_machine_first_input_from_display_off_only_wakes);
    RUN_TEST(test_ui_state_machine_every_input_rearms_the_interaction_timeout);
    RUN_TEST(test_ui_state_machine_timeout_abandons_an_in_progress_edit);
    RUN_TEST(test_ui_state_machine_normal_rotation_starts_from_the_displayed_scheduled_zone);
    RUN_TEST(test_ui_state_machine_schedule_active_field_is_a_direct_toggle);
    RUN_TEST(test_ui_state_machine_menu_schedule_with_no_records_only_navigates_back);
    RUN_TEST(test_ui_state_machine_settings_back_row_is_one_tick_backwards_and_exits);
    RUN_TEST(test_ui_state_machine_factory_reset_confirm_overrides_any_screen_and_edit);
    RUN_TEST(test_ui_state_machine_factory_reset_confirm_no_cancels_and_yes_emits_intent);
    RUN_TEST(test_ui_state_machine_factory_reset_result_is_terminal);
    RUN_TEST(test_ui_state_machine_result_screens_bound_caller_supplied_strings);
    RUN_TEST(test_calendar_math_round_trips_and_weekday);
    RUN_TEST(test_calendar_math_epoch_day_zero_is_thursday_1970_01_01);
    RUN_TEST(test_calendar_math_handles_gregorian_leap_rules);
    RUN_TEST(test_calendar_math_weekday_for_pre_epoch_days);
    RUN_TEST(test_format_iso8601_utc_round_trips_and_fits_20_bytes);
    RUN_TEST(test_format_iso8601_utc_handles_pre_epoch_seconds);
    RUN_TEST(test_wifi_quality_thresholds_and_bar_count);
    RUN_TEST(test_dst_rules_matches_known_2024_eu_transitions);
    RUN_TEST(test_standard_offset_from_geolocation_is_season_independent);
    RUN_TEST(test_standard_offset_from_geolocation_truncates_and_clamps);
    return UNITY_END();
}