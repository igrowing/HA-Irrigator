#pragma once

#include <stdint.h>

namespace irrigator::domain {

// EU rule: clocks go forward the last Sunday of March at 01:00 UTC, and back the last Sunday of
// October at 01:00 UTC. Deliberately UTC-based (not "02:00 local") since that's how the rule is
// actually defined — it's what keeps every EU country's transition simultaneous.
bool isEuDstActive(int64_t utcEpochSeconds);

// Converts a geolocation provider's *current* UTC offset (ip-api.com's `offset` field: seconds,
// DST already folded in at the moment of the lookup) into the plain standard-time hour offset
// config.json's `timezone` stores and drivers::Clock expects — i.e. it takes the DST hour back
// out, since Clock re-applies it dynamically via isEuDstActive(). Doing the subtraction against
// the same EU rule Clock uses means the round trip is exact whenever the two agree, which is the
// whole point: a lookup done in July and one done in January land on the same stored offset.
//
// Two known limits, both inherited from that EU-only rule rather than introduced here: a location
// on a different DST rule (or none) queried during the EU's summer window comes out an hour early
// for its winter half, and sub-hour zones (+05:30, +05:45) truncate toward zero because the
// stored field is whole hours. Either can still be corrected by uploading a config.json with an
// explicit `timezone` — mergeConfig deliberately keeps honouring that field.
//
// Clamped to board::TIMEZONE_OFFSET_MIN/MAX_HOURS so a garbage `offset` can never produce a
// config that validateConfig() would reject.
int8_t standardUtcOffsetHoursFromCurrent(int32_t currentOffsetSeconds, int64_t utcEpochSeconds);

}  // namespace irrigator::domain
