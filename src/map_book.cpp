#include "lob/map_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace lob {

namespace {

constexpr Price kMaxPrice = std::numeric_limits<Price>::max();
constexpr Price kMinPrice = std::numeric_limits<Price>::min();

// Does a resting price trade against an incoming order with this limit?
bool crosses(Side incoming_side, Price resting_price, Price limit) {
    return incoming_side == Side::Buy ? resting_price <= limit : resting_price >= limit;
}

template <class Levels>
std::vector<Level> top_levels(const Levels& levels, std::size_t n) {
    std::vector<Level> out;
    for (auto it = levels.begin(); it != levels.end() && out.size() < n; ++it) {
        out.push_back({it->first, it->second.total});
    }
    return out;
}

}  // namespace

// Fill `qty` against the best levels of `levels` (the side opposite to the
// incoming order) until it is filled or the next level no longer crosses.
template <class Levels>
void MapBook::match(Levels& levels, OrderId incoming_id, Side incoming_side, Price limit, Qty& qty,
                    std::vector<Trade>& trades) {
    while (qty > 0 && !levels.empty()) {
        auto level_it = levels.begin();
        const Price price = level_it->first;
        if (!crosses(incoming_side, price, limit)) break;

        PriceLevel& level = level_it->second;
        while (qty > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();
            const Qty fill = std::min(qty, resting.qty);
            trades.push_back({incoming_id, resting.id, price, fill});

            qty -= fill;
            resting.qty -= fill;
            level.total -= fill;
            if (resting.qty == 0) {
                orders_.erase(resting.id);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) levels.erase(level_it);
    }
}

template <class Levels>
void MapBook::rest(Levels& levels, const Order& order) {
    PriceLevel& level = levels[order.price];
    level.orders.push_back(order);
    level.total += order.qty;
    orders_[order.id] = std::prev(level.orders.end());
}

template <class Levels>
void MapBook::erase(Levels& levels, std::list<Order>::iterator order) {
    auto level_it = levels.find(order->price);
    level_it->second.total -= order->qty;
    level_it->second.orders.erase(order);
    if (level_it->second.orders.empty()) levels.erase(level_it);
}

template <class Levels>
void MapBook::reduce_in_place(Levels& levels, std::list<Order>::iterator order, Qty qty) {
    order->qty -= qty;
    levels.find(order->price)->second.total -= qty;
}

void MapBook::add_limit(const Order& order, std::vector<Trade>& trades) {
    assert(order.qty > 0);
    assert(!orders_.contains(order.id));

    Order remaining = order;
    if (order.side == Side::Buy) {
        match(asks_, order.id, order.side, order.price, remaining.qty, trades);
        if (remaining.qty > 0) rest(bids_, remaining);
    } else {
        match(bids_, order.id, order.side, order.price, remaining.qty, trades);
        if (remaining.qty > 0) rest(asks_, remaining);
    }
}

void MapBook::add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) {
    assert(qty > 0);
    if (side == Side::Buy) {
        match(asks_, id, side, kMaxPrice, qty, trades);
    } else {
        match(bids_, id, side, kMinPrice, qty, trades);
    }
}

bool MapBook::cancel(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;

    auto order = it->second;
    orders_.erase(it);
    if (order->side == Side::Buy) {
        erase(bids_, order);
    } else {
        erase(asks_, order);
    }
    return true;
}

bool MapBook::reduce(OrderId id, Qty qty) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;

    auto order = it->second;
    if (qty >= order->qty) return cancel(id);

    if (order->side == Side::Buy) {
        reduce_in_place(bids_, order, qty);
    } else {
        reduce_in_place(asks_, order, qty);
    }
    return true;
}

std::optional<Price> MapBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> MapBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Depth MapBook::depth(std::size_t levels) const {
    return {top_levels(bids_, levels), top_levels(asks_, levels)};
}

}  // namespace lob
