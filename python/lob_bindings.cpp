// Python bindings: the two order books, and a LOBSTER day that can be
// replayed, sampled and backtested. Results come back as NumPy arrays in
// seconds and dollars; everything inside stays in integer ns and ticks.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/backtest.hpp"
#include "lob/ladder_book.hpp"
#include "lob/lobster.hpp"
#include "lob/map_book.hpp"

namespace py = pybind11;
namespace bt = lob::backtest;

namespace {

constexpr double kPriceScale = 10'000.0;  // LOBSTER prices are dollars x 10,000
constexpr lob::Price kLobsterTick = 100;   // one cent
constexpr std::size_t kSnapshotLevels = 10;

std::int64_t to_ns(double seconds) { return std::llround(seconds * 1e9); }
double to_seconds(std::int64_t ns) { return static_cast<double>(ns) / 1e9; }
double to_dollars(double price_units) { return price_units / kPriceScale; }

std::unique_ptr<lob::OrderBook> make_book(const std::string& kind) {
    if (kind == "ladder") return std::make_unique<lob::LadderBook>(kLobsterTick);
    if (kind == "map") return std::make_unique<lob::MapBook>();
    throw std::invalid_argument("book must be 'ladder' or 'map'");
}

py::list to_list(const std::vector<lob::Trade>& trades) {
    py::list out;
    for (const auto& t : trades) out.append(py::make_tuple(t.incoming_id, t.resting_id, t.price, t.qty));
    return out;
}

py::list to_list(const std::vector<lob::Level>& levels) {
    py::list out;
    for (const auto& l : levels) out.append(py::make_tuple(l.price, l.qty));
    return out;
}

py::dict to_dict(const bt::Summary& s) {
    py::dict d;
    d["trades"] = s.trades;
    d["at_mid"] = to_dollars(static_cast<double>(s.at_mid_x2) / 2);
    d["gross"] = to_dollars(static_cast<double>(s.gross));
    d["fees"] = to_dollars(static_cast<double>(s.fees));
    d["net"] = to_dollars(static_cast<double>(s.net));
    d["hit_rate"] = s.hit_rate;
    return d;
}

py::dict to_dict(const std::vector<bt::RoundTrip>& trips) {
    const auto n = static_cast<py::ssize_t>(trips.size());
    py::array_t<double> entry_time(n), exit_time(n), entry_price(n), exit_price(n), at_mid(n), gross(n),
        fees(n), net(n);
    py::array_t<std::int64_t> side(n), qty(n);
    for (py::ssize_t i = 0; i < n; ++i) {
        const bt::RoundTrip& t = trips[static_cast<std::size_t>(i)];
        const double shares = static_cast<double>(t.qty);
        entry_time.mutable_at(i) = to_seconds(t.entry_ns);
        exit_time.mutable_at(i) = to_seconds(t.exit_ns);
        side.mutable_at(i) = t.side == lob::Side::Buy ? 1 : -1;
        qty.mutable_at(i) = t.qty;
        entry_price.mutable_at(i) = to_dollars(static_cast<double>(t.entry_notional)) / shares;
        exit_price.mutable_at(i) = to_dollars(static_cast<double>(t.exit_notional)) / shares;
        at_mid.mutable_at(i) = to_dollars(static_cast<double>(t.at_mid_x2()) / 2);
        gross.mutable_at(i) = to_dollars(static_cast<double>(t.gross()));
        fees.mutable_at(i) = to_dollars(static_cast<double>(t.fees));
        net.mutable_at(i) = to_dollars(static_cast<double>(t.net()));
    }
    py::dict d;
    d["entry_time"] = entry_time;
    d["exit_time"] = exit_time;
    d["side"] = side;
    d["qty"] = qty;
    d["entry_price"] = entry_price;
    d["exit_price"] = exit_price;
    d["at_mid"] = at_mid;
    d["gross"] = gross;
    d["fees"] = fees;
    d["net"] = net;
    return d;
}

bt::Config make_config(double latency_us, double fee_per_share) {
    bt::Config config;
    config.latency_ns = std::llround(latency_us * 1'000);
    config.fee_per_share = std::llround(fee_per_share * kPriceScale);
    return config;
}

bt::ImbalanceParams make_params(double threshold, double holding_s, double start_s, double end_s,
                                lob::Qty qty) {
    bt::ImbalanceParams p;
    p.threshold = threshold;
    p.holding_ns = to_ns(holding_s);
    p.start_ns = to_ns(start_s);
    p.end_ns = to_ns(end_s);
    p.qty = qty;
    return p;
}

// One LOBSTER day (message file + order book file), loaded once.
class Day {
public:
    Day(const std::string& message_file, const std::string& orderbook_file)
        : messages_(lob::lobster::read_messages(message_file)),
          snapshots_(lob::lobster::read_snapshots(orderbook_file)) {
        if (messages_.empty() || messages_.size() != snapshots_.size()) {
            throw std::runtime_error("message and order book files must be non-empty with equal row counts");
        }
    }

    std::size_t size() const { return messages_.size(); }

    py::dict replay(const std::string& book_kind, std::size_t levels) const {
        auto book = make_book(book_kind);
        std::ostringstream log;
        const auto r = lob::lobster::replay_and_check(*book, messages_, snapshots_, log, levels);
        py::dict d;
        d["messages"] = r.messages;
        d["mismatches"] = r.mismatches;
        d["exact_rows"] = r.exact_rows;
        d["trades"] = r.trades;
        d["unknown_ids"] = r.unknown_ids;
        d["unexplained"] = r.unexplained;
        d["revealed_levels"] = r.revealed_levels;
        d["report"] = log.str();
        return d;
    }

    // The replayed book's top `levels` levels after every `step`-th message.
    // Empty levels have price NaN and size 0.
    py::dict book_history(std::size_t levels, std::size_t step) const {
        if (levels == 0 || step == 0) throw std::invalid_argument("levels and step must be positive");
        const std::size_t rows = (messages_.size() - 1) / step + 1;
        const auto shape = std::vector<py::ssize_t>{static_cast<py::ssize_t>(rows), static_cast<py::ssize_t>(levels)};
        py::array_t<double> time(static_cast<py::ssize_t>(rows));
        py::array_t<double> bid_price(shape), bid_size(shape), ask_price(shape), ask_size(shape);
        auto t = time.mutable_unchecked<1>();
        auto bp = bid_price.mutable_unchecked<2>();
        auto bs = bid_size.mutable_unchecked<2>();
        auto ap = ask_price.mutable_unchecked<2>();
        auto as = ask_size.mutable_unchecked<2>();

        lob::LadderBook book(kLobsterTick);
        lob::lobster::Replayer replayer(book);
        replayer.seed(snapshots_[0]);  // the state after message 0
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (std::size_t i = 0, row = 0; i < messages_.size(); ++i) {
            if (i > 0) {
                replayer.apply(messages_[i]);
                replayer.reveal(snapshots_[i - 1], snapshots_[i], kSnapshotLevels);
            }
            if (i % step != 0) continue;

            const lob::Depth depth = book.depth(levels);
            const auto r = static_cast<py::ssize_t>(row++);
            t(r) = to_seconds(messages_[i].time_ns);
            for (std::size_t k = 0; k < levels; ++k) {
                const auto c = static_cast<py::ssize_t>(k);
                const bool has_bid = k < depth.bids.size();
                const bool has_ask = k < depth.asks.size();
                bp(r, c) = has_bid ? to_dollars(static_cast<double>(depth.bids[k].price)) : nan;
                bs(r, c) = has_bid ? static_cast<double>(depth.bids[k].qty) : 0;
                ap(r, c) = has_ask ? to_dollars(static_cast<double>(depth.asks[k].price)) : nan;
                as(r, c) = has_ask ? static_cast<double>(depth.asks[k].qty) : 0;
            }
        }
        py::dict d;
        d["time"] = time;
        d["bid_price"] = bid_price;
        d["bid_size"] = bid_size;
        d["ask_price"] = ask_price;
        d["ask_size"] = ask_size;
        return d;
    }

    // Imbalance and mid price at every book update (once per timestamp).
    py::dict signal(std::size_t levels) const {
        bt::SignalRecorder recorder(levels);
        bt::run(messages_, snapshots_, {&recorder});
        const auto& samples = recorder.samples();
        const auto n = static_cast<py::ssize_t>(samples.size());
        py::array_t<double> time(n), imbalance(n), mid(n);
        for (py::ssize_t i = 0; i < n; ++i) {
            const auto& s = samples[static_cast<std::size_t>(i)];
            time.mutable_at(i) = to_seconds(s.time_ns);
            imbalance.mutable_at(i) = s.imbalance;
            mid.mutable_at(i) = to_dollars(static_cast<double>(s.mid_x2) / 2);
        }
        py::dict d;
        d["time"] = time;
        d["imbalance"] = imbalance;
        d["mid"] = mid;
        return d;
    }

    py::dict backtest(double threshold, double holding_s, double start_s, double end_s, lob::Qty qty,
                      double latency_us, double fee_per_share) const {
        bt::ImbalanceStrategy strategy(make_params(threshold, holding_s, start_s, end_s, qty));
        bt::run(messages_, snapshots_, {&strategy}, make_config(latency_us, fee_per_share));
        py::dict d;
        d["trades"] = to_dict(strategy.round_trips());
        d["summary"] = to_dict(bt::summarize(strategy.round_trips()));
        return d;
    }

    // Every threshold x holding combination, in one replay.
    py::list backtest_grid(const std::vector<double>& thresholds, const std::vector<double>& holdings_s,
                           double start_s, double end_s, lob::Qty qty, double latency_us,
                           double fee_per_share) const {
        std::vector<std::unique_ptr<bt::ImbalanceStrategy>> grid;
        std::vector<bt::Strategy*> strategies;
        for (double threshold : thresholds) {
            for (double holding : holdings_s) {
                grid.push_back(std::make_unique<bt::ImbalanceStrategy>(
                    make_params(threshold, holding, start_s, end_s, qty)));
                strategies.push_back(grid.back().get());
            }
        }
        bt::run(messages_, snapshots_, strategies, make_config(latency_us, fee_per_share));

        py::list out;
        for (const auto& strategy : grid) {
            py::dict d = to_dict(bt::summarize(strategy->round_trips()));
            d["threshold"] = strategy->params().threshold;
            d["holding_s"] = to_seconds(strategy->params().holding_ns);
            out.append(d);
        }
        return out;
    }

private:
    std::vector<lob::lobster::Message> messages_;
    std::vector<lob::Depth> snapshots_;
};

}  // namespace

PYBIND11_MODULE(_lob, m) {
    m.doc() = "Limit order book, LOBSTER replay and backtester";

    py::enum_<lob::Side>(m, "Side").value("BUY", lob::Side::Buy).value("SELL", lob::Side::Sell);

    // Prices here are integer ticks, as in C++. Trades are
    // (incoming_id, resting_id, price, qty); depth is (bids, asks) of (price, qty).
    py::class_<lob::OrderBook>(m, "OrderBook")
        .def(
            "add_limit",
            [](lob::OrderBook& book, lob::OrderId id, lob::Side side, lob::Price price, lob::Qty qty) {
                if (qty <= 0) throw std::invalid_argument("qty must be positive");
                if (book.contains(id)) throw std::invalid_argument("an order with this id is already resting");
                std::vector<lob::Trade> trades;
                book.add_limit({id, side, price, qty}, trades);
                return to_list(trades);
            },
            py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"))
        .def(
            "add_market",
            [](lob::OrderBook& book, lob::OrderId id, lob::Side side, lob::Qty qty) {
                if (qty <= 0) throw std::invalid_argument("qty must be positive");
                std::vector<lob::Trade> trades;
                book.add_market(id, side, qty, trades);
                return to_list(trades);
            },
            py::arg("id"), py::arg("side"), py::arg("qty"))
        .def("cancel", &lob::OrderBook::cancel, py::arg("id"))
        .def("contains", &lob::OrderBook::contains, py::arg("id"))
        .def("reduce", &lob::OrderBook::reduce, py::arg("id"), py::arg("qty"))
        .def("best_bid", &lob::OrderBook::best_bid)
        .def("best_ask", &lob::OrderBook::best_ask)
        .def(
            "depth",
            [](const lob::OrderBook& book, std::size_t levels) {
                const lob::Depth d = book.depth(levels);
                return py::make_tuple(to_list(d.bids), to_list(d.asks));
            },
            py::arg("levels") = 10);

    py::class_<lob::MapBook, lob::OrderBook>(m, "MapBook").def(py::init<>());
    py::class_<lob::LadderBook, lob::OrderBook>(m, "LadderBook")
        .def(py::init<lob::Price, std::size_t>(), py::arg("tick_size") = 1,
             py::arg("expected_orders") = 1 << 16);

    py::class_<Day>(m, "Day", "One LOBSTER day: a message file and its order book file.")
        .def(py::init<const std::string&, const std::string&>(), py::arg("message_file"),
             py::arg("orderbook_file"))
        .def("__len__", &Day::size)
        .def("replay", &Day::replay, py::arg("book") = "ladder", py::arg("levels") = 10,
             "Replay the day and check every message against the snapshots.")
        .def("book_history", &Day::book_history, py::arg("levels") = 5, py::arg("step") = 1,
             "Top levels of the replayed book after every step-th message.")
        .def("signal", &Day::signal, py::arg("levels") = 3,
             "Order-book imbalance and mid price at every book update.")
        .def("backtest", &Day::backtest, py::arg("threshold"), py::arg("holding_s"),
             py::arg("start_s") = 0.0, py::arg("end_s") = 86'400.0, py::arg("qty") = 100,
             py::arg("latency_us") = 50.0, py::arg("fee_per_share") = 0.003,
             "Run the imbalance strategy; returns its trades and a summary.")
        .def("backtest_grid", &Day::backtest_grid, py::arg("thresholds"), py::arg("holdings_s"),
             py::arg("start_s") = 0.0, py::arg("end_s") = 86'400.0, py::arg("qty") = 100,
             py::arg("latency_us") = 50.0, py::arg("fee_per_share") = 0.003,
             "Run every threshold x holding combination in one replay; returns summaries.");
}
