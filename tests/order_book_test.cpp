// Hand-written cases, run against every book implementation (typed tests).

#include <gtest/gtest.h>

#include "lob/ladder_book.hpp"
#include "lob/map_book.hpp"
#include "oracle_book.hpp"

namespace lob {
namespace {

template <class Book>
class OrderBookTest : public ::testing::Test {
protected:
    // Adds a limit order and returns the trades it caused.
    std::vector<Trade> limit(OrderId id, Side side, Price price, Qty qty) {
        std::vector<Trade> trades;
        book.add_limit({id, side, price, qty}, trades);
        return trades;
    }

    std::vector<Trade> market(OrderId id, Side side, Qty qty) {
        std::vector<Trade> trades;
        book.add_market(id, side, qty, trades);
        return trades;
    }

    Book book;
};

using BookTypes = ::testing::Types<MapBook, LadderBook, OracleBook>;
TYPED_TEST_SUITE(OrderBookTest, BookTypes);

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;

TYPED_TEST(OrderBookTest, EmptyBook) {
    auto& book = this->book;
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.depth(10), Depth{});
    EXPECT_FALSE(book.cancel(1));
    EXPECT_FALSE(book.reduce(1, 5));
    EXPECT_TRUE(this->market(1, kBuy, 100).empty());
    EXPECT_TRUE(this->market(2, kSell, 100).empty());
    EXPECT_EQ(book.depth(10), Depth{});
}

TYPED_TEST(OrderBookTest, NonCrossingOrdersRest) {
    EXPECT_TRUE(this->limit(1, kBuy, 100, 10).empty());
    EXPECT_TRUE(this->limit(2, kBuy, 99, 5).empty());
    EXPECT_TRUE(this->limit(3, kBuy, 100, 7).empty());
    EXPECT_TRUE(this->limit(4, kSell, 101, 3).empty());
    EXPECT_TRUE(this->limit(5, kSell, 103, 4).empty());

    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.best_ask(), 101);
    Depth expected{{{100, 17}, {99, 5}}, {{101, 3}, {103, 4}}};
    EXPECT_EQ(this->book.depth(10), expected);
}

TYPED_TEST(OrderBookTest, DepthIsLimitedToRequestedLevels) {
    for (Price p = 1; p <= 5; ++p) {
        this->limit(p, kBuy, 100 - p, 1);
        this->limit(10 + p, kSell, 100 + p, 1);
    }
    Depth expected{{{99, 1}, {98, 1}}, {{101, 1}, {102, 1}}};
    EXPECT_EQ(this->book.depth(2), expected);
}

TYPED_TEST(OrderBookTest, CrossingLimitTradesAtRestingPrice) {
    this->limit(1, kSell, 100, 10);
    auto trades = this->limit(2, kBuy, 105, 10);

    EXPECT_EQ(trades, (std::vector<Trade>{{2, 1, 100, 10}}));
    EXPECT_EQ(this->book.depth(10), Depth{});
}

TYPED_TEST(OrderBookTest, TimePriorityWithinLevel) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kSell, 100, 10);
    auto trades = this->limit(3, kBuy, 100, 15);

    EXPECT_EQ(trades, (std::vector<Trade>{{3, 1, 100, 10}, {3, 2, 100, 5}}));
    EXPECT_EQ(this->book.depth(10), (Depth{{}, {{100, 5}}}));
}

TYPED_TEST(OrderBookTest, PricePriorityBeatsTimePriority) {
    this->limit(1, kBuy, 99, 10);
    this->limit(2, kBuy, 100, 10);  // later, but a better price
    auto trades = this->limit(3, kSell, 99, 15);

    EXPECT_EQ(trades, (std::vector<Trade>{{3, 2, 100, 10}, {3, 1, 99, 5}}));
    EXPECT_EQ(this->book.depth(10), (Depth{{{99, 5}}, {}}));
}

TYPED_TEST(OrderBookTest, PartialFillAcrossLevelsThenRest) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 101, 5);
    this->limit(3, kSell, 102, 5);
    auto trades = this->limit(4, kBuy, 101, 12);

    EXPECT_EQ(trades, (std::vector<Trade>{{4, 1, 100, 5}, {4, 2, 101, 5}}));
    // The unfilled 2 shares rest at the order's limit price.
    EXPECT_EQ(this->book.best_bid(), 101);
    EXPECT_EQ(this->book.best_ask(), 102);
    EXPECT_EQ(this->book.depth(10), (Depth{{{101, 2}}, {{102, 5}}}));
}

TYPED_TEST(OrderBookTest, CancelPartlyFilledOrder) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kBuy, 100, 4);
    EXPECT_EQ(this->book.depth(10), (Depth{{}, {{100, 6}}}));

    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_EQ(this->book.depth(10), Depth{});
    EXPECT_FALSE(this->book.cancel(1));
}

TYPED_TEST(OrderBookTest, CancelFromMiddleOfQueue) {
    this->limit(1, kBuy, 100, 1);
    this->limit(2, kBuy, 100, 2);
    this->limit(3, kBuy, 100, 3);
    EXPECT_TRUE(this->book.cancel(2));
    EXPECT_EQ(this->book.depth(10), (Depth{{{100, 4}}, {}}));

    auto trades = this->market(4, kSell, 4);
    EXPECT_EQ(trades, (std::vector<Trade>{{4, 1, 100, 1}, {4, 3, 100, 3}}));
}

TYPED_TEST(OrderBookTest, FilledOrderIsGone) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kBuy, 100, 5);
    EXPECT_FALSE(this->book.cancel(1));
    EXPECT_FALSE(this->book.reduce(1, 1));
    EXPECT_FALSE(this->book.cancel(2));  // fully filled on arrival, never rested
}

TYPED_TEST(OrderBookTest, ReduceKeepsQueuePosition) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kSell, 100, 10);
    EXPECT_TRUE(this->book.reduce(1, 7));
    EXPECT_EQ(this->book.depth(10), (Depth{{}, {{100, 13}}}));

    auto trades = this->market(3, kBuy, 5);
    EXPECT_EQ(trades, (std::vector<Trade>{{3, 1, 100, 3}, {3, 2, 100, 2}}));
}

TYPED_TEST(OrderBookTest, ReduceByFullQuantityRemovesOrder) {
    this->limit(1, kBuy, 100, 10);
    EXPECT_TRUE(this->book.reduce(1, 10));
    EXPECT_EQ(this->book.depth(10), Depth{});
    EXPECT_FALSE(this->book.cancel(1));
}

TYPED_TEST(OrderBookTest, MarketOrderLargerThanWholeBook) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 101, 5);
    this->limit(3, kBuy, 90, 5);
    auto trades = this->market(4, kBuy, 100);

    EXPECT_EQ(trades, (std::vector<Trade>{{4, 1, 100, 5}, {4, 2, 101, 5}}));
    // The unfilled remainder is dropped, not rested; the bid side is untouched.
    EXPECT_EQ(this->book.depth(10), (Depth{{{90, 5}}, {}}));
}

TYPED_TEST(OrderBookTest, MarketSellWalksBidsFromHighest) {
    this->limit(1, kBuy, 98, 5);
    this->limit(2, kBuy, 100, 5);
    this->limit(3, kBuy, 99, 5);
    auto trades = this->market(4, kSell, 12);

    EXPECT_EQ(trades, (std::vector<Trade>{{4, 2, 100, 5}, {4, 3, 99, 5}, {4, 1, 98, 2}}));
    EXPECT_EQ(this->book.depth(10), (Depth{{{98, 3}}, {}}));
}

TYPED_TEST(OrderBookTest, ClearLevelRemovesEveryOrderAtThatPrice) {
    this->limit(1, kBuy, 100, 5);
    this->limit(2, kBuy, 100, 6);
    this->limit(3, kBuy, 99, 7);
    this->limit(4, kSell, 105, 8);
    this->book.clear_level(kBuy, 100);
    this->book.clear_level(kBuy, 98);  // no such level: no effect

    EXPECT_EQ(this->book.depth(10), (Depth{{{99, 7}}, {{105, 8}}}));
    EXPECT_FALSE(this->book.cancel(1));
    EXPECT_FALSE(this->book.cancel(2));
    EXPECT_TRUE(this->book.cancel(3));
}

TYPED_TEST(OrderBookTest, PricesFarApart) {
    // Far beyond the ladder's initial range on both sides.
    this->limit(1, kBuy, 1'000, 5);
    this->limit(2, kSell, 1'000'000, 5);
    this->limit(3, kBuy, 10, 5);
    this->limit(4, kSell, 2'000'000, 5);
    EXPECT_EQ(this->book.depth(10), (Depth{{{1'000, 5}, {10, 5}}, {{1'000'000, 5}, {2'000'000, 5}}}));

    auto trades = this->market(5, kSell, 7);
    EXPECT_EQ(trades, (std::vector<Trade>{{5, 1, 1'000, 5}, {5, 3, 10, 2}}));
    trades = this->limit(6, kBuy, 3'000'000, 10);
    EXPECT_EQ(trades, (std::vector<Trade>{{6, 2, 1'000'000, 5}, {6, 4, 2'000'000, 5}}));
    EXPECT_EQ(this->book.depth(10), (Depth{{{10, 3}}, {}}));
}

TYPED_TEST(OrderBookTest, NegativePrices) {
    this->limit(1, kBuy, -5, 5);
    this->limit(2, kSell, -2, 5);
    EXPECT_EQ(this->book.best_bid(), -5);
    EXPECT_EQ(this->book.best_ask(), -2);
    auto trades = this->limit(3, kSell, -6, 5);
    EXPECT_EQ(trades, (std::vector<Trade>{{3, 1, -5, 5}}));
}

TYPED_TEST(OrderBookTest, IdCanBeReusedAfterOrderLeaves) {
    this->limit(1, kBuy, 100, 5);
    this->book.cancel(1);
    this->limit(1, kSell, 105, 5);
    EXPECT_EQ(this->book.depth(10), (Depth{{}, {{105, 5}}}));
}

}  // namespace
}  // namespace lob
