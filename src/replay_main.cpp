// Replays a LOBSTER sample through a book and checks every message against
// the recorded order book snapshot.
//
//   lob_replay <message.csv> <orderbook.csv>

#include <iostream>

#include "lob/lobster.hpp"
#include "lob/map_book.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <message.csv> <orderbook.csv>\n";
        return 2;
    }
    try {
        const auto messages = lob::lobster::read_messages(argv[1]);
        const auto snapshots = lob::lobster::read_snapshots(argv[2]);

        lob::MapBook book;
        const auto result = lob::lobster::replay_and_check(book, messages, snapshots, std::cout);

        std::cout << "messages:           " << result.messages << "\n"
                  << "mismatched rows:    " << result.mismatches << "\n"
                  << "exact rows:         " << result.exact_rows << "\n"
                  << "trades:             " << result.trades << "\n"
                  << "pre-existing ids:   " << result.unknown_ids << "\n"
                  << "unexplained events: " << result.unexplained << "\n"
                  << "revealed levels:    " << result.revealed_levels << "\n";
        return result.mismatches == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
