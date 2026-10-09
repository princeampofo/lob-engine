#include "lob/lobster.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace lob::lobster {

namespace {

// Empty levels are written as price +/-9999999999 with size 0.
constexpr Price kPlaceholderPrice = 9'999'999'999;

std::ifstream open(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open " + path);
    return file;
}

// Splits a CSV line into fields (LOBSTER files have no quoting).
std::vector<std::string_view> split(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        fields.push_back(line.substr(start, comma - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return fields;
}

std::int64_t parse_int(std::string_view text) {
    std::int64_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("bad integer: '" + std::string(text) + "'");
    }
    return value;
}

// "34200.004241176" -> 34200004241176 ns. Parsed by hand to stay exact.
std::int64_t parse_time_ns(std::string_view text) {
    const std::size_t dot = text.find('.');
    const std::int64_t seconds = parse_int(text.substr(0, dot));
    std::int64_t nanos = 0;
    if (dot != std::string_view::npos) {
        std::string digits(text.substr(dot + 1, 9));
        digits.resize(9, '0');
        nanos = parse_int(digits);
    }
    return seconds * 1'000'000'000 + nanos;
}

bool is_placeholder(Price price, Qty qty) {
    return qty == 0 || price >= kPlaceholderPrice || price <= -kPlaceholderPrice;
}

const char* side_name(Side side) { return side == Side::Buy ? "buy" : "sell"; }

// The message file only reports events inside the top `levels` levels. On a
// side that shows all `levels` levels, the last one is the edge of that
// window; a side showing fewer levels is complete and has no edge.
std::optional<Price> window_edge(const std::vector<Level>& side, std::size_t levels) {
    if (side.size() < levels) return std::nullopt;
    return side.back().price;
}

bool beyond(Side side, Price price, std::optional<Price> edge) {
    if (!edge) return false;
    return side == Side::Buy ? price < *edge : price > *edge;
}

// The levels of `side` that are not beyond `edge`.
std::vector<Level> inside(const std::vector<Level>& levels, Side side, std::optional<Price> edge) {
    std::vector<Level> out;
    for (const Level& level : levels) {
        if (!beyond(side, level.price, edge)) out.push_back(level);
    }
    return out;
}

std::vector<Level> outside(const std::vector<Level>& levels, Side side, std::optional<Price> edge) {
    std::vector<Level> out;
    for (const Level& level : levels) {
        if (beyond(side, level.price, edge)) out.push_back(level);
    }
    return out;
}

// Every level that was inside the previous row's window must match exactly:
// all events that touched it are in the message file.
bool matches_inside_window(const Depth& ours, const Depth& expected, const Depth& previous,
                           std::size_t levels) {
    const auto bid_edge = window_edge(previous.bids, levels);
    const auto ask_edge = window_edge(previous.asks, levels);
    return inside(ours.bids, Side::Buy, bid_edge) == inside(expected.bids, Side::Buy, bid_edge) &&
           inside(ours.asks, Side::Sell, ask_edge) == inside(expected.asks, Side::Sell, ask_edge);
}

}  // namespace

std::vector<Message> read_messages(const std::string& path) {
    std::ifstream file = open(path);
    std::vector<Message> messages;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line == "\r") continue;
        const auto f = split(line);
        if (f.size() < 6) throw std::runtime_error("message row needs 6 columns: " + line);
        Message m;
        m.time_ns = parse_time_ns(f[0]);
        m.type = static_cast<EventType>(parse_int(f[1]));
        m.id = static_cast<OrderId>(parse_int(f[2]));
        m.size = parse_int(f[3]);
        m.price = parse_int(f[4]);
        m.side = parse_int(f[5]) == 1 ? Side::Buy : Side::Sell;
        messages.push_back(m);
    }
    return messages;
}

std::vector<Depth> read_snapshots(const std::string& path) {
    std::ifstream file = open(path);
    std::vector<Depth> snapshots;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line == "\r") continue;
        const auto f = split(line);
        if (f.size() % 4 != 0) throw std::runtime_error("book row needs 4 columns per level: " + line);
        Depth depth;
        for (std::size_t i = 0; i < f.size(); i += 4) {
            const Price ask_price = parse_int(f[i]);
            const Qty ask_qty = parse_int(f[i + 1]);
            const Price bid_price = parse_int(f[i + 2]);
            const Qty bid_qty = parse_int(f[i + 3]);
            if (!is_placeholder(ask_price, ask_qty)) depth.asks.push_back({ask_price, ask_qty});
            if (!is_placeholder(bid_price, bid_qty)) depth.bids.push_back({bid_price, bid_qty});
        }
        snapshots.push_back(std::move(depth));
    }
    return snapshots;
}

void Replayer::add_anonymous(Side side, const Level& level) {
    const OrderId id = next_anonymous_id_++;
    book_.add_limit({id, side, level.price, level.qty}, scratch_);
    scratch_.clear();
    anonymous_[{side, level.price}] = id;
}

void Replayer::seed(const Depth& depth) {
    for (const Level& level : depth.bids) add_anonymous(Side::Buy, level);
    for (const Level& level : depth.asks) add_anonymous(Side::Sell, level);
}

void Replayer::reveal(const Depth& previous, const Depth& current, std::size_t levels) {
    reveal_side(Side::Buy, previous.bids, current.bids, levels);
    reveal_side(Side::Sell, previous.asks, current.asks, levels);
}

// Levels beyond the previous window had no events reported, so whatever the
// book holds there may be stale. When such levels come into view, replace
// them with what the snapshot shows. This is done even when the totals agree,
// because the orders behind them may have changed unseen.
void Replayer::reveal_side(Side side, const std::vector<Level>& previous,
                           const std::vector<Level>& current, std::size_t levels) {
    const auto edge = window_edge(previous, levels);
    if (!edge) return;  // the previous row showed the whole side

    const Depth ours = book_.depth(levels);
    const auto& our_side = side == Side::Buy ? ours.bids : ours.asks;
    const auto shown = outside(current, side, edge);
    if (shown.empty() && outside(our_side, side, edge).empty()) return;

    // Clear our levels from the old edge to the end of the new window (or to
    // the end of the book if the new row shows the whole side).
    const auto new_edge = window_edge(current, levels);
    const Depth all = book_.depth(std::numeric_limits<std::size_t>::max());
    for (const Level& level : side == Side::Buy ? all.bids : all.asks) {
        if (!beyond(side, level.price, edge)) continue;
        if (beyond(side, level.price, new_edge)) break;
        book_.clear_level(side, level.price);
    }
    for (const Level& level : shown) add_anonymous(side, level);
    revealed_ += shown.size();
}

void Replayer::apply(const Message& m) {
    switch (m.type) {
        case EventType::Submit:
            book_.add_limit({m.id, m.side, m.price, m.size}, scratch_);
            scratch_.clear();  // recorded submissions never cross; trades arrive as type 4
            break;
        case EventType::PartialCancel:
            if (!book_.reduce(m.id, m.size)) remove_unknown(m);
            break;
        case EventType::Delete:
            if (!book_.cancel(m.id)) remove_unknown(m);
            break;
        case EventType::ExecuteVisible:
            ++trades_;
            if (!book_.reduce(m.id, m.size)) remove_unknown(m);
            break;
        case EventType::ExecuteHidden:
        case EventType::CrossTrade:
            ++trades_;  // a trade, but the visible book doesn't change
            break;
        case EventType::Halt:
            break;
    }
}

// The order was resting before the data starts: take its size out of the
// anonymous volume seeded at that price.
void Replayer::remove_unknown(const Message& m) {
    auto it = anonymous_.find({m.side, m.price});
    if (it != anonymous_.end() && book_.reduce(it->second, m.size)) {
        ++unknown_ids_;
    } else {
        ++unexplained_;
    }
}

ReplayResult replay_and_check(OrderBook& book, const std::vector<Message>& messages,
                              const std::vector<Depth>& snapshots, std::ostream& log,
                              std::size_t levels, std::size_t max_reports) {
    if (messages.size() != snapshots.size()) {
        throw std::runtime_error("message and order book files have different row counts");
    }
    ReplayResult result;
    if (messages.empty()) return result;

    // Snapshot 0 is the book *after* message 0, so seeding from it already
    // includes message 0. Replay starts at message 1. If message 0 was a
    // submission, its later cancel/execution finds its size in the seed.
    Replayer replayer(book);
    replayer.seed(snapshots[0]);

    result.messages = messages.size();
    for (std::size_t i = 1; i < messages.size(); ++i) {
        replayer.apply(messages[i]);
        const Depth ours = book.depth(levels);
        if (ours == snapshots[i]) ++result.exact_rows;
        if (!matches_inside_window(ours, snapshots[i], snapshots[i - 1], levels)) {
            if (result.mismatches < max_reports) {
                log << "Mismatch at message " << i << ": " << to_string(messages[i]) << "\n"
                    << format_books(ours, snapshots[i]) << "\n";
            }
            ++result.mismatches;
        }
        replayer.reveal(snapshots[i - 1], snapshots[i], levels);
    }
    result.trades = replayer.trades();
    result.unknown_ids = replayer.unknown_ids();
    result.unexplained = replayer.unexplained();
    result.revealed_levels = replayer.revealed_levels();
    return result;
}

std::string to_string(const Message& m) {
    std::ostringstream out;
    out << "time_ns=" << m.time_ns << " type=" << static_cast<int>(m.type) << " id=" << m.id
        << " size=" << m.size << " price=" << m.price << " side=" << side_name(m.side);
    return out.str();
}

// Both books level by level, as "qty@price", ours on the left.
std::string format_books(const Depth& ours, const Depth& expected) {
    auto cell = [](const std::vector<Level>& side, std::size_t i) {
        if (i >= side.size()) return std::string("-");
        return std::to_string(side[i].qty) + "@" + std::to_string(side[i].price);
    };
    const std::size_t rows = std::max({ours.bids.size(), ours.asks.size(), expected.bids.size(),
                                       expected.asks.size()});
    std::ostringstream out;
    out << std::left << "  lvl " << std::setw(18) << "ours bid" << std::setw(18) << "ours ask"
        << std::setw(18) << "file bid" << std::setw(18) << "file ask" << "\n";
    for (std::size_t i = 0; i < rows; ++i) {
        const bool same = cell(ours.bids, i) == cell(expected.bids, i) &&
                          cell(ours.asks, i) == cell(expected.asks, i);
        out << (same ? "  " : "* ") << std::setw(4) << i + 1 << std::setw(18) << cell(ours.bids, i)
            << std::setw(18) << cell(ours.asks, i) << std::setw(18) << cell(expected.bids, i)
            << std::setw(18) << cell(expected.asks, i) << "\n";
    }
    return out.str();
}

}  // namespace lob::lobster
