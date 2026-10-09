#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lob/order_book.hpp"

namespace lob::lobster {

// LOBSTER event types (column 2 of the message file).
enum class EventType : int {
    Submit = 1,           // new limit order
    PartialCancel = 2,    // order reduced by `size`
    Delete = 3,           // order removed
    ExecuteVisible = 4,   // resting visible order executed for `size`
    ExecuteHidden = 5,    // hidden order executed; not in the visible book
    CrossTrade = 6,       // auction cross; not in the visible book
    Halt = 7,             // trading halt / resume indicator
};

// One row of the message file.
struct Message {
    std::int64_t time_ns = 0;  // nanoseconds after midnight
    EventType type = EventType::Submit;
    OrderId id = 0;
    Qty size = 0;
    Price price = 0;  // dollars x 10,000
    Side side = Side::Buy;  // for executions, the side of the *resting* order
};

std::vector<Message> read_messages(const std::string& path);

// Each row of the order book file is the top N levels *after* the message on
// the same row. Placeholder levels (empty) are dropped.
std::vector<Depth> read_snapshots(const std::string& path);

// Applies LOBSTER messages to a book as recorded, without re-matching.
//
// The data starts mid-session, so the book already holds orders that never
// appeared as submissions. `seed` loads each visible level as one anonymous
// order; a later cancel or execution of an unknown order id is taken out of
// that level's anonymous volume instead.
//
// The message file also only reports events inside the top N levels. Levels
// beyond that window can change unseen, so when one comes into view, `reveal`
// replaces it with the snapshot's volume (again as an anonymous order).
class Replayer {
public:
    explicit Replayer(OrderBook& book) : book_(book) {}

    void seed(const Depth& depth);
    void apply(const Message& message);
    void reveal(const Depth& previous, const Depth& current, std::size_t levels);

    std::size_t trades() const { return trades_; }
    std::size_t unknown_ids() const { return unknown_ids_; }
    std::size_t unexplained() const { return unexplained_; }
    std::size_t revealed_levels() const { return revealed_; }

private:
    void remove_unknown(const Message& message);
    void add_anonymous(Side side, const Level& level);
    void reveal_side(Side side, const std::vector<Level>& previous,
                     const std::vector<Level>& current, std::size_t levels);

    OrderBook& book_;
    std::vector<Trade> scratch_;
    std::map<std::pair<Side, Price>, OrderId> anonymous_;  // seeded level -> its order id
    OrderId next_anonymous_id_ = OrderId{1} << 62;  // far above real LOBSTER ids

    std::size_t trades_ = 0;       // executions of any kind (types 4, 5, 6)
    std::size_t unknown_ids_ = 0;  // events taken from anonymous volume
    std::size_t unexplained_ = 0;  // unknown id with no anonymous volume left at its price
    std::size_t revealed_ = 0;     // levels loaded from beyond the reported window
};

struct ReplayResult {
    std::size_t messages = 0;
    std::size_t mismatches = 0;  // rows where a level inside the window differs
    std::size_t exact_rows = 0;  // rows matching all levels before any reload
    std::size_t trades = 0;
    std::size_t unknown_ids = 0;
    std::size_t unexplained = 0;
    std::size_t revealed_levels = 0;
};

// Seeds `book` from the first snapshot, replays every message and checks the
// book after each one. Every level inside the previous row's window must
// match the snapshot exactly; levels newly in view are loaded via `reveal`. The first `max_reports` mismatches are
// printed to `log` with the message and both books.
ReplayResult replay_and_check(OrderBook& book, const std::vector<Message>& messages,
                              const std::vector<Depth>& snapshots, std::ostream& log,
                              std::size_t levels = 10, std::size_t max_reports = 3);

std::string to_string(const Message& message);
std::string format_books(const Depth& ours, const Depth& expected);

}  // namespace lob::lobster
