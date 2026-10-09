#pragma once

#include <cstdint>
#include <vector>

namespace lob {

// Prices are integer ticks (LOBSTER uses dollars x 10,000). Never floats.
using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

inline Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;  // remaining quantity
};

// One fill between an incoming (aggressive) order and a resting order.
// The trade happens at the resting order's price.
struct Trade {
    OrderId incoming_id = 0;
    OrderId resting_id = 0;
    Price price = 0;
    Qty qty = 0;

    bool operator==(const Trade&) const = default;
};

// Total visible quantity at one price.
struct Level {
    Price price = 0;
    Qty qty = 0;

    bool operator==(const Level&) const = default;
};

// Top-N levels on each side, best price first.
struct Depth {
    std::vector<Level> bids;  // highest price first
    std::vector<Level> asks;  // lowest price first

    bool operator==(const Depth&) const = default;
};

}  // namespace lob
