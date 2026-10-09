// Replays a LOBSTER sample through a book and checks every message against
// the recorded order book snapshot.
//
//   lob_replay <message.csv> <orderbook.csv> [ladder|map]

#include <iostream>
#include <memory>
#include <string>

#include "lob/ladder_book.hpp"
#include "lob/lobster.hpp"
#include "lob/map_book.hpp"

int main(int argc, char** argv) {
    const std::string kind = argc == 4 ? argv[3] : "ladder";
    if ((argc != 3 && argc != 4) || (kind != "ladder" && kind != "map")) {
        std::cerr << "usage: " << argv[0] << " <message.csv> <orderbook.csv> [ladder|map]\n";
        return 2;
    }
    try {
        const auto messages = lob::lobster::read_messages(argv[1]);
        const auto snapshots = lob::lobster::read_snapshots(argv[2]);

        // LOBSTER prices are dollars x 10,000 and move in whole cents.
        std::unique_ptr<lob::OrderBook> book;
        if (kind == "ladder") {
            book = std::make_unique<lob::LadderBook>(100);
        } else {
            book = std::make_unique<lob::MapBook>();
        }
        const auto result = lob::lobster::replay_and_check(*book, messages, snapshots, std::cout);

        std::cout << "book:               " << kind << "\n"
                  << "messages:           " << result.messages << "\n"
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
