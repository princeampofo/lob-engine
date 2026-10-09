#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Hash map from order id to a 32-bit index, using open addressing with
// linear probing: every entry lives in one flat array, so a lookup reads
// contiguous memory and inserting never allocates (until the table grows,
// which reserving up front avoids).
class OrderIdMap {
public:
    static constexpr std::uint32_t kMissing = UINT32_MAX;

    explicit OrderIdMap(std::size_t expected_size) {
        std::size_t capacity = 16;
        while (capacity < 2 * expected_size) capacity *= 2;  // keep load at or below 1/2
        resize(capacity);
    }

    std::uint32_t find(OrderId id) const {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& slot = slots_[i];
            if (slot.value == kMissing) return kMissing;
            if (slot.key == id) return slot.value;
        }
    }

    // `id` must not already be in the map.
    void insert(OrderId id, std::uint32_t value) {
        if (2 * (size_ + 1) > slots_.size()) rehash(2 * slots_.size());
        std::size_t i = home(id);
        while (slots_[i].value != kMissing) i = (i + 1) & mask_;
        slots_[i] = {id, value};
        ++size_;
    }

    void erase(OrderId id) {
        std::size_t hole = home(id);
        while (true) {
            if (slots_[hole].value == kMissing) return;  // not present
            if (slots_[hole].key == id) break;
            hole = (hole + 1) & mask_;
        }
        --size_;

        // Backward-shift deletion: pull later entries of the probe run into
        // the hole when the hole lies between their home slot and where they
        // sit, so lookups never stop early at a gap. No tombstones needed.
        for (std::size_t i = (hole + 1) & mask_; slots_[i].value != kMissing; i = (i + 1) & mask_) {
            const std::size_t h = home(slots_[i].key);
            const bool home_between_hole_and_i = hole <= i ? (h > hole && h <= i) : (h > hole || h <= i);
            if (!home_between_hole_and_i) {
                slots_[hole] = slots_[i];
                hole = i;
            }
        }
        slots_[hole].value = kMissing;
    }

    std::size_t size() const { return size_; }

private:
    struct Slot {
        OrderId key = 0;
        std::uint32_t value = kMissing;
    };

    // Fibonacci hashing: spreads sequential ids across the table.
    std::size_t home(OrderId id) const {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void resize(std::size_t capacity) {
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c /= 2) --shift_;
    }

    void rehash(std::size_t capacity) {
        std::vector<Slot> old = std::move(slots_);
        resize(capacity);
        size_ = 0;
        for (const Slot& slot : old) {
            if (slot.value != kMissing) insert(slot.key, slot.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace lob
