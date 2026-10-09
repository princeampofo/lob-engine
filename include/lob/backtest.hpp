#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "lob/lobster.hpp"
#include "lob/order_book.hpp"

namespace lob::backtest {

// An aggressive order from a strategy: when it reaches the market it takes
// liquidity at the best opposite prices.
struct StrategyOrder {
    Side side = Side::Buy;
    Qty qty = 0;
};

// What happened to one strategy order. `qty` can be below the requested size,
// even 0, when the visible book is too thin.
struct Fill {
    std::int64_t sent_ns = 0;
    std::int64_t time_ns = 0;  // when it reached the market
    Side side = Side::Buy;
    Qty qty = 0;
    std::int64_t notional = 0;  // sum of price x shares, in price units (dollars x 10,000)
    std::int64_t fee = 0;       // in price units
    Price mid_x2 = 0;           // best bid + best ask at arrival (0 if a side is empty)
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called once per timestamp, after every message at that time has been
    // applied. Orders appended to `orders` reach the market after the latency.
    virtual void on_book_update(std::int64_t time_ns, const OrderBook& book,
                                std::vector<StrategyOrder>& orders) = 0;

    // Called when an order reaches the market, filled or not.
    virtual void on_fill(const Fill& fill) = 0;
};

struct Config {
    std::int64_t latency_ns = 50'000;  // 50 microseconds
    std::int64_t fee_per_share = 30;   // $0.0030, in price units
};

// Replays a LOBSTER day into a fresh book and runs every strategy on it in
// one pass. A strategy order fills against the book as it is when the order
// arrives but does not change it: orders are assumed small enough to have no
// market impact. Returns each strategy's fills, in order.
std::vector<std::vector<Fill>> run(const std::vector<lobster::Message>& messages,
                                   const std::vector<Depth>& snapshots,
                                   const std::vector<Strategy*>& strategies,
                                   const Config& config = {});

// (bid volume - ask volume) / (bid volume + ask volume) over the top `levels`
// levels. 0 when the book is empty.
double imbalance(const OrderBook& book, std::size_t levels);

// One entry and the exit that closed it.
struct RoundTrip {
    std::int64_t entry_ns = 0;
    std::int64_t exit_ns = 0;
    Side side = Side::Buy;  // side of the entry
    Qty qty = 0;
    std::int64_t entry_notional = 0;
    std::int64_t exit_notional = 0;
    std::int64_t fees = 0;
    Price entry_mid_x2 = 0;  // twice the mid price at entry and exit
    Price exit_mid_x2 = 0;

    std::int64_t gross() const {
        return side == Side::Buy ? exit_notional - entry_notional : entry_notional - exit_notional;
    }
    std::int64_t net() const { return gross() - fees; }

    // What the trade would have made entering and exiting at the mid price:
    // the signal's value before paying the spread. Twice the P&L, to stay exact.
    std::int64_t at_mid_x2() const {
        const std::int64_t move = (exit_mid_x2 - entry_mid_x2) * qty;
        return side == Side::Buy ? move : -move;
    }
};

struct ImbalanceParams {
    double threshold = 0.5;                  // enter when |imbalance| reaches this
    std::int64_t holding_ns = 1'000'000'000;  // exit this long after the entry fill
    Qty qty = 100;
    std::size_t levels = 3;
    std::int64_t start_ns = 0;  // only enter within [start_ns, end_ns - holding_ns)
    std::int64_t end_ns = std::numeric_limits<std::int64_t>::max();
};

// Buys at the ask when imbalance >= threshold, sells at the bid when
// imbalance <= -threshold, and exits after a fixed holding time. Holds at
// most one position and one order in flight at a time.
class ImbalanceStrategy final : public Strategy {
public:
    explicit ImbalanceStrategy(const ImbalanceParams& params) : params_(params) {}

    void on_book_update(std::int64_t time_ns, const OrderBook& book,
                        std::vector<StrategyOrder>& orders) override;
    void on_fill(const Fill& fill) override;

    const ImbalanceParams& params() const { return params_; }
    const std::vector<RoundTrip>& round_trips() const { return round_trips_; }

private:
    ImbalanceParams params_;
    bool in_flight_ = false;
    Qty position_ = 0;  // positive long, negative short
    std::int64_t exit_due_ns_ = 0;
    RoundTrip current_;
    std::vector<RoundTrip> round_trips_;
};

struct Summary {
    std::size_t trades = 0;
    std::int64_t at_mid_x2 = 0;  // sum of RoundTrip::at_mid_x2
    std::int64_t gross = 0;      // price units
    std::int64_t fees = 0;
    std::int64_t net = 0;
    double hit_rate = 0;  // share of round trips with net > 0
};

Summary summarize(const std::vector<RoundTrip>& round_trips);

// Imbalance and mid price at one book update.
struct SignalSample {
    std::int64_t time_ns = 0;
    double imbalance = 0;
    Price mid_x2 = 0;  // best bid + best ask, i.e. twice the mid, kept as an integer
};

// Records a SignalSample at every update with both sides present.
class SignalRecorder final : public Strategy {
public:
    explicit SignalRecorder(std::size_t levels = 3) : levels_(levels) {}

    void on_book_update(std::int64_t time_ns, const OrderBook& book,
                        std::vector<StrategyOrder>& orders) override;
    void on_fill(const Fill&) override {}

    const std::vector<SignalSample>& samples() const { return samples_; }

private:
    std::size_t levels_;
    std::vector<SignalSample> samples_;
};

// Pearson correlation between each sample's imbalance and the next change in
// mid price after it. Samples with no later change are skipped.
double next_move_correlation(const std::vector<SignalSample>& samples);

}  // namespace lob::backtest
