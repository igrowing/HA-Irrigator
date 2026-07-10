#include "OtaService.h"

#include "../drivers/Logger.h"

namespace irrigator::network {

void OtaService::beginUpdateFromUrl(const char* url) {
    inProgress_ = true;
    ::irrigator::drivers::Log.printf("[INFO] OTA update requested: %s\n",
                                     url == nullptr ? "" : url);
    // TODO: migrate legacy HTTPClient + Update implementation and publish result.
}

}  // namespace irrigator::network