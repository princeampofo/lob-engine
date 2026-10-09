// Microbenchmarks for the three hot operations (add, cancel, match) on each
// book. Every benchmark iteration runs a batch of operations; setup and
// cleanup happen outside the timed region, and the per-operation time is
// reported in the `per_op` column.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <random>

#include "lob/ladder_book.hpp"
#include "lob/map_book.hpp"

namespace lob {
namespace {

constexpr Price kMid = 10'000;
constexpr Price kBand = 100;           // background orders sit within kBand ticks of mid
constexpr int kBackgroundOrders = 10'000;
constexpr int kBatch = 1'000;

template <class Book>
Book make_book() {
    if constexpr (std::is_same_v<Book, LadderBook>) {
        return LadderBook(1, 1 << 16);
    } else {
        return Book{};
    }
}

// A resting order that does not cross: bids below mid, asks above.
Order random_resting_order(OrderId id, std::mt19937_64& rng) {
    const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
    const Price offset = 1 + static_cast<Price>(rng() % kBand);
    const Price price = side == Side::Buy ? kMid - offset : kMid + offset;
    return {id, side, price, static_cast<Qty>(rng() % 100 + 1)};
}

template <class Book>
OrderId add_background(Book& book, std::mt19937_64& rng) {
    std::vector<Trade> trades;
    OrderId id = 1;
    for (; id <= kBackgroundOrders; ++id) book.add_limit(random_resting_order(id, rng), trades);
    return id;
}

void report_per_op(benchmark::State& state) {
    state.counters["per_op"] = benchmark::Counter(
        static_cast<double>(state.iterations()) * kBatch,
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// Add a resting order to a book holding ~10,000 orders.
template <class Book>
void BM_AddLimit(benchmark::State& state) {
    auto book = make_book<Book>();
    std::mt19937_64 rng(1);
    OrderId next_id = add_background(book, rng);
    std::vector<Trade> trades;
    std::vector<Order> batch(kBatch);

    for (auto _ : state) {
        state.PauseTiming();
        for (Order& order : batch) order = random_resting_order(next_id++, rng);
        state.ResumeTiming();

        for (const Order& order : batch) book.add_limit(order, trades);

        state.PauseTiming();
        for (const Order& order : batch) book.cancel(order.id);
        state.ResumeTiming();
    }
    report_per_op(state);
}

// Cancel a resting order, chosen in random order, from a book holding
// ~10,000 orders.
template <class Book>
void BM_Cancel(benchmark::State& state) {
    auto book = make_book<Book>();
    std::mt19937_64 rng(2);
    OrderId next_id = add_background(book, rng);
    std::vector<Trade> trades;
    std::vector<OrderId> batch(kBatch);

    for (auto _ : state) {
        state.PauseTiming();
        for (OrderId& id : batch) {
            id = next_id++;
            book.add_limit(random_resting_order(id, rng), trades);
        }
        std::shuffle(batch.begin(), batch.end(), rng);
        state.ResumeTiming();

        for (const OrderId id : batch) benchmark::DoNotOptimize(book.cancel(id));
    }
    report_per_op(state);
}

// A market buy that fully fills one resting ask. The asks are spread over 5
// levels, so the batch also empties levels and moves the best ask.
template <class Book>
void BM_Match(benchmark::State& state) {
    auto book = make_book<Book>();
    std::mt19937_64 rng(3);
    std::vector<Trade> trades;
    trades.reserve(kBatch);
    OrderId next_id = 1;
    for (; next_id <= kBackgroundOrders; ++next_id) {  // bids only, as background
        const Price offset = 1 + static_cast<Price>(rng() % kBand);
        book.add_limit({next_id, Side::Buy, kMid - offset, 100}, trades);
    }

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < kBatch; ++i) {
            book.add_limit({next_id++, Side::Sell, kMid + 1 + i % 5, 10}, trades);
        }
        trades.clear();
        state.ResumeTiming();

        for (int i = 0; i < kBatch; ++i) book.add_market(next_id++, Side::Buy, 10, trades);
    }
    report_per_op(state);
}

BENCHMARK_TEMPLATE(BM_AddLimit, MapBook);
BENCHMARK_TEMPLATE(BM_AddLimit, LadderBook);
BENCHMARK_TEMPLATE(BM_Cancel, MapBook);
BENCHMARK_TEMPLATE(BM_Cancel, LadderBook);
BENCHMARK_TEMPLATE(BM_Match, MapBook);
BENCHMARK_TEMPLATE(BM_Match, LadderBook);

}  // namespace
}  // namespace lob

BENCHMARK_MAIN();
