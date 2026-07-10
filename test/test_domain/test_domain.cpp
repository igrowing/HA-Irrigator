#include <unity.h>

#include "../../src/domain/CurrentFaultMonitor.h"
#include "../../src/domain/DurationAdjuster.h"
#include "../../src/domain/IrrigationStateMachine.h"
#include "../../src/domain/RainSkip.h"
#include "../../src/domain/Schedule.h"
#include "../../src/domain/WateringQueue.h"
#include "board_config.h"

using irrigator::domain::RotationDirection;

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
    return UNITY_END();
}