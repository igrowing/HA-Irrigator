#include "Tuf2000mProtocol.h"

#include <string.h>

namespace irrigator::domain {

float decodeSwappedFloat(uint16_t reg0, uint16_t reg1) {
    // Same word-swap github.com/tczerwonka/tuf2000-modbus-reader uses: put the SECOND register
    // first in memory, then reinterpret the 4 bytes as an IEEE-754 float.
    uint16_t swapped[2] = {reg1, reg0};
    float value = 0.0f;
    memcpy(&value, swapped, sizeof(value));
    return value;
}

uint32_t Tuf2000mPollTracker::beginPoll(uint32_t nowMs) {
    ++activePollSeq_;
    pendingFieldCount_ = kFieldCount;
    failed_ = false;
    inFlight_ = true;
    startedAtMs_ = nowMs;
    return activePollSeq_;
}

bool Tuf2000mPollTracker::recordField(uint32_t token, bool ok) {
    if (!isCurrent(token)) {
        return false;  // Stale reply from an already-abandoned poll - see the class comment.
    }
    if (!ok) {
        failed_ = true;
    }
    if (pendingFieldCount_ > 0) {
        --pendingFieldCount_;
    }
    if (pendingFieldCount_ == 0) {
        inFlight_ = false;
        return true;
    }
    return false;
}

}  // namespace irrigator::domain
