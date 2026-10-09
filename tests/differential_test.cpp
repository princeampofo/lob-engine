// Random differential test: apply the same random operations to a real book
// and to the naive oracle, and require identical trades, return values and
// book state after every operation.

#include <gtest/gtest.h>

#include <random>

#include "lob/ladder_book.hpp"
#include "lob/map_book.hpp"
#include "oracle_book.hpp"

namespace lob {
namespace {

constexpr int kOperations = 1'000'000;
constexpr Price kMidPrice = 10'000;
constexpr Price kPriceRange = 20;    // limit prices are mid +/- this many ticks
constexpr OrderId kCancelWindow = 500;  // cancels target one of the last N ids

template <class Book>
class DifferentialTest : public ::testing::Test {};

using BookTypes = ::testing::Types<MapBook, LadderBook>;
TYPED_TEST_SUITE(DifferentialTest, BookTypes);

TYPED_TEST(DifferentialTest, MatchesOracleOnRandomOperations) {
    TypeParam book;
    OracleBook oracle;
    std::vector<Trade> book_trades;
    std::vector<Trade> oracle_trades;

    std::mt19937_64 rng(42);
    auto uniform = [&rng](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };

    OrderId next_id = 1;
    for (int op = 0; op < kOperations; ++op) {
        book_trades.clear();
        oracle_trades.clear();

        const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
        const std::int64_t kind = uniform(0, 99);

        if (kind < 50) {
            // Now and then a price far from the action, to exercise deep books.
            const Price offset = uniform(0, 999) == 0 ? uniform(-5'000, 5'000)
                                                      : uniform(-kPriceRange, kPriceRange);
            const Order order{next_id++, side, kMidPrice + offset, uniform(1, 100)};
            book.add_limit(order, book_trades);
            oracle.add_limit(order, oracle_trades);
        } else if (kind < 60) {
            const OrderId id = next_id++;
            const Qty qty = uniform(1, 300);
            book.add_market(id, side, qty, book_trades);
            oracle.add_market(id, side, qty, oracle_trades);
        } else {
            // Target a recent id; it may be resting, already filled or cancelled.
            const OrderId oldest = next_id > kCancelWindow ? next_id - kCancelWindow : 1;
            const OrderId id = static_cast<OrderId>(
                uniform(static_cast<std::int64_t>(oldest), static_cast<std::int64_t>(next_id)));
            if (kind == 99) {
                const Price price = kMidPrice + uniform(-kPriceRange, kPriceRange);
                book.clear_level(side, price);
                oracle.clear_level(side, price);
            } else if (kind < 85) {
                ASSERT_EQ(book.cancel(id), oracle.cancel(id)) << "op " << op;
            } else {
                const Qty qty = uniform(1, 100);
                ASSERT_EQ(book.reduce(id, qty), oracle.reduce(id, qty)) << "op " << op;
            }
        }

        ASSERT_EQ(book_trades, oracle_trades) << "op " << op;
        ASSERT_EQ(book.best_bid(), oracle.best_bid()) << "op " << op;
        ASSERT_EQ(book.best_ask(), oracle.best_ask()) << "op " << op;
        ASSERT_EQ(book.depth(10), oracle.depth(10)) << "op " << op;
        if (op % 1000 == 0) {
            ASSERT_EQ(book.depth(1000), oracle.depth(1000)) << "op " << op;
        }
    }
}

}  // namespace
}  // namespace lob
