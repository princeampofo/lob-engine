# lob-engine

[![CI](https://github.com/princeampofo/lob-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/princeampofo/lob-engine/actions/workflows/ci.yml)

A C++20 limit order book with price-time matching, validated by replaying Nasdaq
order-level data (LOBSTER). It has two implementations behind one interface, a
`std::map` reference book and a price ladder about 4x faster, plus an event-driven
backtester for an order-book-imbalance strategy and Python bindings with an
[analysis notebook](python/analysis.ipynb).

## Results

**Correctness**
- **0 mismatches across 2.11M messages.** Five full days (AAPL, AMZN, GOOG, INTC, MSFT,
  2012-06-21) are replayed, and the book is checked against LOBSTER's top 10 levels after
  every message ([details](#lobster-replay)).
- **1M-operation differential test** against a naive oracle book: trades, return values
  and book state must match after every operation.
- The test suite also runs under ASan and UBSan.
- **0 heap allocations** in the ladder book across 200k add/cancel/match operations,
  counted by hooking `operator new`. The map book makes over 10k.

**Speed**: Apple M4, Apple clang 21, `-O3 -march=native`, median of 5 runs.

| Operation (book of ~10k orders) | Map | Ladder | Speedup |
|---|---:|---:|---:|
| Add | 46.1 ns | 10.6 ns | 4.4x |
| Cancel | 43.4 ns | 10.1 ns | 4.3x |
| Match one resting order | 26.7 ns | 6.7 ns | 4.0x |

| Full-day replay | Messages | Map ns/msg | Ladder ns/msg | Ladder msgs/s | p50 / p99 / p99.9 ns (map → ladder) |
|---|---:|---:|---:|---:|---|
| AAPL | 400k | 58.8 | 15.8 | 63M | 42/125/208 → <42/42/83 |
| AMZN | 270k | 54.9 | 15.0 | 67M | 42/167/291 → <42/42/42 |
| GOOG | 148k | 60.6 | 15.7 | 64M | 42/125/167 → <42/42/83 |
| INTC | 624k | 43.0 | 11.8 | 85M | 42/84/125 → <42/42/42 |
| MSFT | 669k | 45.4 | 11.8 | 84M | 42/84/125 → <42/42/42 |

The percentiles are only good to about ±42 ns, because Apple Silicon's user-space clock
ticks every 41.7 ns. ns/msg is timed over whole days and is precise.

**Why the ladder is faster:** finding a level is one array index instead of a red-black
tree walk (about 8 to 10 dependent loads). Levels near the best price share cache lines,
while map, list and hash nodes are scattered heap allocations. Orders come from a
preallocated pool, so adding one never calls `malloc`.

**Strategy**: imbalance = (bid vol − ask vol) / (bid vol + ask vol) over the top 3 levels.
When |imbalance| ≥ threshold, the strategy crosses the spread to trade 100 shares in that
direction, then exits after a fixed holding time. Orders arrive after 50 µs latency and
pay a $0.003/share fee. The threshold (0.2 to 0.9) and holding time (0.1 to 60 s) are
tuned on the morning (best net P&L with ≥ 20 trades), and the afternoon is reported
out-of-sample.

| Afternoon (test) | Setting | Trades | At mid $ | Net $ | Hit rate | Corr (AM / PM) |
|---|---|---:|---:|---:|---:|---|
| AAPL | 0.9, 60 s | 102 | −271 | −2,246 | 23% | 0.026 / 0.024 |
| AMZN | 0.9, 60 s | 86 | +32 | −1,309 | 12% | 0.069 / 0.057 |
| GOOG | 0.9, 60 s | 74 | +345 | −2,195 | 12% | 0.051 / 0.040 |
| INTC | 0.5, 60 s | 14 | +8.50 | −14.66 | 21% | 0.260 / 0.261 |
| MSFT | 0.7, 10 s | 1 | +2 | +0.40 | 100% | 0.221 / 0.275 |

*At mid* is the P&L at mid prices (the signal before costs). *Corr* is the correlation
between imbalance and the next mid-price change. Morning results and per-trade logs come
from `lob_backtest`.

- **The signal is real and stable.** Correlation with the next mid move is 0.22 to 0.28 on
  the large-tick stocks (INTC, MSFT) and 0.02 to 0.07 on the rest, nearly unchanged from
  morning to afternoon.
- **It doesn't survive costs.** On INTC the signal earned about 1.5¢/share at mid, and
  the one-tick spread plus fees took all of it. On AAPL, AMZN and GOOG, levels often hold
  only a few dozen shares, so 100 shares sweep several levels. All 56 settings lost
  money on AAPL's morning.
- **Tuning picked 60 s holds** because fewer trades means fewer spreads paid, not because
  the signal is better at that horizon.
- **Simplifications:** no market impact (the strategy's orders don't alter the recorded
  book), fixed latency, a flat taker fee with no rebates, and one day per stock. Trading
  this signal profitably would need passive orders that earn the spread, which requires a
  queue-position model.

## Design

One [`OrderBook`](include/lob/order_book.hpp) interface: limit, market, cancel, reduce,
clear level, best bid/ask, top-N depth. Prices and quantities are `int64` ticks, with no
floating point in the book. Every test runs against every implementation (typed tests).

**[Map book](include/lob/map_book.hpp)**: `std::map<price, level>` per side, a
`std::list` FIFO per level, and an `unordered_map` from id to list iterator.

**[Ladder book](include/lob/ladder_book.hpp)**:

```
levels_ (one slot per tick, bids and asks share it)   nodes_ (object pool)
  ... [bid 60][bid 10][  ][ask 200][ask 5] ...        [0] id=17 qty=50 next=4
        ^ best_bid_         ^ best_ask_               [4] id=23 qty=10 prev=0
  each level: head/tail node index + total qty        ids_: flat hash, id -> node
```

- **Price ladder:** `index = (price − base) / tick`. The best bid/ask are tracked as
  indexes, and scans stop at each side's outermost occupied slot.
- **Intrusive FIFO:** prev/next links live in the order node, giving O(1) append and
  O(1) removal from anywhere.
- **Object pool + flat id map:** open addressing with backward-shift deletion and no
  tombstones. Nothing allocates once they are sized.
- Add, cancel and per-fill match are O(1); the map book is O(log L) for add and cancel.

**Trade-off:** the ladder needs a bounded price range (a LOBSTER day fits in ~1,750
one-cent slots). It grows and re-centers when a price falls outside, which is the only
allocation. Finding the next level after the best empties costs the gap in ticks, so a
very sparse book would want an occupancy bitmap.

## LOBSTER replay

Each recorded event is applied directly (no re-matching), handling the format's quirks:

- **Pre-existing orders:** the first snapshot is seeded as anonymous volume per level.
  Cancels or executions of unknown ids draw it down.
- **Hidden executions and cross trades** count as trades without touching the book.
- **Execution direction** is the resting order's side.
- **Placeholder prices** (±9,999,999,999) mark empty levels.
- **Unreported deep levels:** LOBSTER only logs events within the top 10 levels, so a
  level that drifts past the 10th can change unseen. After each message, every level at
  or above the previous row's 10th must match exactly. Levels newly in view are loaded
  from the snapshot and counted. 98% of rows also match all 10 levels outright.

On a mismatch, `lob_replay` prints the message index, the event and both books side by
side.

**Backtester** ([backtest.hpp](include/lob/backtest.hpp)): the strategy is called once
per timestamp after all its messages are applied, so it has no lookahead. Orders queue
with arrival = send + latency and fill against the book at arrival, walking the visible
levels. Many strategies run in one pass, so the 56-setting grid takes one replay per
stock.

## Python

`pip install .` builds the C++ core into a `lob` module (pybind11 + scikit-build-core).
Results come back as NumPy arrays:

```python
import lob
day = lob.Day("data/AAPL_..._message_10.csv", "data/AAPL_..._orderbook_10.csv")
day.replay()                                   # mismatch check, as lob_replay
day.book_history(levels=10, step=1)            # replayed book: time, bid/ask price & size
day.signal(levels=3)                           # imbalance and mid at every update
day.backtest(0.5, holding_s=60, start_s=45_900, end_s=57_600)   # trades + summary
day.backtest_grid([0.3, 0.5, 0.7], [1, 10, 60], end_s=45_900)    # many settings, one pass
```

[python/analysis.ipynb](python/analysis.ipynb) (`pip install ".[notebook]"`) plots the book
over time, imbalance against future moves at several horizons, and where the strategy's
P&L goes. It also shows that the signal fades beyond about 10 to 60 seconds, which is
why long holds don't rescue it.

## Build and run

```
cmake -S . -B build && cmake --build build -j     # Release, -O3 -march=native
./build/lob_tests && ./build/lob_allocation_tests

scripts/download_data.sh                          # LOBSTER samples, ~600 MB
./build/lob_replay data/AAPL_2012-06-21_34200000_57600000_{message,orderbook}_10.csv
./build/lob_backtest                              # writes results/
./build/lob_micro_bench
./build/lob_replay_bench

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON   # sanitizers
```

Requires CMake 3.25+ and a C++20 compiler. GoogleTest and Google Benchmark are fetched at
configure time. Replay tests skip themselves when `data/` is empty.
