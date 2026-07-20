#pragma once

#include <stdint.h>

namespace irrigator::domain {

enum class WifiQuality : uint8_t { Miserable = 0, Poor = 1, Fair = 2, Good = 3, Excellent = 4 };

inline WifiQuality wifiQualityFromRssi(int32_t rssiDbm) {
    if (rssiDbm >= -55) {
        return WifiQuality::Excellent;
    } else if (rssiDbm >= -65) {
        return WifiQuality::Good;
    } else if (rssiDbm >= -75) {
        return WifiQuality::Fair;
    } else if (rssiDbm >= -85) {
        return WifiQuality::Poor;
    }
    return WifiQuality::Miserable;
}

inline const char* toString(WifiQuality quality) {
    switch (quality) {
        case WifiQuality::Excellent:
            return "excellent";
        case WifiQuality::Good:
            return "good";
        case WifiQuality::Fair:
            return "fair";
        case WifiQuality::Poor:
            return "poor";
        case WifiQuality::Miserable:
        default:
            return "miserable";
    }
    return "miserable";
}

// 0 (Miserable) .. 4 (Excellent) — the enum's ordinal doubles as a bar/dash count so callers
// don't need a second lookup table alongside toString().
inline uint8_t wifiQualityBarCount(WifiQuality quality) {
    return static_cast<uint8_t>(quality);
}

}  // namespace irrigator::domain
