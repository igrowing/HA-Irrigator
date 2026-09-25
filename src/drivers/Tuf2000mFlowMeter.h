#pragma once

#include <Arduino.h>
// Note for whatever includes this header: eModbus's RTUutils.h (pulled in transitively below) does
// a file-scope `using namespace Modbus;`, which is therefore also visible wherever this header is
// included (e.g. main.ino) - a known quirk of the library itself, not something this driver adds
// deliberately. Its names (Error, SUCCESS, TIMEOUT, READ_HOLD_REGISTER, ...) are Modbus-protocol
// terms unlikely to collide with anything else in this codebase.
#include <ModbusClientRTU.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <memory>

#include "../domain/Tuf2000mProtocol.h"

namespace irrigator::drivers {

// Modbus RTU driver for the TUF-2000M ultrasonic clamp-on flow meter. Named after the specific
// meter model (not a generic FlowMeterSensor) because the register map/decode logic is
// TUF-2000-specific - see hardware/readme.md's "Water flow sensing" section for the wiring this
// talks to, and board_config.h's FLOW_METER_* constants for the register map itself.
//
// This is a thin eModbus-facing wrapper: the actual poll-sequencing/staleness state machine and
// the register decode live in irrigator::domain::Tuf2000mPollTracker/decodeSwappedFloat
// (src/domain/Tuf2000mProtocol.*) - pulled out specifically so that logic is unit-testable under
// `pio test -e native` without any Modbus/serial hardware or mocking. This class's own job is just
// gluing that pure logic to eModbus's actual request/callback API.
//
// Deliberately decoupled from the rest of this project, the same way CurrentSensor is: no
// Settings.h/MqttService/IrrigationStateMachine includes here - pins/baud/slave-ID/register map
// all come in via Config, readings go out as a plain struct through responseReady()/takeReading().
// Keeping this shape is what would let it become its own PlatformIO/Arduino library later without a
// rewrite (a library.json/library.properties, an example sketch, a README, a license - not new
// code), and making the register map configurable rather than hardcoded is what would let it serve
// other TUF-2000 variants too, not just this board.
//
// eModbus's ModbusClientRTU is async-only by design: it runs its own FreeRTOS worker task and
// queues/serializes requests internally, delivering results via onDataHandler/onErrorHandler
// callbacks rather than a blocking call - unlike CurrentSensor's blocking-task-plus-queue pattern,
// no new FreeRTOS task needs to be created for this feature. This driver exposes that async engine
// (readAsync/responseReady/takeReading) plus a thin blocking convenience (read()) built on top of
// it, rather than two separate implementations.
class Tuf2000mFlowMeter {
public:
    struct Config {
        uint8_t rxPin = 0;
        uint8_t txPin = 0;
        uint32_t baudRate = 9600;
        uint8_t slaveId = 1;
        // Register map (function code 03) - see board_config.h's FLOW_METER_REG_* constants.
        // flowRateRegister/totalizerRegister each span 2 words (REAL4/IEEE-754 float,
        // word-swapped - see domain::decodeSwappedFloat()), as does soundSpeedRegister.
        // signalQualityRegister starts a 3-word block: quality, then upstream and downstream signal
        // strength in the two registers right after it.
        uint16_t flowRateRegister = 0;
        uint16_t totalizerRegister = 0;
        uint16_t signalQualityRegister = 0;
        uint16_t soundSpeedRegister = 0;
        uint32_t pollIntervalMs = 2000;
        // Multiplied into flowRateM3h and totalizerM3 as they are decoded (1.0 = meter's own
        // values); corrects a meter whose calibration is off.
        float calibrationMultiplier = 1.0f;
    };

    struct Reading {
        float flowRateM3h = 0.0f;
        float totalizerM3 = 0.0f;
        // Raw quality word: domain::signalQualityValue()/signalAutogainStep() split it.
        uint16_t signalQuality = 0;
        uint16_t signalStrengthUp = 0;
        uint16_t signalStrengthDown = 0;
        float soundSpeedMs = 0.0f;
        uint32_t timestampMs = 0;
        // false on a Modbus error/timeout on any of the four requests polled this cycle - pushed
        // through the same responseReady()/takeReading() path as a real reading, so callers can
        // tell "polled and it failed" apart from "nothing new yet" rather than errors just
        // vanishing.
        bool valid = false;
    };

    // Configures the UART on cfg's RX/TX pins at cfg.baudRate and constructs the ModbusClientRTU
    // worker bound to it. Call once from setup(); safe to call at most once per instance.
    void begin(HardwareSerial& serial, const Config& cfg);

    // Non-blocking; throttles to cfg.pollIntervalMs and no-ops while a poll is already in flight -
    // requests are never overlapped with a NEW poll's, which directly targets the reference
    // project's (github.com/tczerwonka/tuf2000-modbus-reader) "the modbus stuff just didn't give
    // back consistent data for two sequential queries" failure. Call every loop() tick.
    void readAsync(uint32_t nowMs);

    // True once a fresh Reading (success or failure) has landed since the last takeReading().
    bool responseReady() const;

    // Copies the latest Reading out and clears responseReady(). Returns false if nothing was ready.
    bool takeReading(Reading& out);

    // Blocking convenience: calls readAsync() then spins on takeReading() until it succeeds or
    // timeoutMs elapses. For one-off "read now" uses (a bench test sketch, a future MQTT command) -
    // never call this from loop() or anything loop() calls.
    bool read(uint32_t timeoutMs, Reading& out);

    // Most recent Modbus error seen on any request (Modbus::Error value, 0 = SUCCESS = none since
    // boot). Sticky: a later successful poll does NOT clear it, so an intermittent fault stays
    // diagnosable after the fact. Written on eModbus's worker task, read from loop() - a single
    // byte, so the volatile is enough.
    uint8_t lastErrorCode() const { return lastErrorCode_; }
    // Human-readable text for lastErrorCode() (a static string, never null).
    const char* lastErrorText() const;

private:
    void recordError(Modbus::Error error);
    void issueRequest(uint32_t pollSeq,
                       irrigator::domain::Tuf2000mPollTracker::Field field,
                       uint16_t startRegister,
                       uint16_t wordCount);
    void handleData(ModbusMessage msg, uint32_t token);
    void handleError(Modbus::Error error, uint32_t token);
    // Common "one field of the current poll is done" bookkeeping shared by handleData()/
    // handleError(): delegates to pollTracker_ and, once every field has reported in, publishes
    // scratch_ to resultQueue_. Runs on eModbus's worker task.
    void finishField(uint32_t token, bool ok);

    std::unique_ptr<ModbusClientRTU> client_;
    Config config_;

    // Poll-sequencing/staleness state machine - see irrigator::domain::Tuf2000mPollTracker's
    // class comment. readAsync() (whatever task calls it) fully starts a new poll (beginPoll())
    // BEFORE issuing any request for it, then never touches pollTracker_/scratch_ again until the
    // next poll starts - so there is exactly one handoff point per poll, not ongoing concurrent
    // access (pollTracker_.inFlight() itself is the one field genuinely read cross-task; see its
    // own volatile comment).
    irrigator::domain::Tuf2000mPollTracker pollTracker_;
    Reading scratch_;

    volatile uint8_t lastErrorCode_ = 0;

    uint32_t lastPollMs_ = 0;
    bool everPolled_ = false;

    // Depth-1 "latest value" mailbox from eModbus's worker task (where the callbacks above run) to
    // whatever task calls responseReady()/takeReading() - same idiom main.ino's
    // currentBurstResultQueue already uses for CurrentSensor's burst results: xQueueOverwrite/
    // xQueueReceive give atomic, tear-free handoff of the whole Reading struct with no hand-written
    // locking.
    QueueHandle_t resultQueue_ = nullptr;
};

}  // namespace irrigator::drivers
