#include <gtest/gtest.h>

#include "lob/backtest.hpp"
#include "lob/ladder_book.hpp"
#include "lob/map_book.hpp"

namespace lob::backtest {
namespace {

using lobster::EventType;
using lobster::Message;

constexpr std::int64_t kT0 = 1'000'000'000;  // 1 s after midnight
constexpr std::int64_t kMicro = 1'000;

// The order book rows that go with `messages`, starting from `first`.
std::vector<Depth> snapshots_for(const std::vector<Message>& messages, const Depth& first) {
    MapBook book;
    lobster::Replayer replayer(book);
    replayer.seed(first);
    std::vector<Depth> rows = {first};
    for (std::size_t i = 1; i < messages.size(); ++i) {
        replayer.apply(messages[i]);
        rows.push_back(book.depth(10));
    }
    return rows;
}

// Sends one order on its first update and records what it sees.
class ScriptedStrategy final : public Strategy {
public:
    explicit ScriptedStrategy(StrategyOrder order) : order_(order) {}

    void on_book_update(std::int64_t time_ns, const OrderBook&,
                        std::vector<StrategyOrder>& orders) override {
        updates.push_back(time_ns);
        if (updates.size() == 1) orders.push_back(order_);
    }
    void on_fill(const Fill& fill) override { fills.push_back(fill); }

    std::vector<std::int64_t> updates;
    std::vector<Fill> fills;

private:
    StrategyOrder order_;
};

TEST(Backtest, OrdersArriveAfterLatencyAndWalkTheBook) {
    const std::vector<Message> messages = {
        {kT0, EventType::Submit, 1, 300, 1'000'100, Side::Sell},  // in the first snapshot
        {kT0 + 10 * kMicro, EventType::Submit, 2, 10, 999'800, Side::Buy},  // strategy buys here
        {kT0 + 30 * kMicro, EventType::Submit, 3, 50, 1'000'000, Side::Sell},  // before arrival
        {kT0 + 100 * kMicro, EventType::Delete, 3, 50, 1'000'000, Side::Sell},  // after arrival
    };
    const Depth first{{{999'900, 500}}, {{1'000'100, 300}}};

    ScriptedStrategy strategy({Side::Buy, 100});
    const auto fills = run(messages, snapshots_for(messages, first), {&strategy});

    ASSERT_EQ(fills[0].size(), 1u);
    const Fill& fill = fills[0][0];
    EXPECT_EQ(fill.sent_ns, kT0 + 10 * kMicro);
    EXPECT_EQ(fill.time_ns, kT0 + 60 * kMicro);  // default latency is 50 us
    EXPECT_EQ(fill.qty, 100);
    EXPECT_EQ(fill.notional, 50 * 1'000'000 + 50 * 1'000'100);  // two levels
    EXPECT_EQ(fill.fee, 100 * 30);
    EXPECT_EQ(strategy.fills.size(), 1u);
}

TEST(Backtest, OneUpdatePerTimestamp) {
    const std::vector<Message> messages = {
        {kT0, EventType::Submit, 1, 300, 1'000'100, Side::Sell},
        {kT0 + 1, EventType::Submit, 2, 10, 999'800, Side::Buy},
        {kT0 + 1, EventType::Submit, 3, 10, 999'700, Side::Buy},  // same time as previous
        {kT0 + 2, EventType::Delete, 2, 10, 999'800, Side::Buy},
    };
    const Depth first{{{999'900, 500}}, {{1'000'100, 300}}};

    ScriptedStrategy strategy({Side::Buy, 1});
    run(messages, snapshots_for(messages, first), {&strategy});
    EXPECT_EQ(strategy.updates, (std::vector<std::int64_t>{kT0 + 1, kT0 + 2}));
}

TEST(Backtest, ThinBookFillsPartly) {
    const std::vector<Message> messages = {
        {kT0, EventType::Submit, 1, 30, 1'000'100, Side::Sell},
        {kT0 + 1, EventType::Submit, 2, 10, 999'800, Side::Buy},
    };
    const Depth first{{{999'900, 500}}, {{1'000'100, 30}}};

    ScriptedStrategy strategy({Side::Buy, 100});
    const auto fills = run(messages, snapshots_for(messages, first), {&strategy});
    ASSERT_EQ(fills[0].size(), 1u);
    EXPECT_EQ(fills[0][0].qty, 30);
}

TEST(Backtest, Imbalance) {
    LadderBook book;
    std::vector<Trade> trades;
    book.add_limit({1, Side::Buy, 100, 300}, trades);
    book.add_limit({2, Side::Buy, 99, 100}, trades);
    book.add_limit({3, Side::Buy, 98, 100}, trades);
    book.add_limit({4, Side::Buy, 97, 1000}, trades);  // 4th level: ignored
    book.add_limit({5, Side::Sell, 101, 100}, trades);
    EXPECT_DOUBLE_EQ(imbalance(book, 3), (500.0 - 100.0) / 600.0);
    EXPECT_EQ(imbalance(LadderBook{}, 3), 0.0);
}

// Bids heavily outweigh asks, so the strategy buys at the ask, then sells at
// the bid once the holding time is up: a loss of the spread plus fees.
std::vector<Message> imbalanced_day() {
    return {
        {kT0, EventType::Submit, 1, 900, 999'900, Side::Buy},
        {kT0 + 10 * kMicro, EventType::Submit, 2, 10, 999'800, Side::Buy},      // enter: buy
        {kT0 + 500 * kMicro, EventType::Submit, 3, 200, 1'000'000, Side::Sell},  // holding
        {kT0 + 2'000 * kMicro, EventType::Delete, 2, 10, 999'800, Side::Buy},    // exit: sell
    };
}

TEST(Backtest, ImbalanceStrategyRoundTrip) {
    const auto messages = imbalanced_day();
    const Depth first{{{999'900, 900}}, {{1'000'100, 100}}};

    ImbalanceParams params;
    params.threshold = 0.5;
    params.holding_ns = 1'000 * kMicro;
    ImbalanceStrategy strategy(params);
    run(messages, snapshots_for(messages, first), {&strategy});

    ASSERT_EQ(strategy.round_trips().size(), 1u);
    const RoundTrip& trip = strategy.round_trips()[0];
    EXPECT_EQ(trip.side, Side::Buy);
    EXPECT_EQ(trip.entry_ns, kT0 + 60 * kMicro);
    EXPECT_EQ(trip.exit_ns, kT0 + 2'050 * kMicro);
    EXPECT_EQ(trip.entry_notional, 100 * 1'000'100);
    EXPECT_EQ(trip.exit_notional, 100 * 999'900);
    // Mid 100.00 at entry; at exit the best ask is 100.00, so mid 99.995.
    EXPECT_EQ(trip.entry_mid_x2, 999'900 + 1'000'100);
    EXPECT_EQ(trip.exit_mid_x2, 999'900 + 1'000'000);
    EXPECT_EQ(trip.at_mid_x2(), -100 * 100);

    const Summary summary = summarize(strategy.round_trips());
    EXPECT_EQ(summary.trades, 1u);
    EXPECT_EQ(summary.gross, -20'000);  // 2 cents x 100 shares = $2.00
    EXPECT_EQ(summary.fees, 6'000);     // 200 shares x $0.003
    EXPECT_EQ(summary.net, -26'000);
    EXPECT_EQ(summary.hit_rate, 0.0);
}

TEST(Backtest, ImbalanceStrategyOnlyEntersInsideItsWindow) {
    const auto messages = imbalanced_day();
    const Depth first{{{999'900, 900}}, {{1'000'100, 100}}};

    ImbalanceParams params;
    params.threshold = 0.6;  // only the signal at +10 us (0.80) reaches it
    params.holding_ns = 1'000 * kMicro;
    params.start_ns = kT0 + 100 * kMicro;  // after that signal
    ImbalanceStrategy late(params);
    params.start_ns = 0;
    params.end_ns = kT0 + 500 * kMicro;  // the exit would land past the end
    ImbalanceStrategy early(params);
    run(messages, snapshots_for(messages, first), {&late, &early});

    EXPECT_TRUE(late.round_trips().empty());
    EXPECT_TRUE(early.round_trips().empty());
}

TEST(Backtest, NextMoveCorrelation) {
    // Mids 10, 10, 12, 12, 10 (as mid x 2): next moves +2, +2, -2, -2, none.
    std::vector<SignalSample> samples = {
        {0, 1, 20}, {1, 1, 20}, {2, -1, 24}, {3, -1, 24}, {4, 1, 20}};
    EXPECT_DOUBLE_EQ(next_move_correlation(samples), 1.0);

    for (auto& s : samples) s.imbalance = -s.imbalance;
    EXPECT_DOUBLE_EQ(next_move_correlation(samples), -1.0);
    EXPECT_EQ(next_move_correlation({}), 0.0);
}

}  // namespace
}  // namespace lob::backtest
