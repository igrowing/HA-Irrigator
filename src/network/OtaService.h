#pragma once

#include <functional>

namespace irrigator::network {

using OtaResultHandler = std::function<void(bool success, const char* message)>;

class OtaService {
public:
    void setResultHandler(OtaResultHandler handler) { resultHandler_ = handler; }
    bool inProgress() const { return inProgress_; }

    // Synchronous by design (mirrors the ESP32 Arduino OTA-from-URL convention): downloads and
    // flashes the whole image before returning, then restarts on success. This intentionally
    // monopolizes the CPU for the duration — see agents.md's note to treat OTA as higher-risk
    // than other changes; a chunked non-blocking flash write buys little here and adds a lot of
    // failure-mode surface for a path where "just don't get it wrong" matters more than
    // responsiveness.
    void beginUpdateFromUrl(const char* url);

private:
    void reportResult(bool success, const char* message);

    bool inProgress_ = false;
    OtaResultHandler resultHandler_;
};

}  // namespace irrigator::network
