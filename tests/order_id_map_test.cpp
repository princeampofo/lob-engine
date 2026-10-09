#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "lob/order_id_map.hpp"

namespace lob {
namespace {

TEST(OrderIdMap, InsertFindErase) {
    OrderIdMap map(4);
    EXPECT_EQ(map.find(7), OrderIdMap::kMissing);
    map.insert(7, 70);
    map.insert(0, 1);  // id 0 is an ordinary key
    EXPECT_EQ(map.find(7), 70u);
    EXPECT_EQ(map.find(0), 1u);
    map.erase(7);
    map.erase(12345);  // not present: no effect
    EXPECT_EQ(map.find(7), OrderIdMap::kMissing);
    EXPECT_EQ(map.find(0), 1u);
    EXPECT_EQ(map.size(), 1u);
}

// Random inserts and erases, checked against std::unordered_map. A small
// starting size forces many rehashes; clustered ids force long probe runs.
TEST(OrderIdMap, MatchesUnorderedMap) {
    OrderIdMap map(8);
    std::unordered_map<OrderId, std::uint32_t> expected;
    std::mt19937_64 rng(7);

    for (std::uint32_t step = 0; step < 200'000; ++step) {
        const OrderId id = rng() % 5'000 + (rng() % 2 == 0 ? 0 : (OrderId{1} << 62));
        if (expected.contains(id)) {
            map.erase(id);
            expected.erase(id);
        } else {
            map.insert(id, step);
            expected[id] = step;
        }
        if (step % 1'000 == 0) {
            ASSERT_EQ(map.size(), expected.size());
            for (const auto& [key, value] : expected) ASSERT_EQ(map.find(key), value);
        }
        ASSERT_EQ(map.find(id), expected.contains(id) ? expected[id] : OrderIdMap::kMissing);
    }
}

}  // namespace
}  // namespace lob
