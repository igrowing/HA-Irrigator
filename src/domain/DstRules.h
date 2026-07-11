#pragma once

#include <stdint.h>

namespace irrigator::domain {

// EU rule: clocks go forward the last Sunday of March at 01:00 UTC, and back the last Sunday of
// October at 01:00 UTC. Deliberately UTC-based (not "02:00 local") since that's how the rule is
// actually defined — it's what keeps every EU country's transition simultaneous.
bool isEuDstActive(int64_t utcEpochSeconds);

}  // namespace irrigator::domain
