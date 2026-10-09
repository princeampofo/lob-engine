#include "lob/ladder_book.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace lob {

namespace {

constexpr std::int64_t kInitialLevels = 4096;

bool crosses(Side incoming_side, Price resting_price, Price limit) {
    return incoming_side == Side::Buy ? resting_price <= limit : resting_price >= limit;
}

}  // namespace

LadderBook::LadderBook(Price tick_size, std::size_t expected_orders)
    : tick_(tick_size), nodes_(expected_orders), ids_(expected_orders) {
    assert(tick_size > 0);
}

void LadderBook::add_limit(const Order& order, std::vector<Trade>& trades) {
    assert(order.qty > 0);
    assert(ids_.find(order.id) == OrderIdMap::kMissing);

    Qty remaining = order.qty;
    match(order.id, order.side, order.price, remaining, trades);
    if (remaining > 0) rest({order.id, order.side, order.price, remaining});
}

void LadderBook::add_market(OrderId id, Side side, Qty qty, std::vector<Trade>& trades) {
    assert(qty > 0);
    const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max()
                                          : std::numeric_limits<Price>::min();
    match(id, side, limit, qty, trades);
}

// Fill against the best opposite levels until filled or the next level no
// longer crosses.
void LadderBook::match(OrderId incoming_id, Side incoming_side, Price limit, Qty& qty,
                       std::vector<Trade>& trades) {
    std::int64_t& best = incoming_side == Side::Buy ? best_ask_ : best_bid_;
    while (qty > 0 && best != kNoLevel) {
        const Price price = price_at(best);
        if (!crosses(incoming_side, price, limit)) break;

        Level& level = levels_[best];
        while (qty > 0 && level.head != kNone) {
            const std::uint32_t head = level.head;
            Node& resting = nodes_[head];
            const Qty fill = std::min(qty, resting.qty);
            trades.push_back({incoming_id, resting.id, price, fill});

            qty -= fill;
            resting.qty -= fill;
            level.total -= fill;
            if (resting.qty == 0) {
                unlink(level, head);
                ids_.erase(resting.id);
                nodes_.release(head);
            }
        }
        if (level.head == kNone) level_emptied(best);
    }
}

void LadderBook::rest(const Order& order) {
    assert(order.price % tick_ == 0);
    ensure_in_ladder(order.price);
    const std::int64_t index = index_of(order.price);

    const std::uint32_t n = nodes_.allocate();
    Node& node = nodes_[n];
    Level& level = levels_[index];
    node = {order.id, order.price, order.qty, level.tail, kNone, order.side};
    if (level.tail != kNone) {
        nodes_[level.tail].next = n;
    } else {
        level.head = n;
    }
    level.tail = n;
    level.total += order.qty;
    ids_.insert(order.id, n);

    if (order.side == Side::Buy) {
        if (best_bid_ == kNoLevel || index > best_bid_) best_bid_ = index;
        if (lowest_bid_ == kNoLevel || index < lowest_bid_) lowest_bid_ = index;
    } else {
        if (best_ask_ == kNoLevel || index < best_ask_) best_ask_ = index;
        if (highest_ask_ == kNoLevel || index > highest_ask_) highest_ask_ = index;
    }
}

// Takes a node out of its level's list. Does not change the level total.
void LadderBook::unlink(Level& level, std::uint32_t n) {
    const Node& node = nodes_[n];
    if (node.prev != kNone) {
        nodes_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNone) {
        nodes_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
}

// Removes a resting order completely.
void LadderBook::remove(std::uint32_t n) {
    const Node& node = nodes_[n];
    const std::int64_t index = index_of(node.price);
    Level& level = levels_[index];
    level.total -= node.qty;
    unlink(level, n);
    ids_.erase(node.id);
    nodes_.release(n);
    if (level.head == kNone) level_emptied(index);
}

// If the emptied level was the best on its side, move to the next one.
void LadderBook::level_emptied(std::int64_t index) {
    if (index == best_bid_) best_bid_ = next_bid_at_or_below(index - 1);
    if (index == best_ask_) best_ask_ = next_ask_at_or_above(index + 1);
    if (best_bid_ == kNoLevel) lowest_bid_ = kNoLevel;
    if (best_ask_ == kNoLevel) highest_ask_ = kNoLevel;
}

// Everything below the best bid is a bid level or empty, so scanning down
// from it finds the next bid. No bid sits below `lowest_bid_`.
std::int64_t LadderBook::next_bid_at_or_below(std::int64_t index) const {
    for (; index >= lowest_bid_ && index >= 0; --index) {
        if (levels_[index].head != kNone) return index;
    }
    return kNoLevel;
}

std::int64_t LadderBook::next_ask_at_or_above(std::int64_t index) const {
    if (highest_ask_ == kNoLevel) return kNoLevel;
    for (; index <= highest_ask_; ++index) {
        if (levels_[index].head != kNone) return index;
    }
    return kNoLevel;
}

// Grows the ladder so `price` has a slot, keeping existing levels centered.
void LadderBook::ensure_in_ladder(Price price) {
    if (levels_.empty()) {
        base_ = price - (kInitialLevels / 2) * tick_;
        levels_.resize(kInitialLevels);
        return;
    }
    const std::int64_t index = index_of(price);
    if (in_ladder(index)) return;

    const std::int64_t old_size = static_cast<std::int64_t>(levels_.size());
    const std::int64_t low = std::min<std::int64_t>(0, index);
    const std::int64_t high = std::max(old_size - 1, index);
    const std::int64_t new_size = std::max(2 * old_size, 2 * (high - low + 1));
    const std::int64_t shift = -low + (new_size - (high - low + 1)) / 2;

    std::vector<Level> grown(new_size);
    std::copy(levels_.begin(), levels_.end(), grown.begin() + shift);
    levels_ = std::move(grown);
    base_ -= shift * tick_;
    if (best_bid_ != kNoLevel) best_bid_ += shift;
    if (best_ask_ != kNoLevel) best_ask_ += shift;
    if (lowest_bid_ != kNoLevel) lowest_bid_ += shift;
    if (highest_ask_ != kNoLevel) highest_ask_ += shift;
}

bool LadderBook::cancel(OrderId id) {
    const std::uint32_t n = ids_.find(id);
    if (n == OrderIdMap::kMissing) return false;
    remove(n);
    return true;
}

bool LadderBook::reduce(OrderId id, Qty qty) {
    const std::uint32_t n = ids_.find(id);
    if (n == OrderIdMap::kMissing) return false;

    Node& node = nodes_[n];
    if (qty >= node.qty) {
        remove(n);
    } else {
        node.qty -= qty;
        levels_[index_of(node.price)].total -= qty;
    }
    return true;
}

void LadderBook::clear_level(Side side, Price price) {
    if (levels_.empty() || price % tick_ != 0) return;
    const std::int64_t index = index_of(price);
    if (!in_ladder(index)) return;

    Level& level = levels_[index];
    if (level.head == kNone || nodes_[level.head].side != side) return;
    for (std::uint32_t n = level.head; n != kNone;) {
        const std::uint32_t next = nodes_[n].next;
        ids_.erase(nodes_[n].id);
        nodes_.release(n);
        n = next;
    }
    level = Level{};
    level_emptied(index);
}

std::optional<Price> LadderBook::best_bid() const {
    if (best_bid_ == kNoLevel) return std::nullopt;
    return price_at(best_bid_);
}

std::optional<Price> LadderBook::best_ask() const {
    if (best_ask_ == kNoLevel) return std::nullopt;
    return price_at(best_ask_);
}

Depth LadderBook::depth(std::size_t levels) const {
    Depth depth;
    if (best_bid_ != kNoLevel) {
        for (std::int64_t i = best_bid_; i >= lowest_bid_ && depth.bids.size() < levels; --i) {
            if (levels_[i].head != kNone) depth.bids.push_back({price_at(i), levels_[i].total});
        }
    }
    if (best_ask_ != kNoLevel) {
        for (std::int64_t i = best_ask_; i <= highest_ask_ && depth.asks.size() < levels; ++i) {
            if (levels_[i].head != kNone) depth.asks.push_back({price_at(i), levels_[i].total});
        }
    }
    return depth;
}

}  // namespace lob
