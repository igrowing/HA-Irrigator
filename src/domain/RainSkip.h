#pragma once

#include <stdint.h>

namespace irrigator::domain {

class RainSkip {
public:
    void setSkipUntil(int64_t epochSeconds) { skipUntilEpochSeconds_ = epochSeconds; }
    void clear() { skipUntilEpochSeconds_ = 0; }

    bool shouldSkipScheduledRun(int64_t nowEpochSeconds) const {
        return skipUntilEpochSeconds_ > 0 && nowEpochSeconds < skipUntilEpochSeconds_;
    }

    int64_t skipUntil() const { return skipUntilEpochSeconds_; }

private:
    int64_t skipUntilEpochSeconds_ = 0;
};

}  // namespace irrigator::domain