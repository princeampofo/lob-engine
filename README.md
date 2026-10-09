# lob-engine

A limit order book and matching engine in C++20. It matches limit and market orders by
price-time priority, rebuilds the book from real Nasdaq order-level data (LOBSTER), and
comes in two implementations behind one interface: a simple `std::map` reference book and
a cache-friendly price ladder that is about 4x faster.

## Results

### Correctness

- **Nasdaq replay: 0 mismatches across 2,110,860 messages.** Five full trading days
  (AAPL, AMZN, GOOG, INTC, MSFT on 2012-06-21) are replayed message by message, and the
  book is checked against LOBSTER's recorded top 10 levels after every message. Both
  books pass. [How the check works](#replaying-lobster-data).
- **Differential test: 1,000,000 random operations** (limit, market, cancel, reduce, mass
  cancel) applied to each book and to a deliberately naive oracle, with identical trades,
  return values and book state required after every operation.
- **Sanitizers:** the full test suite also runs under AddressSanitizer and
  UndefinedBehaviorSanitizer.
- **No allocation while trading:** a test counts every call to `operator new` across
  200,000 adds, cancels, reductions and crossing orders. The ladder book makes 0; the map
  book makes over 10,000.

### Speed

Measured on an Apple M4 (10 cores, 24 GB), macOS 26.6, Apple clang 21.0.0, Release build
with `-O3 -march=native -DNDEBUG`. The machine was in normal desktop use (load average
about 2.6), not fully idle. Numbers from different machines are not comparable.

**Microbenchmarks** (Google Benchmark, median of 5 repetitions, book holding ~10,000
orders):

| Operation | Map book | Ladder book | Speedup |
|---|---:|---:|---:|
| Add a resting limit order | 46.1 ns | 10.6 ns | 4.4x |
| Cancel an order (random order) | 43.4 ns | 10.1 ns | 4.3x |
| Match a market order against one resting order | 26.7 ns | 6.7 ns | 4.0x |

**Full-day replay** (warm-up pass, then median of 5 passes; per-message latency over 5
more passes):

| Stock | Messages | Map: msgs/s | Ladder: msgs/s | Map ns/msg | Ladder ns/msg | Map p50 / p99 / p99.9 | Ladder p50 / p99 / p99.9 |
|---|---:|---:|---:|---:|---:|---|---|
| AAPL | 400,390 | 17.0M | 63.2M | 58.8 | 15.8 | 42 / 125 / 208 | <42 / 42 / 83 |
| AMZN | 269,747 | 18.2M | 66.9M | 54.9 | 15.0 | 42 / 167 / 291 | <42 / 42 / 42 |
| GOOG | 147,915 | 16.5M | 63.9M | 60.6 | 15.7 | 42 / 125 / 167 | <42 / 42 / 83 |
| INTC | 624,039 | 23.3M | 84.8M | 43.0 | 11.8 | 42 / 84 / 125 | <42 / 42 / 42 |
| MSFT | 668,764 | 22.0M | 84.4M | 45.4 | 11.8 | 42 / 84 / 125 | <42 / 42 / 42 |

Latency percentiles are in nanoseconds. On Apple Silicon the system clock ticks every
41.7 ns and no finer timer is available to user programs, so each reading is only good
to about ±42 ns, and "<42" means under one tick. The ns/msg column comes from timing whole
days and is precise.

### Why the ladder is faster

- **One array lookup per price instead of a tree walk.** The map book finds a level by
  walking a red-black tree, about 8 to 10 dependent pointer loads for a few hundred
  levels.
  The ladder computes `(price - base) / tick` and indexes an array.
- **Contiguous memory.** Ladder levels sit next to each other, so the levels near the
  best price share cache lines. Map nodes, list nodes and hash-map nodes are separate
  heap allocations scattered through memory.
- **No allocation per order.** Orders come from a preallocated pool and the order-id map
  is a flat array, so adding an order is a few writes into memory that is already in
  cache. The map book allocates a list node and a hash-map node per order, plus a
  tree node for each new price level.

## Design

Both books implement one interface, [`OrderBook`](include/lob/order_book.hpp): add a limit
order, add a market order, cancel, reduce quantity, mass-cancel a level, best bid and
ask, and a top-N depth snapshot. Prices and quantities are 64-bit integers; nothing in
the book uses floating point. Trades go into a caller-owned vector, so the caller can
reuse it.

**Reference book** ([map_book.hpp](include/lob/map_book.hpp)): a `std::map` per side from
price to level, each level a `std::list` of orders, plus a `std::unordered_map` from order
id to the order's place in its list.

**Ladder book** ([ladder_book.hpp](include/lob/ladder_book.hpp)):

```
 levels_ (one slot per tick)                 nodes_ (object pool)
 index:  ...  97   98   99  100  101 ...     [0] id=17 qty=50  prev=-  next=4
            [bid][bid][   ][ask][ask]        [4] id=23 qty=10  prev=0  next=-
               |              |              [9] id=31 qty=200 prev=-  next=-
               v              v
         head=0, tail=4    head=9, tail=9     ids_ (flat hash map)
         total=60          total=200          17 -> 0, 23 -> 4, 31 -> 9
            ^                 ^
        best_bid_         best_ask_
```

- **Price ladder:** an array with one level per tick from a base price. Bids and asks
  share it, since every bid is below every ask. The best bid and ask are tracked as
  indexes. A price outside the array makes it grow and re-center (the only time it
  allocates).
- **Intrusive FIFO per level:** each order holds the indexes of its neighbours, so
  orders append at the tail and are removed from anywhere in O(1).
- **Object pool:** all orders live in one preallocated array, handed out and returned
  through a free list.
- **Flat order-id map:** open addressing with linear probing and backward-shift deletion,
  in one array, kept at most half full.

| Operation | Map book | Ladder book |
|---|---|---|
| Add (resting) | O(log L) | O(1) |
| Cancel by id | O(log L) | O(1) |
| Match, per fill | O(1) | O(1) |
| Best bid / ask | O(1) | O(1) |

L is the number of price levels. When the ladder empties the best level, it scans to
the next occupied slot, which costs the gap in ticks. Real books are dense near the
best price, so the gap is usually a tick or two.

**The ladder's trade-off:** it needs a bounded price range and spends memory on empty
ticks. Here it grows and re-centers when a price falls outside, which is rare and cheap
for a stock that moves a few percent a day (a full LOBSTER day fits in about 1,750
one-cent slots). A lone order far from the best price also makes the next-level scan
long once the levels in between empty. A bitmap of occupied slots would let the scan
check 64 slots per instruction, if profiling ever shows it matters.

## Replaying LOBSTER data

[LOBSTER](https://lobsterdata.com) provides Nasdaq order-level data as two files per day:
a message file (one event per row: submit, partial cancel, delete, visible execution,
hidden execution, cross trade, trading halt) and an order book file (the top N levels
after each event). The [replay](include/lob/lobster.hpp) applies each recorded event to
the book directly instead of re-matching, and handles the format's quirks:

- **Integer prices** (dollars x 10,000) map straight onto integer ticks.
- **Orders from before the file starts.** The book is seeded from the first snapshot as
  one anonymous order per level. A cancel or execution of an unknown order id is taken
  out of that level's anonymous volume.
- **Hidden executions and cross trades** are counted as trades but leave the visible book
  alone.
- **Execution direction** refers to the resting order's side.
- **Empty levels** use placeholder prices (±9,999,999,999) and are dropped.
- **Only the top 10 levels are reported.** LOBSTER's message file only contains events
  that change the book within the requested 10 levels. Once a level drifts beyond the
  10th, changes to it go unreported, so when it comes back into view its contents can't
  be known from the messages. The check is strict wherever the data allows: after every
  message, every level at or better than the previous row's 10th level must match
  exactly. A level that comes into view from beyond that window is loaded from the
  snapshot as anonymous volume and counted. About 98% of checked rows (2,076,364 of 2,110,855)
  also match on all 10 levels outright.

On a mismatch, `lob_replay` prints the message index, the event and both books side by
side:

```
Mismatch at message 5: time_ns=34202000000001 type=3 id=101 size=100 price=1010000 side=sell
  lvl ours bid          ours ask          file bid          file ask
* 1   130@1000000       180@1010000       130@1000000       181@1010000
```

## Build and run

Requires CMake 3.25+ and a C++20 compiler. GoogleTest and Google Benchmark are downloaded
during configuration.

```
cmake -S . -B build                    # Release, -O3 -march=native
cmake --build build -j
./build/lob_tests                      # unit, differential and replay tests
./build/lob_allocation_tests           # no-allocation check

scripts/download_data.sh               # LOBSTER samples, ~600 MB (see data/README.md)
./build/lob_replay data/AAPL_2012-06-21_34200000_57600000_message_10.csv \
                   data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv
./build/lob_micro_bench                # add / cancel / match
./build/lob_replay_bench               # full-day throughput and latency
```

Sanitizer build:

```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON
cmake --build build-asan -j && ./build-asan/lob_tests
```

The replay tests skip themselves when `data/` is empty.

## Layout

```
include/lob/   types, OrderBook interface, map and ladder books, object pool,
               order-id map, LOBSTER parser and replay
src/           implementations, lob_replay tool
tests/         typed tests run on every book, naive oracle, differential test,
               LOBSTER fixtures and replay tests, allocation test
bench/         Google Benchmark microbenchmarks, full-day replay benchmark
scripts/       data download
data/          LOBSTER samples (not committed)
```
