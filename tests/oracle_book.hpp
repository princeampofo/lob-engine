#pragma once

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>

#include "lob/order_book.hpp"

namespace lob {

// A deliberately naive book used only in tests: one vector of every resting
// order in arrival order, searched from scratch on every operation. Far too
// slow for real use, but simple enough to be obviously correct.
class OracleBook final : public OrderBook {
public:
    void add_limit(const Order& order, std::vector<Trade>& trades) override {
        assert(order.qty > 0);
        assert(find(order.id) == orders_.end());
        Order remaining = order;
        match(order.id, order.side, order.price, remaining.qty, trades);
        if (remaining.qty > 0) orders_.push_back(remaining);
    }

    void add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) override {
        assert(qty > 0);
        const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max()
                                              : std::numeric_limits<Price>::min();
        match(id, side, limit, qty, trades);
    }

    bool cancel(OrderId id) override {
        auto it = find(id);
        if (it == orders_.end()) return false;
        orders_.erase(it);
        return true;
    }

    bool reduce(OrderId id, Qty qty) override {
        auto it = find(id);
        if (it == orders_.end()) return false;
        if (qty >= it->qty) {
            orders_.erase(it);
        } else {
            it->qty -= qty;  // stays at the same index, so keeps its priority
        }
        return true;
    }

    void clear_level(Side side, Price price) override {
        std::erase_if(orders_, [&](const Order& o) { return o.side == side && o.price == price; });
    }

    std::optional<Price> best_bid() const override {
        Depth d = depth(1);
        if (d.bids.empty()) return std::nullopt;
        return d.bids[0].price;
    }

    std::optional<Price> best_ask() const override {
        Depth d = depth(1);
        if (d.asks.empty()) return std::nullopt;
        return d.asks[0].price;
    }

    Depth depth(std::size_t levels) const override {
        std::map<Price, Qty, std::greater<>> bids;
        std::map<Price, Qty> asks;
        for (const Order& o : orders_) {
            if (o.side == Side::Buy) {
                bids[o.price] += o.qty;
            } else {
                asks[o.price] += o.qty;
            }
        }
        Depth d;
        for (auto [price, qty] : bids) {
            if (d.bids.size() == levels) break;
            d.bids.push_back({price, qty});
        }
        for (auto [price, qty] : asks) {
            if (d.asks.size() == levels) break;
            d.asks.push_back({price, qty});
        }
        return d;
    }

private:
    std::vector<Order>::iterator find(OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(),
                            [id](const Order& o) { return o.id == id; });
    }

    // Index of the resting order an incoming order should trade with next:
    // the best crossing price, and among equal prices the earliest arrival.
    // Returns -1 if nothing crosses.
    int best_match(Side incoming_side, Price limit) const {
        int best = -1;
        for (int i = 0; i < static_cast<int>(orders_.size()); ++i) {
            const Order& o = orders_[i];
            if (o.side == incoming_side) continue;
            const bool crosses = incoming_side == Side::Buy ? o.price <= limit : o.price >= limit;
            if (!crosses) continue;
            if (best == -1) {
                best = i;
                continue;
            }
            const bool better = incoming_side == Side::Buy ? o.price < orders_[best].price
                                                           : o.price > orders_[best].price;
            if (better) best = i;  // strictly better, so ties keep the earlier order
        }
        return best;
    }

    void match(OrderId id, Side side, Price limit, Qty& qty, std::vector<Trade>& trades) {
        while (qty > 0) {
            const int i = best_match(side, limit);
            if (i == -1) break;
            Order& resting = orders_[i];
            const Qty fill = std::min(qty, resting.qty);
            trades.push_back({id, resting.id, resting.price, fill});
            qty -= fill;
            resting.qty -= fill;
            if (resting.qty == 0) orders_.erase(orders_.begin() + i);
        }
    }

    std::vector<Order> orders_;  // every resting order, oldest first
};

}  // namespace lob
