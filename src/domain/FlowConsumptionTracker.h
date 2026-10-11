#pragma once

#include <stdint.h>

#include "board_config.h"

namespace irrigator::domain {

// Pure (no MQTT/hardware dependency) water-consumption bookkeeping derived from the flow meter's
// cumulative totalizer readings, covering the two things main.ino's updateFlowSampling()/
// startIrrigation()/reportStop()/reportValveClosed() need. Both are RAM-only, like the rest of
// this project's transient runtime state - a restart naturally resets them, same tolerance as
// everything else here.

// A never-reset-while-running liters counter per zone plus one for "other", fed by the totalizer
// delta between consecutive polls - see the HA "Water Consumption" dashboard group
// (homeassistant/dashboards/garden_tab.yaml), which reads a rolling 24h window off these via HA's
// own long-term statistics (state_class: total_increasing tolerates this counter resetting to 0 on
// a device reboot without miscounting that reset as negative consumption).
class FlowLifetimeCounters {
public:
    // Adds deltaLiters (the totalizer's increase since the previous poll - callers should already
    // have discarded a non-positive delta) to whichever bucket the CURRENT irrigation state
    // indicates:
    //   - activeZone (1..board::ZONE_COUNT) if a zone is watering - that zone's own counter.
    //   - "other" if activeZone == 0 AND !pumpOn - every valve is closed, so any flow at all is
    //     leakage or a hose spliced into the line, not misattributed to whichever zone last ran
    //     (per the user's definition: "other" is not a second whole-house meter).
    //   - discarded entirely (added to neither bucket) if pumpOn && activeZone == 0 - a pump-only
    //     run, or the post-valve-close pump-graceful-shutdown tail, where the valve is already shut
    //     but the pump is still spinning down. This project has no third bucket for that case.
    void addDelta(uint8_t activeZone, bool pumpOn, float deltaLiters);

    // zone: 1..board::ZONE_COUNT. Returns 0 for an out-of-range zone.
    float zoneLiters(uint8_t zone) const;
    float otherLiters() const { return otherLiters_; }

private:
    float zoneLiters_[board::ZONE_COUNT] = {};
    float otherLiters_ = 0.0f;
};

// Captures the totalizer reading a run started at, then computes and clears it exactly once - the
// run's completed volume, in liters - the moment that run's watering actually ends (the valve
// closing, for a zone+pump run; the whole run stopping, for a pump-only one - see main.ino's call
// sites for exactly which). A later stop event for the SAME logical run (e.g. the final
// reportStop() once a zone+pump run's pump graceful-shutdown tail also finishes, after
// reportValveClosed() already reported the real number) reads back 0 instead of repeating or
// drifting - this is what main.ino's `/state` "volume" field carries.
class FlowRunVolumeTracker {
public:
    // haveTotalizer=false (no flow reading has ever landed yet, or flow sensing is otherwise
    // unavailable) means takeVolumeLitersOnStop() will report 0 for this run rather than a bogus
    // delta against an uninitialized baseline.
    void beginRun(float totalizerM3, bool haveTotalizer);

    float takeVolumeLitersOnStop(float totalizerM3, bool haveTotalizer);

private:
    float startTotalizerM3_ = 0.0f;
    bool captured_ = false;
};

}  // namespace irrigator::domain
