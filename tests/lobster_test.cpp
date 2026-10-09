// LOBSTER parsing and replay. The small fixture in tests/data exercises every
// data quirk; the full-day samples in data/ are replayed when present.

#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>

#include "lob/ladder_book.hpp"
#include "lob/lobster.hpp"
#include "lob/map_book.hpp"

namespace lob::lobster {
namespace {

namespace fs = std::filesystem;

// The fixture, message by message (2 levels, prices are dollars x 10,000):
//   0  submit buy 50 @ 100.00          (already included in the first snapshot)
//   1  submit sell 100 @ 101.00
//   2  execute 120 of a pre-existing sell @ 101.00  (unknown id)
//   3  hidden execution                              (book unchanged)
//   4  partial cancel 20 of order 100                (unknown id: it was in the seed)
//   5  delete order 101
//   6  trading halt                                  (book unchanged)
//   7  delete a pre-existing buy @ 99.00             (level empties -> placeholder)
//   8  execute the rest of the 101.00 ask level
//   9  cross trade                                   (book unchanged)
const std::string kMessages = std::string(LOB_TEST_DATA_DIR) + "/TEST_message_2.csv";
const std::string kSnapshots = std::string(LOB_TEST_DATA_DIR) + "/TEST_orderbook_2.csv";

// Levels drifting out of the 2-level window and changing unseen:
//   0  submit sell 10 @ 100.00          (already included in the first snapshot)
//   1  delete the pre-existing 101.00 ask; the ask side is no longer full
//   2  submit order 2: sell 20 @ 101.00
//   3  submit sell 5 @ 100.50, pushing 101.00 out of the window
//      unseen: order 2 cancelled, order 77 (sell 20 @ 101.00) and 30 @ 102.00 arrive
//   4  delete the 100.50 order: 101.00 comes back into view, same total of 20
//   5  delete order 77: 102.00 comes into view
//   6  execute the 100.00 order
//   7  partial cancel 10 of a 102.00 order nobody reported (id 88)
const std::string kWindowMessages = std::string(LOB_TEST_DATA_DIR) + "/WINDOW_message_2.csv";
const std::string kWindowSnapshots = std::string(LOB_TEST_DATA_DIR) + "/WINDOW_orderbook_2.csv";

TEST(LobsterParse, ReadsMessages) {
    const auto messages = read_messages(kMessages);
    ASSERT_EQ(messages.size(), 10u);

    const Message& m = messages[0];
    EXPECT_EQ(m.time_ns, 34'200'004'241'176);
    EXPECT_EQ(m.type, EventType::Submit);
    EXPECT_EQ(m.id, 100u);
    EXPECT_EQ(m.size, 50);
    EXPECT_EQ(m.price, 1'000'000);
    EXPECT_EQ(m.side, Side::Buy);

    EXPECT_EQ(messages[2].time_ns, 34'200'100'000'000);  // short fraction
    EXPECT_EQ(messages[3].time_ns, 34'201'000'000'000);  // no fraction
    EXPECT_EQ(messages[1].side, Side::Sell);
    EXPECT_EQ(messages[6].type, EventType::Halt);
}

TEST(LobsterParse, DropsPlaceholderLevels) {
    const auto snapshots = read_snapshots(kSnapshots);
    ASSERT_EQ(snapshots.size(), 10u);
    EXPECT_EQ(snapshots[0], (Depth{{{1'000'000, 150}, {990'000, 400}},
                                   {{1'010'000, 300}, {1'020'000, 200}}}));
    EXPECT_EQ(snapshots[8], (Depth{{{1'000'000, 130}}, {{1'020'000, 200}}}));
}

template <class Book>
class LobsterReplayTest : public ::testing::Test {};

// LOBSTER prices are dollars x 10,000 and move in whole cents.
template <class Book>
Book make_book() {
    if constexpr (std::is_same_v<Book, LadderBook>) {
        return LadderBook(100);
    } else {
        return Book{};
    }
}

using BookTypes = ::testing::Types<MapBook, LadderBook>;
TYPED_TEST_SUITE(LobsterReplayTest, BookTypes);

TYPED_TEST(LobsterReplayTest, FixtureMatchesEveryRow) {
    auto book = make_book<TypeParam>();
    std::ostringstream log;
    const auto result =
        replay_and_check(book, read_messages(kMessages), read_snapshots(kSnapshots), log, 2);

    EXPECT_EQ(result.mismatches, 0u) << log.str();
    EXPECT_EQ(result.exact_rows, 9u);
    EXPECT_EQ(result.revealed_levels, 0u);
    EXPECT_EQ(result.trades, 4u);       // messages 2, 3, 8, 9
    EXPECT_EQ(result.unknown_ids, 4u);  // messages 2, 4, 7, 8
    EXPECT_EQ(result.unexplained, 0u);
}

TYPED_TEST(LobsterReplayTest, ReloadsLevelsComingIntoView) {
    auto book = make_book<TypeParam>();
    std::ostringstream log;
    const auto result = replay_and_check(book, read_messages(kWindowMessages),
                                         read_snapshots(kWindowSnapshots), log, 2);

    EXPECT_EQ(result.mismatches, 0u) << log.str();
    EXPECT_EQ(result.unexplained, 0u);       // order 77 is found in the reloaded 101.00 level
    EXPECT_EQ(result.revealed_levels, 2u);   // 101.00 at message 4, 102.00 at message 5
    EXPECT_EQ(result.exact_rows, 6u);        // all but message 5, where 102.00 appears
    EXPECT_EQ(result.unknown_ids, 4u);       // messages 1, 5, 6, 7
    EXPECT_EQ(book.depth(2), (Depth{{{990'000, 100}}, {{1'020'000, 20}}}));
}

TYPED_TEST(LobsterReplayTest, ReportsMismatch) {
    auto snapshots = read_snapshots(kSnapshots);
    snapshots[5].asks[0].qty += 1;

    auto book = make_book<TypeParam>();
    std::ostringstream log;
    const auto result = replay_and_check(book, read_messages(kMessages), snapshots, log, 2);

    EXPECT_EQ(result.mismatches, 1u);
    EXPECT_NE(log.str().find("Mismatch at message 5: time_ns=34202000000001 type=3 id=101"),
              std::string::npos)
        << log.str();
    EXPECT_NE(log.str().find("180@1010000"), std::string::npos);
    EXPECT_NE(log.str().find("181@1010000"), std::string::npos);
}

// Replays every full-day sample found in data/ (see data/README.md).
TYPED_TEST(LobsterReplayTest, SampleDaysMatchEveryRow) {
    std::vector<fs::path> message_files;
    if (fs::exists(LOB_DATA_DIR)) {
        for (const auto& entry : fs::directory_iterator(LOB_DATA_DIR)) {
            const std::string name = entry.path().filename().string();
            if (name.ends_with("_message_10.csv")) message_files.push_back(entry.path());
        }
    }
    if (message_files.empty()) GTEST_SKIP() << "no LOBSTER samples in " << LOB_DATA_DIR;

    for (const fs::path& messages : message_files) {
        std::string book_file = messages.string();
        book_file.replace(book_file.rfind("_message_"), 9, "_orderbook_");

        auto book = make_book<TypeParam>();
        std::ostringstream log;
        const auto result = replay_and_check(book, read_messages(messages.string()),
                                             read_snapshots(book_file), log);
        EXPECT_EQ(result.mismatches, 0u)
            << messages.filename() << ": " << result.mismatches << " of " << result.messages
            << " rows differ\n"
            << log.str();
    }
}

}  // namespace
}  // namespace lob::lobster
