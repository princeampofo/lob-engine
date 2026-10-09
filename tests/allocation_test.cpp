// Checks that the ladder book does not touch the heap while trading, by
// counting every call to the global operator new.

#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <random>

#include "lob/ladder_book.hpp"
#include "lob/map_book.hpp"

// Every plain, array and nothrow form is replaced, so whatever form allocates,
// the matching delete frees memory from the same allocator (malloc).
namespace {
std::size_t g_allocations = 0;

void* counted_malloc(std::size_t size) noexcept {
    ++g_allocations;
    return std::malloc(size == 0 ? 1 : size);
}
}  // namespace

void* operator new(std::size_t size) {
    if (void* p = counted_malloc(size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (void* p = counted_malloc(size)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_malloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return counted_malloc(size); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace lob {
namespace {

// Adds, cancels, reductions and crossing orders in a fixed price band.
template <class Book>
std::size_t allocations_while_trading(Book& book) {
    std::vector<Trade> trades;
    trades.reserve(1'000);
    std::mt19937_64 rng(1);

    const std::size_t before = g_allocations;
    OrderId next_id = 1;
    for (int op = 0; op < 200'000; ++op) {
        trades.clear();
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        const OrderId recent = next_id - 1 - rng() % std::min<OrderId>(next_id, 500);
        switch (rng() % 4) {
            case 0:
            case 1:
                book.add_limit({next_id++, side, 10'000 + static_cast<Price>(rng() % 41) - 20,
                                static_cast<Qty>(rng() % 100 + 1)},
                               trades);
                break;
            case 2:
                book.cancel(recent);
                break;
            case 3:
                book.reduce(recent, static_cast<Qty>(rng() % 50 + 1));
                break;
        }
    }
    return g_allocations - before;
}

TEST(Allocation, LadderBookDoesNotAllocateWhileTrading) {
    LadderBook book(1, 1 << 16);
    std::vector<Trade> trades;
    book.add_limit({0, Side::Buy, 10'000, 1}, trades);  // creates the ladder
    book.cancel(0);

    EXPECT_EQ(allocations_while_trading(book), 0u);
}

// Shows the counter works: the map book allocates a node per order.
TEST(Allocation, MapBookAllocatesPerOrder) {
    MapBook book;
    EXPECT_GT(allocations_while_trading(book), 10'000u);
}

}  // namespace
}  // namespace lob
