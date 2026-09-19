#pragma once

#include <stdint.h>

namespace irrigator::domain {

// Pure (no Arduino/FreeRTOS/eModbus dependency) TUF-2000M protocol logic, extracted out of
// src/drivers/Tuf2000mFlowMeter so it can be unit tested under `pio test -e native` without any
// Modbus/serial hardware or mocking - the driver itself is just a thin eModbus-facing wrapper
// around this.

// Swaps the two 16-bit register words and reinterprets the 4 bytes as an IEEE-754 float - exactly
// how github.com/tczerwonka/tuf2000-modbus-reader decodes this meter's REAL4 registers (confirmed
// against its source; the byte order only works out on a little-endian host, true of both its
// ESP8266 target and this project's ESP32).
float decodeSwappedFloat(uint16_t reg0, uint16_t reg1);

// Tracks one poll cycle's three outstanding Modbus sub-requests (flow rate / totalizer / signal
// quality - see board_config.h's FLOW_METER_REG_* constants), issued together and never
// overlapped with the NEXT poll's. Encodes/decodes the eModbus request "token" (poll sequence
// number + which field) so a reply arriving after this poll has already been abandoned (stuck-
// request watchdog) is recognized as stale and ignored, instead of corrupting whatever poll comes
// next.
class Tuf2000mPollTracker {
public:
    enum Field : uint32_t { kFlowRate = 0, kTotalizer = 1, kSignalQuality = 2 };
    static constexpr uint8_t kFieldCount = 3;
    static constexpr uint32_t kFieldMask = 0x3;

    // Starts a new poll: bumps the poll sequence number and resets pending/failed state. Returns
    // the sequence number the caller should pass to makeToken() for each of this poll's requests.
    uint32_t beginPoll(uint32_t nowMs);

    static uint32_t makeToken(uint32_t pollSeq, Field field) {
        return (pollSeq << 2) | static_cast<uint32_t>(field);
    }
    static Field fieldOf(uint32_t token) { return static_cast<Field>(token & kFieldMask); }
    static uint32_t pollSeqOf(uint32_t token) { return token >> 2; }

    // True if token was issued under the currently active poll - false for a stale reply from a
    // poll already abandoned by forceClear() (or superseded by a later beginPoll()).
    bool isCurrent(uint32_t token) const { return pollSeqOf(token) == activePollSeq_; }

    // Records one field's outcome for the active poll. Stale tokens (see isCurrent()) are ignored
    // outright - they don't count against pendingFieldCount_ and can't flip failed(). Returns true
    // exactly once per poll: the moment every field has reported in (success or failure), at which
    // point inFlight() also goes false.
    bool recordField(uint32_t token, bool ok);

    bool failed() const { return failed_; }
    bool inFlight() const { return inFlight_; }
    uint32_t startedAtMs() const { return startedAtMs_; }
    uint32_t activePollSeq() const { return activePollSeq_; }

    // True once the active poll has been in flight longer than timeoutMs - the stuck-request
    // watchdog condition (a bug, or a wedged transceiver, left a reply from eModbus never arriving
    // at all). Always false when nothing is in flight.
    bool isStuck(uint32_t nowMs, uint32_t timeoutMs) const {
        return inFlight_ && (nowMs - startedAtMs_) >= timeoutMs;
    }

    // Force-ends the active poll without every field having reported (the watchdog's response to
    // isStuck()) - inFlight() goes false so the caller can retry, but activePollSeq_ is left alone
    // (only beginPoll() bumps it) so any of this poll's replies that do eventually trickle in are
    // still correctly recognized as stale by isCurrent() until the NEXT beginPoll() moves on.
    void forceClear() { inFlight_ = false; }

private:
    uint32_t activePollSeq_ = 0;
    uint8_t pendingFieldCount_ = 0;
    bool failed_ = false;
    // volatile: in the real driver, recordField()/forceClear() run on a different task (eModbus's
    // worker task / the caller of readAsync() respectively) than inFlight()/isStuck() are read
    // from - same cross-task tolerance as CurrentSensor's currentSamplingWindowActive elsewhere in
    // this codebase. Doesn't change this class's value semantics for the (single-threaded) unit
    // tests, only prevents the compiler from caching/reordering the read.
    volatile bool inFlight_ = false;
    uint32_t startedAtMs_ = 0;
};

}  // namespace irrigator::domain
