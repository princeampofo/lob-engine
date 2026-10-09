// Backtests the order-book-imbalance strategy on each LOBSTER sample day.
//
// The threshold and holding time are chosen on the morning only (best net
// P&L with at least kMinTrades round trips), then that one setting is run on
// the afternoon. Writes trade logs and the morning grid to the results folder.
//
//   lob_backtest [data_dir] [results_dir]

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "lob/backtest.hpp"
#include "lob/lobster.hpp"

namespace {

namespace bt = lob::backtest;
namespace fs = std::filesystem;

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kMorningStart = 34'200 * kSecond;  // 09:30
constexpr std::int64_t kSplit = 45'900 * kSecond;         // 12:45
constexpr std::int64_t kAfternoonEnd = 57'600 * kSecond;  // 16:00
constexpr std::size_t kMinTrades = 20;

const std::vector<double> kThresholds = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
const std::vector<std::int64_t> kHoldings = {kSecond / 10, kSecond / 2, kSecond, 5 * kSecond,
                                             10 * kSecond, 30 * kSecond, 60 * kSecond};

double dollars(std::int64_t price_units) { return static_cast<double>(price_units) / 10'000.0; }
double seconds(std::int64_t ns) { return static_cast<double>(ns) / 1e9; }

std::vector<bt::SignalSample> between(const std::vector<bt::SignalSample>& samples,
                                      std::int64_t start, std::int64_t end) {
    std::vector<bt::SignalSample> out;
    for (const auto& s : samples) {
        if (s.time_ns >= start && s.time_ns < end) out.push_back(s);
    }
    return out;
}

void write_trade_log(const fs::path& path, const std::vector<bt::RoundTrip>& trips) {
    std::ofstream out(path);
    out << "entry_time,exit_time,side,qty,entry_price,exit_price,entry_mid,exit_mid,at_mid,gross,fees,net\n";
    for (const auto& t : trips) {
        char line[256];
        std::snprintf(line, sizeof line, "%.6f,%.6f,%s,%lld,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f\n",
                      seconds(t.entry_ns), seconds(t.exit_ns), t.side == lob::Side::Buy ? "buy" : "sell",
                      static_cast<long long>(t.qty),
                      dollars(t.entry_notional) / static_cast<double>(t.qty),
                      dollars(t.exit_notional) / static_cast<double>(t.qty), dollars(t.entry_mid_x2) / 2,
                      dollars(t.exit_mid_x2) / 2, dollars(t.at_mid_x2()) / 2, dollars(t.gross()),
                      dollars(t.fees), dollars(t.net()));
        out << line;
    }
}

void print_row(const std::string& ticker, const char* half, const bt::ImbalanceParams& p,
               const bt::Summary& s, double correlation) {
    std::printf("| %s | %s | %.1f | %gs | %zu | %.2f | %.2f | %.2f | %.2f | %.0f%% | %.3f |\n",
                ticker.c_str(), half, p.threshold, seconds(p.holding_ns), s.trades,
                dollars(s.at_mid_x2) / 2, dollars(s.gross), dollars(s.fees), dollars(s.net),
                100.0 * s.hit_rate, correlation);
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path data_dir = argc > 1 ? argv[1] : LOB_DATA_DIR;
    const fs::path results_dir = argc > 2 ? argv[2] : LOB_RESULTS_DIR;

    std::vector<fs::path> message_files;
    if (fs::exists(data_dir)) {
        for (const auto& entry : fs::directory_iterator(data_dir)) {
            if (entry.path().filename().string().ends_with("_message_10.csv")) {
                message_files.push_back(entry.path());
            }
        }
    }
    if (message_files.empty()) {
        std::fprintf(stderr, "no *_message_10.csv files in %s (see data/README.md)\n", data_dir.c_str());
        return 1;
    }
    std::sort(message_files.begin(), message_files.end());
    fs::create_directories(results_dir);

    const bt::Config config;
    std::printf("latency %lld us, fee $%.4f/share, 100 shares per trade\n\n",
                static_cast<long long>(config.latency_ns / 1000), dollars(config.fee_per_share));
    std::printf("| Stock | Half | Threshold | Holding | Trades | At mid $ | Gross $ | Fees $ | Net $ | Hit rate | Corr |\n");
    std::printf("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");

    for (const fs::path& path : message_files) {
        const std::string name = path.filename().string();
        const std::string ticker = name.substr(0, name.find('_'));
        std::string book_file = path.string();
        book_file.replace(book_file.rfind("_message_"), 9, "_orderbook_");
        const auto messages = lob::lobster::read_messages(path.string());
        const auto snapshots = lob::lobster::read_snapshots(book_file);

        // Pass 1: every grid setting on the morning, plus the signal all day.
        std::vector<std::unique_ptr<bt::ImbalanceStrategy>> grid;
        std::vector<bt::Strategy*> strategies;
        for (double threshold : kThresholds) {
            for (std::int64_t holding : kHoldings) {
                bt::ImbalanceParams p;
                p.threshold = threshold;
                p.holding_ns = holding;
                p.start_ns = kMorningStart;
                p.end_ns = kSplit;
                grid.push_back(std::make_unique<bt::ImbalanceStrategy>(p));
                strategies.push_back(grid.back().get());
            }
        }
        bt::SignalRecorder recorder;
        strategies.push_back(&recorder);
        bt::run(messages, snapshots, strategies, config);

        std::ofstream grid_out(results_dir / (ticker + "_morning_grid.csv"));
        grid_out << "threshold,holding_s,trades,at_mid,gross,fees,net,hit_rate\n";
        const bt::ImbalanceStrategy* best = nullptr;
        bt::Summary best_summary;
        for (const auto& strategy : grid) {
            const bt::Summary s = bt::summarize(strategy->round_trips());
            grid_out << strategy->params().threshold << "," << seconds(strategy->params().holding_ns) << ","
                     << s.trades << "," << dollars(s.at_mid_x2) / 2 << "," << dollars(s.gross) << "," << dollars(s.fees) << ","
                     << dollars(s.net) << "," << s.hit_rate << "\n";
            if (s.trades >= kMinTrades && (!best || s.net > best_summary.net)) {
                best = strategy.get();
                best_summary = s;
            }
        }
        if (!best) {
            std::printf("| %s | no setting made %zu morning trades |\n", ticker.c_str(), kMinTrades);
            continue;
        }

        // Pass 2: the chosen setting on the afternoon only.
        bt::ImbalanceParams afternoon_params = best->params();
        afternoon_params.start_ns = kSplit;
        afternoon_params.end_ns = kAfternoonEnd;
        bt::ImbalanceStrategy afternoon(afternoon_params);
        bt::run(messages, snapshots, {&afternoon}, config);

        const auto& samples = recorder.samples();
        print_row(ticker, "morning (tuning)", best->params(), best_summary,
                  bt::next_move_correlation(between(samples, kMorningStart, kSplit)));
        print_row(ticker, "afternoon (test)", afternoon_params, bt::summarize(afternoon.round_trips()),
                  bt::next_move_correlation(between(samples, kSplit, kAfternoonEnd)));
        std::fflush(stdout);

        write_trade_log(results_dir / (ticker + "_morning_trades.csv"), best->round_trips());
        write_trade_log(results_dir / (ticker + "_afternoon_trades.csv"), afternoon.round_trips());
    }
    return 0;
}
