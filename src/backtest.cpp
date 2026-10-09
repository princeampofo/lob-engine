#include "lob/backtest.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <stdexcept>

#include "lob/ladder_book.hpp"

namespace lob::backtest {

namespace {

constexpr std::size_t kSnapshotLevels = 10;
constexpr Price kLobsterTick = 100;  // one cent in LOBSTER price units

struct PendingOrder {
    std::int64_t sent_ns;
    std::int64_t arrival_ns;
    std::size_t strategy;
    StrategyOrder order;
};

Qty signed_qty(Side side, Qty qty) { return side == Side::Buy ? qty : -qty; }

// Takes liquidity from the visible levels of the opposite side without
// changing the book.
Fill execute(const PendingOrder& pending, const OrderBook& book, const Config& config) {
    Fill fill{pending.sent_ns, pending.arrival_ns, pending.order.side, 0, 0, 0};
    const Depth depth = book.depth(kSnapshotLevels);
    const auto& levels = pending.order.side == Side::Buy ? depth.asks : depth.bids;
    for (const Level& level : levels) {
        const Qty take = std::min(pending.order.qty - fill.qty, level.qty);
        fill.qty += take;
        fill.notional += take * level.price;
        if (fill.qty == pending.order.qty) break;
    }
    fill.fee = fill.qty * config.fee_per_share;
    if (!depth.bids.empty() && !depth.asks.empty()) {
        fill.mid_x2 = depth.bids[0].price + depth.asks[0].price;
    }
    return fill;
}

}  // namespace

std::vector<std::vector<Fill>> run(const std::vector<lobster::Message>& messages,
                                   const std::vector<Depth>& snapshots,
                                   const std::vector<Strategy*>& strategies,
                                   const Config& config) {
    if (messages.size() != snapshots.size()) {
        throw std::runtime_error("message and order book files have different row counts");
    }
    std::vector<std::vector<Fill>> fills(strategies.size());
    if (messages.empty()) return fills;

    LadderBook book(kLobsterTick);
    lobster::Replayer replayer(book);
    replayer.seed(snapshots[0]);  // snapshot 0 already includes message 0

    // The latency is the same for every order, so arrivals stay in send order.
    std::deque<PendingOrder> pending;
    std::vector<StrategyOrder> orders;
    auto deliver = [&](const PendingOrder& p) {
        const Fill fill = execute(p, book, config);
        fills[p.strategy].push_back(fill);
        strategies[p.strategy]->on_fill(fill);
    };

    for (std::size_t i = 1; i < messages.size(); ++i) {
        const lobster::Message& message = messages[i];

        // Orders that arrive before this message see the book as it is now.
        while (!pending.empty() && pending.front().arrival_ns < message.time_ns) {
            deliver(pending.front());
            pending.pop_front();
        }

        replayer.apply(message);
        replayer.reveal(snapshots[i - 1], snapshots[i], kSnapshotLevels);

        // Strategies see the book once all messages at this timestamp are in.
        const bool last_at_this_time = i + 1 == messages.size() || messages[i + 1].time_ns != message.time_ns;
        if (!last_at_this_time) continue;

        for (std::size_t s = 0; s < strategies.size(); ++s) {
            orders.clear();
            strategies[s]->on_book_update(message.time_ns, book, orders);
            for (const StrategyOrder& order : orders) {
                pending.push_back({message.time_ns, message.time_ns + config.latency_ns, s, order});
            }
        }
    }

    // Orders still in flight at the end of the day meet the final book.
    for (const PendingOrder& p : pending) deliver(p);
    return fills;
}

double imbalance(const OrderBook& book, std::size_t levels) {
    const Depth depth = book.depth(levels);
    Qty bids = 0;
    Qty asks = 0;
    for (const Level& level : depth.bids) bids += level.qty;
    for (const Level& level : depth.asks) asks += level.qty;
    if (bids + asks == 0) return 0;
    return static_cast<double>(bids - asks) / static_cast<double>(bids + asks);
}

void ImbalanceStrategy::on_book_update(std::int64_t time_ns, const OrderBook& book,
                                       std::vector<StrategyOrder>& orders) {
    if (in_flight_) return;

    if (position_ != 0) {
        if (time_ns >= exit_due_ns_) {
            orders.push_back({position_ > 0 ? Side::Sell : Side::Buy, std::abs(position_)});
            in_flight_ = true;
        }
        return;
    }

    if (time_ns < params_.start_ns || time_ns + params_.holding_ns >= params_.end_ns) return;
    const double signal = imbalance(book, params_.levels);
    if (signal >= params_.threshold) {
        orders.push_back({Side::Buy, params_.qty});
    } else if (signal <= -params_.threshold) {
        orders.push_back({Side::Sell, params_.qty});
    } else {
        return;
    }
    in_flight_ = true;
}

void ImbalanceStrategy::on_fill(const Fill& fill) {
    in_flight_ = false;
    if (fill.qty == 0) return;

    if (position_ == 0) {  // entry
        current_ = {fill.time_ns, 0, fill.side, fill.qty, fill.notional, 0, fill.fee, fill.mid_x2, 0};
        position_ = signed_qty(fill.side, fill.qty);
        exit_due_ns_ = fill.time_ns + params_.holding_ns;
        return;
    }

    // Exit, possibly in several pieces if the book was thin.
    current_.exit_ns = fill.time_ns;
    current_.exit_notional += fill.notional;
    current_.fees += fill.fee;
    current_.exit_mid_x2 = fill.mid_x2;
    position_ += signed_qty(fill.side, fill.qty);
    if (position_ == 0) round_trips_.push_back(current_);
}

Summary summarize(const std::vector<RoundTrip>& round_trips) {
    Summary summary;
    std::size_t winners = 0;
    for (const RoundTrip& trip : round_trips) {
        ++summary.trades;
        summary.at_mid_x2 += trip.at_mid_x2();
        summary.gross += trip.gross();
        summary.fees += trip.fees;
        if (trip.net() > 0) ++winners;
    }
    summary.net = summary.gross - summary.fees;
    if (summary.trades > 0) {
        summary.hit_rate = static_cast<double>(winners) / static_cast<double>(summary.trades);
    }
    return summary;
}

void SignalRecorder::on_book_update(std::int64_t time_ns, const OrderBook& book,
                                    std::vector<StrategyOrder>&) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (!bid || !ask) return;
    samples_.push_back({time_ns, imbalance(book, levels_), *bid + *ask});
}

double next_move_correlation(const std::vector<SignalSample>& samples) {
    // next_mid[i]: the first mid after sample i that differs from sample i's.
    // Walking backwards: either sample i+1 already differs, or sample i+1 has
    // the same mid and so the same answer.
    const std::size_t count = samples.size();
    std::vector<std::optional<Price>> next_mid(count);
    for (std::size_t i = count; i-- > 1;) {
        const Price later = samples[i].mid_x2;
        next_mid[i - 1] = later != samples[i - 1].mid_x2 ? std::optional<Price>(later) : next_mid[i];
    }

    std::vector<double> xs;
    std::vector<double> ys;
    for (std::size_t i = 0; i < count; ++i) {
        if (!next_mid[i]) continue;
        xs.push_back(samples[i].imbalance);
        ys.push_back(static_cast<double>(*next_mid[i] - samples[i].mid_x2));
    }

    const double n = static_cast<double>(xs.size());
    if (n < 2) return 0;
    double mean_x = 0;
    double mean_y = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        mean_x += xs[i];
        mean_y += ys[i];
    }
    mean_x /= n;
    mean_y /= n;
    double cov = 0;
    double var_x = 0;
    double var_y = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        cov += (xs[i] - mean_x) * (ys[i] - mean_y);
        var_x += (xs[i] - mean_x) * (xs[i] - mean_x);
        var_y += (ys[i] - mean_y) * (ys[i] - mean_y);
    }
    if (var_x == 0 || var_y == 0) return 0;
    return cov / std::sqrt(var_x * var_y);
}

}  // namespace lob::backtest
