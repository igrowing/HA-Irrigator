#pragma once

namespace irrigator::network {

class OtaService {
public:
    bool inProgress() const { return inProgress_; }
    void beginUpdateFromUrl(const char* url);

private:
    bool inProgress_ = false;
};

}  // namespace irrigator::network