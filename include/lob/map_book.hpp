#pragma once

#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "lob/order_book.hpp"

namespace lob {

// Reference implementation: simple and easy to check, not fast.
//
//   bids_/asks_ : std::map from price to level, best price at begin()
//   level       : std::list of orders in arrival order (FIFO) + total qty
//   orders_     : order id -> where the order lives, for O(1) cancel lookup
class MapBook final : public OrderBook {
public:
    void add_limit(const Order& order, std::vector<Trade>& trades) override;
    void add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) override;
    bool cancel(OrderId id) override;
    bool reduce(OrderId id, Qty qty) override;
    void clear_level(Side side, Price price) override;

    std::optional<Price> best_bid() const override;
    std::optional<Price> best_ask() const override;
    Depth depth(std::size_t levels) const override;

private:
    struct PriceLevel {
        std::list<Order> orders;
        Qty total = 0;
    };

    // Bids sort high to low and asks low to high, so begin() is the best
    // price on both sides.
    using Bids = std::map<Price, PriceLevel, std::greater<>>;
    using Asks = std::map<Price, PriceLevel, std::less<>>;

    template <class Levels>
    void match(Levels& levels, OrderId incoming_id, Side incoming_side, Price limit, Qty& qty,
               std::vector<Trade>& trades);

    template <class Levels>
    void rest(Levels& levels, const Order& order);

    template <class Levels>
    static void erase(Levels& levels, std::list<Order>::iterator order);

    template <class Levels>
    void clear(Levels& levels, Price price);

    template <class Levels>
    static void reduce_in_place(Levels& levels, std::list<Order>::iterator order, Qty qty);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, std::list<Order>::iterator> orders_;
};

}  // namespace lob
