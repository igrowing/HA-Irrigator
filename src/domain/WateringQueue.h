#pragma once

#include <stdint.h>

#include "IrrigationTypes.h"

namespace irrigator::domain {

template <uint8_t Capacity>
class WateringQueue {
public:
    bool push(const WateringRequest& request) {
        if (count_ >= Capacity) {
            return false;
        }
        items_[(head_ + count_) % Capacity] = request;
        ++count_;
        return true;
    }

    bool pop(WateringRequest& out) {
        if (count_ == 0) {
            return false;
        }
        out = items_[head_];
        head_ = static_cast<uint8_t>((head_ + 1U) % Capacity);
        --count_;
        return true;
    }

    void clear() {
        head_ = 0;
        count_ = 0;
    }

    uint8_t size() const { return count_; }
    constexpr uint8_t capacity() const { return Capacity; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ >= Capacity; }

private:
    WateringRequest items_[Capacity] = {};
    uint8_t head_ = 0;
    uint8_t count_ = 0;
};

}  // namespace irrigator::domain