# C++ Limit Order Book Replay Engine

A small, reproducible market-infrastructure project for learning how ordered market events update a limit order book. It supports `ADD` and `CANCEL` events, tracks the best bid and ask, rejects malformed event sequences, and measures replay throughput.

This is an educational systems project—not a production exchange gateway, trading strategy, alpha model, or implementation of CME iLink.

## What the program does

1. Reads timestamped order events from CSV.
2. Maintains aggregated quantity at each bid and ask price level.
3. Uses an order-ID index so cancellations do not require scanning the book.
4. Reports best bid, best ask, spread, rejected events, and replay throughput.
5. Tests ordering, cancellation, validation, and CSV parsing behavior.

Prices are stored as integer ticks: `10025` represents `$100.25` when one tick is one cent. Integer prices avoid floating-point equality and ordering problems.

## Architecture

```text
CSV events
    |
    v
CSV reader -> Event -> OrderBook -> top of book / validation result
                            |
                            v
                    throughput benchmark
```

The book uses:

- `std::map<Price, Quantity>` for ordered bid and ask price levels;
- `std::unordered_map<OrderId, Order>` for average-case constant-time order lookup;
- value semantics because order ownership is simple and does not require dynamic allocation.

`std::map` prioritizes clarity and correctness. A production low-latency system would likely use specialized contiguous storage, custom allocators, and exchange-specific price bounds.

## Build and test with CMake

Requires a C++17 compiler and CMake 3.16+.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/order_book_replay data/sample_events.csv
./build/order_book_benchmark 500000
```

## Build directly with Clang or GCC

```bash
mkdir -p build
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
  src/order_book.cpp src/csv_reader.cpp src/main.cpp \
  -o build/order_book_replay
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
  src/order_book.cpp src/csv_reader.cpp tests/order_book_tests.cpp \
  -o build/order_book_tests
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
  src/order_book.cpp benchmarks/replay_benchmark.cpp \
  -o build/order_book_benchmark

./build/order_book_tests
./build/order_book_replay data/sample_events.csv
./build/order_book_benchmark 500000
```

## CSV contract

```text
timestamp_ns,event_type,order_id,side,price_ticks,quantity
1000,ADD,1,BUY,10025,10
1010,ADD,2,SELL,10030,8
1020,CANCEL,1,BUY,10025,10
```

For `CANCEL`, only `order_id` determines which stored order is removed; the remaining columns are retained to keep the event schema uniform.

## Benchmark methodology

The benchmark generates alternating add/cancel pairs in memory, processes them sequentially, and reports events per second. Build in Release mode before reporting results. Numbers depend on hardware, compiler, optimization flags, workload, and measurement scope, so results should always include that context.

The benchmark intentionally excludes CSV parsing to isolate order-book update cost. The CLI measurement includes replay processing after parsing, but not file loading.

## Current limitations

- Only `ADD` and full-order `CANCEL` are implemented.
- Quantity is aggregated by price level; price-time priority within a level is not modeled.
- No matching engine, partial fills, modify events, fees, or strategy logic.
- Events are processed on one thread to preserve deterministic order.
- The synthetic benchmark is not representative of CME traffic.
- This code has not been connected to CME iLink, exchange multicast feeds, FPGA hardware, or colocation infrastructure.

## Next engineering experiments

1. Add `MODIFY` and partial execution events with invariant tests.
2. Compare aggregated price levels with per-order FIFO queues.
3. Profile CSV parsing separately from book updates.
4. Test a bounded producer-consumer queue for parsing and replay, then retain it only if measured throughput justifies synchronization complexity.
5. Export spread and order-flow metrics for downstream time-series change detection.
