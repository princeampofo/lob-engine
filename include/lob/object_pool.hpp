#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// A preallocated pool of objects addressed by 32-bit index. Allocating and
// releasing only push and pop a free list, so neither touches the heap once
// the pool is big enough. If it runs out, it doubles (the only allocation).
//
// Indexes rather than pointers keep references valid when the pool grows.
template <class T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) { grow(capacity == 0 ? 1 : capacity); }

    std::uint32_t allocate() {
        if (free_.empty()) grow(items_.size());
        const std::uint32_t index = free_.back();
        free_.pop_back();
        return index;
    }

    // Never allocates: the free list always has room for every item.
    void release(std::uint32_t index) { free_.push_back(index); }

    T& operator[](std::uint32_t index) { return items_[index]; }
    const T& operator[](std::uint32_t index) const { return items_[index]; }

private:
    void grow(std::size_t extra) {
        const std::size_t old_size = items_.size();
        items_.resize(old_size + extra);
        free_.reserve(items_.size());
        // Push in reverse so the lowest indexes are handed out first.
        for (std::size_t i = items_.size(); i > old_size; --i) {
            free_.push_back(static_cast<std::uint32_t>(i - 1));
        }
    }

    std::vector<T> items_;
    std::vector<std::uint32_t> free_;
};

}  // namespace lob
