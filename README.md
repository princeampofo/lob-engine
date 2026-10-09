# lob-engine

[![CI](https://github.com/princeampofo/lob-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/princeampofo/lob-engine/actions/workflows/ci.yml)

A C++20 limit order book with price-time matching, validated against Nasdaq order-level data
(LOBSTER). It has two implementations behind one interface, a `std::map` reference book and
a price ladder about 4x faster, plus an event-driven backtester and Python bindings with an
[analysis notebook](python/analysis.ipynb).

## Results

**Correctness**
- **0 mismatches across 2.11M messages:** five full days (AAPL, AMZN, GOOG, INTC, MSFT,
  2012-06-21), checked against LOBSTER's top 10 levels after every message
  ([details](#lobster-replay)).
- **1M-operation differential test** against a naive oracle book, plus ASan/UBSan runs.
- **0 heap allocations** in the ladder book over 200k operations (counted by hooking
  `operator new`), against 10k+ for the map book.

**Speed** (Apple M4, Apple clang 21, `-O3 -march=native`, median of 5 runs)

| Operation (~10k-order book) | Map | Ladder | Speedup |
|---|---:|---:|---:|
| Add | 46.1 ns | 10.6 ns | 4.4x |
| Cancel | 43.4 ns | 10.1 ns | 4.3x |
| Match one order | 26.7 ns | 6.7 ns | 4.0x |

| Full-day replay | Messages | Map ns/msg | Ladder ns/msg | Ladder msgs/s | p50/p99/p99.9 ns, map → ladder |
|---|---:|---:|---:|---:|---|
| AAPL | 400k | 58.8 | 15.8 | 63M | 42/125/208 → <42/42/83 |
| AMZN | 270k | 54.9 | 15.0 | 67M | 42/167/291 → <42/42/42 |
| GOOG | 148k | 60.6 | 15.7 | 64M | 42/125/167 → <42/42/83 |
| INTC | 624k | 43.0 | 11.8 | 85M | 42/84/125 → <42/42/42 |
| MSFT | 669k | 45.4 | 11.8 | 84M | 42/84/125 → <42/42/42 |

Percentiles are good to about ±42 ns, the tick of Apple Silicon's clock. ns/msg is timed
over whole days and is precise.

**Why the ladder is faster:** a level lookup is one array index instead of a red-black tree
walk, levels near the best price share cache lines, and orders come from a preallocated
pool instead of scattered heap nodes.

**Strategy:** imbalance = (bid vol − ask vol) / (bid vol + ask vol) over the top 3 levels.
When |imbalance| ≥ threshold, the strategy crosses the spread for 100 shares and exits
after a fixed holding time, with 50 µs latency and a $0.003/share fee. The threshold and
holding time are tuned on the morning (best net P&L, ≥ 20 trades) and tested on the
afternoon:

| Afternoon | Setting | Trades | At mid $ | Net $ | Hit rate | Corr (AM / PM) |
|---|---|---:|---:|---:|---:|---|
| AAPL | 0.9, 60 s | 102 | −271 | −2,246 | 23% | 0.026 / 0.024 |
| AMZN | 0.9, 60 s | 86 | +32 | −1,309 | 12% | 0.069 / 0.057 |
| GOOG | 0.9, 60 s | 74 | +345 | −2,195 | 12% | 0.051 / 0.040 |
| INTC | 0.5, 60 s | 14 | +8.50 | −14.66 | 21% | 0.260 / 0.261 |
| MSFT | 0.7, 10 s | 1 | +2 | +0.40 | 100% | 0.221 / 0.275 |

*At mid* is the P&L at mid prices, i.e. the signal before costs. *Corr* is the correlation
between imbalance and the next mid-price change.

- **The signal is real and stable:** correlation is 0.22 to 0.28 on large-tick INTC and
  MSFT and 0.02 to 0.07 elsewhere, nearly unchanged from morning to afternoon.
- **It doesn't survive costs:** INTC earned about 1.5¢/share at mid, which the one-tick
  spread plus fees erased. On AAPL, AMZN and GOOG, thin levels mean 100 shares sweep the
  book; all 56 settings lost money on AAPL's morning.
- **60 s holds won tuning** because fewer trades pay fewer spreads, not because the signal
  is better at that horizon.
- **Simplifications:** no market impact, fixed latency, flat taker fee, one day per stock.
  Profiting would need passive orders that earn the spread, i.e. a queue-position model.

## Design

One [`OrderBook`](include/lob/order_book.hpp) interface (limit, market, cancel, reduce,
clear level, best bid/ask, depth). Prices and quantities are `int64` ticks, with no floating
point. Typed tests run every test against every implementation.

- **[Map book](include/lob/map_book.hpp):** `std::map` of levels per side, a `std::list`
  FIFO per level, and an `unordered_map` from id to list iterator. Add and cancel are
  O(log L).
- **[Ladder book](include/lob/ladder_book.hpp):** add, cancel and per-fill match are O(1).

```
levels_ (one slot per tick, bids and asks share it)   nodes_ (object pool)
  ... [bid 60][bid 10][  ][ask 200][ask 5] ...        [0] id=17 qty=50 next=4
        ^ best_bid_         ^ best_ask_               [4] id=23 qty=10 prev=0
  each level: head/tail node index + total qty        ids_: flat hash, id -> node
```

- **Price ladder:** `index = (price − base) / tick`, with the best bid/ask tracked as
  indexes.
- **Intrusive FIFO:** prev/next links live in the order node, so removal from anywhere is
  O(1).
- **Object pool + flat id map** (open addressing, backward-shift deletion): nothing
  allocates once they're sized.
- **Trade-off:** it needs a bounded price range (a LOBSTER day fits in ~1,750 one-cent
  slots) and re-centers when a price falls outside, which is its only allocation. After
  the best level empties, finding the next one costs the gap in ticks.

## LOBSTER replay

Events are applied as recorded (no re-matching), handling the format's quirks:

- **Pre-existing orders:** seeded from the first snapshot as anonymous volume per level,
  which cancels and executions of unknown ids then draw down.
- **Hidden executions and cross trades** count as trades but leave the book alone.
- **Execution direction** is the resting order's side, and ±9,999,999,999 prices mark
  empty levels.
- **Unreported deep levels:** LOBSTER only logs events within the top 10 levels, so a level
  beyond the 10th can change unseen. Every level inside the previous row's window must
  match exactly; levels newly in view are loaded from the snapshot and counted. 98% of rows
  match all 10 levels outright.

On a mismatch, `lob_replay` prints the message and both books side by side.

**Backtester** ([backtest.hpp](include/lob/backtest.hpp)): the strategy runs once per
timestamp, after all of that timestamp's messages, so it has no lookahead. Orders fill at
send + latency against the book at that moment, walking the visible levels. All 56 tuning
settings run in a single replay.

## Python

`pip install .` builds a `lob` module (pybind11 + scikit-build-core) that returns NumPy
arrays:

```python
import lob
day = lob.Day("data/AAPL_..._message_10.csv", "data/AAPL_..._orderbook_10.csv")
day.replay()                                   # mismatch check
day.book_history(levels=10)                    # replayed book over time
day.signal()                                   # imbalance and mid at every update
day.backtest(0.5, holding_s=60, start_s=45_900, end_s=57_600)
day.backtest_grid([0.3, 0.5, 0.7], [1, 10, 60], end_s=45_900)   # one pass
```

The [notebook](python/analysis.ipynb) (`pip install ".[notebook]"`) plots the book over
time, imbalance against future moves, and the P&L breakdown. It also shows the signal
fading beyond 10 to 60 s, which is why long holds don't rescue it.

## Build and run

```
cmake -S . -B build && cmake --build build -j     # Release, -O3 -march=native
./build/lob_tests && ./build/lob_allocation_tests

scripts/download_data.sh                          # LOBSTER samples, ~600 MB
./build/lob_replay data/AAPL_2012-06-21_34200000_57600000_{message,orderbook}_10.csv
./build/lob_backtest                              # writes results/
./build/lob_micro_bench && ./build/lob_replay_bench

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON   # sanitizers
```

Requires CMake 3.25+ and a C++20 compiler; GoogleTest and Google Benchmark are fetched
automatically. Replay tests skip when `data/` is empty.
