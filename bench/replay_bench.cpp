// Full-day replay benchmark. For every LOBSTER sample in the data folder and
// each book, it replays the day once to warm up, then:
//   - throughput: replays it 5 more times untimed per message, reports the
//     median messages per second
//   - latency: replays it 5 more times timing every message, reports
//     p50 / p99 / p99.9 over all of them
// Only applying messages is timed; loading files and seeding the book are not.
//
// Each latency reading is limited by the clock's resolution (printed first;
// about 42 ns on Apple Silicon). A reading below one clock step prints as
// "<N". The ns/message column comes from whole-day timings and is precise.
//
//   lob_replay_bench [data_dir]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lob/ladder_book.hpp"
#include "lob/lobster.hpp"
#include "lob/map_book.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using lob::lobster::Message;

constexpr int kRuns = 5;

std::unique_ptr<lob::OrderBook> make_book(const std::string& kind) {
    if (kind == "ladder") return std::make_unique<lob::LadderBook>(100);  // whole cents
    return std::make_unique<lob::MapBook>();
}

// Replays one day on a fresh book. If `latencies` is given, every message is
// timed individually and appended to it. Returns the total time in seconds.
double replay(const std::string& kind, const std::vector<Message>& messages,
              const lob::Depth& first_snapshot, std::vector<std::int64_t>* latencies) {
    auto book = make_book(kind);
    lob::lobster::Replayer replayer(*book);
    replayer.seed(first_snapshot);

    const auto start = Clock::now();
    for (std::size_t i = 1; i < messages.size(); ++i) {
        if (latencies) {
            const auto t0 = Clock::now();
            replayer.apply(messages[i]);
            const auto t1 = Clock::now();
            latencies->push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        } else {
            replayer.apply(messages[i]);
        }
    }
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::int64_t percentile(std::vector<std::int64_t>& values, double p) {
    const std::size_t k = static_cast<std::size_t>(p / 100.0 * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

std::string format_latency(std::int64_t ns, std::int64_t resolution) {
    if (ns < resolution) return "<" + std::to_string(resolution);
    return std::to_string(ns);
}

// Smallest non-zero step of the clock, which limits latency resolution.
std::int64_t clock_resolution_ns() {
    std::int64_t best = INT64_MAX;
    for (int i = 0; i < 1000; ++i) {
        const auto t0 = Clock::now();
        auto t1 = Clock::now();
        while (t1 == t0) t1 = Clock::now();
        best = std::min<std::int64_t>(best, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path data_dir = argc > 1 ? argv[1] : LOB_DATA_DIR;
    std::vector<std::filesystem::path> message_files;
    if (std::filesystem::exists(data_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
            if (entry.path().filename().string().ends_with("_message_10.csv")) {
                message_files.push_back(entry.path());
            }
        }
    }
    if (message_files.empty()) {
        std::fprintf(stderr, "no *_message_10.csv files in %s (see data/README.md)\n",
                     data_dir.c_str());
        return 1;
    }
    std::sort(message_files.begin(), message_files.end());

    const std::int64_t resolution = clock_resolution_ns();
    std::printf("clock resolution: %lld ns\n\n", static_cast<long long>(resolution));
    std::printf("| Stock | Book | Messages | Messages/s | ns/message | p50 ns | p99 ns | p99.9 ns |\n");
    std::printf("|---|---|---:|---:|---:|---:|---:|---:|\n");

    for (const auto& path : message_files) {
        std::string book_file = path.string();
        book_file.replace(book_file.rfind("_message_"), 9, "_orderbook_");
        const auto messages = lob::lobster::read_messages(path.string());
        const auto snapshots = lob::lobster::read_snapshots(book_file);
        const std::string ticker = path.filename().string().substr(0, path.filename().string().find('_'));
        const double count = static_cast<double>(messages.size() - 1);

        for (const std::string kind : {"map", "ladder"}) {
            replay(kind, messages, snapshots[0], nullptr);  // warm-up

            std::vector<double> seconds;
            for (int run = 0; run < kRuns; ++run) {
                seconds.push_back(replay(kind, messages, snapshots[0], nullptr));
            }
            std::sort(seconds.begin(), seconds.end());
            const double median = seconds[kRuns / 2];

            std::vector<std::int64_t> latencies;
            latencies.reserve(messages.size() * kRuns);
            for (int run = 0; run < kRuns; ++run) replay(kind, messages, snapshots[0], &latencies);

            std::printf("| %s | %s | %.0f | %.1fM | %.1f | %s | %s | %s |\n", ticker.c_str(),
                        kind.c_str(), count, count / median / 1e6, median / count * 1e9,
                        format_latency(percentile(latencies, 50.0), resolution).c_str(),
                        format_latency(percentile(latencies, 99.0), resolution).c_str(),
                        format_latency(percentile(latencies, 99.9), resolution).c_str());
            std::fflush(stdout);
        }
    }
    return 0;
}
