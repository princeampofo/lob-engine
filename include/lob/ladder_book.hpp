#pragma once

#include <cstdint>
#include <vector>

#include "lob/object_pool.hpp"
#include "lob/order_book.hpp"
#include "lob/order_id_map.hpp"

namespace lob {

// Optimized implementation.
//
//   levels_ : a price ladder, one slot per tick from `base_` upward, so
//             finding a level is one array index. Bids and asks share it:
//             every bid is below every ask, so a slot only ever holds one side.
//   level   : an intrusive FIFO list (prev/next live in the order node) + total
//   nodes_  : all orders, preallocated in a pool and addressed by index
//   ids_    : flat hash map from order id to node index
//
// Once the pool, id map and ladder are big enough, add, cancel and match do
// not allocate. Prices must be multiples of `tick_size`. A price outside the
// ladder makes it grow and re-center, which does allocate.
class LadderBook final : public OrderBook {
public:
    explicit LadderBook(Price tick_size = 1, std::size_t expected_orders = 1 << 16);

    void add_limit(const Order& order, std::vector<Trade>& trades) override;
    void add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) override;
    bool cancel(OrderId id) override;
    bool reduce(OrderId id, Qty qty) override;
    void clear_level(Side side, Price price) override;

    std::optional<Price> best_bid() const override;
    std::optional<Price> best_ask() const override;
    Depth depth(std::size_t levels) const override;

private:
    static constexpr std::uint32_t kNone = UINT32_MAX;  // null node index
    static constexpr std::int64_t kNoLevel = -1;        // side is empty

    struct Node {
        OrderId id = 0;
        Price price = 0;
        Qty qty = 0;
        std::uint32_t prev = kNone;
        std::uint32_t next = kNone;
        Side side = Side::Buy;
    };

    struct Level {
        std::uint32_t head = kNone;  // oldest order
        std::uint32_t tail = kNone;  // newest order
        Qty total = 0;
    };

    std::int64_t index_of(Price price) const { return (price - base_) / tick_; }
    Price price_at(std::int64_t index) const { return base_ + index * tick_; }
    bool in_ladder(std::int64_t index) const {
        return index >= 0 && index < static_cast<std::int64_t>(levels_.size());
    }

    void match(OrderId incoming_id, Side incoming_side, Price limit, Qty& qty,
               std::vector<Trade>& trades);
    void rest(const Order& order);
    void unlink(Level& level, std::uint32_t node);
    void remove(std::uint32_t node);
    void level_emptied(std::int64_t index);
    std::int64_t next_bid_at_or_below(std::int64_t index) const;
    std::int64_t next_ask_at_or_above(std::int64_t index) const;
    void ensure_in_ladder(Price price);

    Price tick_;
    Price base_ = 0;  // price of levels_[0]
    std::vector<Level> levels_;
    std::int64_t best_bid_ = kNoLevel;
    std::int64_t best_ask_ = kNoLevel;
    // Bounds on where bids and asks can be, so scans for the next level stop
    // there instead of running to the end of the ladder.
    std::int64_t lowest_bid_ = kNoLevel;
    std::int64_t highest_ask_ = kNoLevel;
    ObjectPool<Node> nodes_;
    OrderIdMap ids_;
};

}  // namespace lob
