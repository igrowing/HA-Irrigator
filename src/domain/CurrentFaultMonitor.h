#pragma once

#include <stdint.h>

namespace irrigator::domain {

struct CurrentThresholds {
    uint16_t minMa = 0;
    uint16_t maxMa = 0;
};

class CurrentFaultMonitor {
public:
    explicit CurrentFaultMonitor(CurrentThresholds thresholds) : thresholds_(thresholds) {}

    void beginActiveCycle() { alertSentThisCycle_ = false; }
    void endActiveCycle() { alertSentThisCycle_ = false; }

    bool checkAndConsumeAlert(uint16_t currentMa) {
        if (alertSentThisCycle_) {
            return false;
        }
        if (currentMa < thresholds_.minMa || currentMa > thresholds_.maxMa) {
            alertSentThisCycle_ = true;
            return true;
        }
        return false;
    }

private:
    CurrentThresholds thresholds_;
    bool alertSentThisCycle_ = false;
};

}  // namespace irrigator::domain