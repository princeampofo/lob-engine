#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// The interface every book implementation provides, so the same tests,
// replay and benchmarks run against all of them.
//
// Matching follows price-time priority: an incoming order trades against the
// best opposite price first, and within a price level the oldest order first.
//
// Trades are appended to a caller-owned vector so the caller can reuse it and
// avoid an allocation per call.
class OrderBook {
public:
    virtual ~OrderBook() = default;

    // Matches as much as possible against the opposite side, then rests any
    // remainder at `price`. Requires qty > 0 and an id not currently resting.
    virtual void add_limit(const Order& order, std::vector<Trade>& trades) = 0;

    // Matches up to `qty` against the opposite side at any price. Whatever
    // cannot be filled is dropped; market orders never rest.
    virtual void add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) = 0;

    // Removes a resting order. Returns false if the id is not in the book.
    virtual bool cancel(OrderId id) = 0;

    // Reduces a resting order by `qty` while keeping its queue position.
    // Reducing by its full remaining quantity or more removes it.
    // Returns false if the id is not in the book.
    virtual bool reduce(OrderId id, Qty qty) = 0;

    // Removes every order resting at `price` on `side` (a mass cancel).
    virtual void clear_level(Side side, Price price) = 0;

    // Whether an order with this id is resting in the book.
    virtual bool contains(OrderId id) const = 0;

    virtual std::optional<Price> best_bid() const = 0;
    virtual std::optional<Price> best_ask() const = 0;

    // Up to `levels` price levels per side, best first.
    virtual Depth depth(std::size_t levels) const = 0;
};

}  // namespace lob
