# C++ Limit Order Book and Matching Engine

A small, reproducible market-infrastructure project for learning how ordered market events update a limit order book. Version 0.3 adds automatic limit-order matching and trade records to the FIFO order lifecycle implemented in v0.2.

This is an educational systems project—not a production exchange gateway, trading strategy, alpha model, or implementation of CME iLink.

## What the program does

1. Reads timestamped order events from CSV.
2. Maintains an ordered FIFO queue of individual orders at each bid and ask price level.
3. Uses an order-ID index with stored queue positions so cancellations do not require scanning the book.
4. Reports best bid, best ask, spread, rejected events, and replay throughput.
5. Tests ordering, cancellation, modification, partial/full execution, validation, and CSV parsing behavior.
6. Matches crossing limit orders at resting prices and emits auditable trade records.

Prices are stored as integer ticks: `10025` represents `$100.25` when one tick is one cent. Integer prices avoid floating-point equality and ordering problems.

## Architecture

```text
CSV events
    |
    v
CSV reader -> Event -> OrderBook -> top of book / validation result
                            |
                            v
                    matching engine
                     |          |
                     v          v
                trade records  benchmark
```

The book uses:

- `std::map<Price, PriceLevel>` for ordered bid and ask price levels;
- `std::list<OrderId>` for stable FIFO ordering within each price level;
- `std::unordered_map<OrderId, StoredOrder>` for average-case constant-time lookup and direct queue removal.

`std::map` prioritizes clarity and correctness. A production low-latency system would likely use specialized contiguous storage, custom allocators, and exchange-specific price bounds.

## Matching behavior

- A buy order matches while its limit price is greater than or equal to the best ask.
- A sell order matches while its limit price is less than or equal to the best bid.
- Better prices execute first; orders at the same price execute in FIFO order.
- Trades use the resting order's price.
- One incoming order may sweep multiple price levels.
- Any unfilled remainder becomes a resting order at the back of its price-level queue.

Each trade records a trade ID, incoming and resting order IDs, execution price, quantity, and timestamp.

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
1020,MODIFY,1,BUY,10025,8
1030,EXECUTE,1,BUY,10025,3
1040,CANCEL,1,BUY,10025,5
```

For `CANCEL`, only `order_id` determines which stored order is removed. For `EXECUTE`, `order_id` and `quantity` are used. The remaining columns keep the schema uniform.

## Benchmark methodology

The benchmark generates alternating add/cancel pairs in memory, processes them sequentially, and reports events per second. Build in Release mode before reporting results. Numbers depend on hardware, compiler, optimization flags, workload, and measurement scope, so results should always include that context.

The benchmark intentionally excludes CSV parsing to isolate order-book update cost. The CLI measurement includes replay processing after parsing, but not file loading.

## Priority rules

- New orders join the back of their price level's FIFO queue.
- A quantity decrease at the same price preserves priority.
- A quantity increase or price change resets priority.
- A partial execution reduces remaining quantity without changing priority.
- A full execution removes the order and deletes an empty price level.

## Current limitations

- Only limit orders are matched; market orders and time-in-force instructions are not implemented.
- Replay `MODIFY` events update resting state but do not yet emit trades when a modification becomes marketable.
- No fees, exchange-specific protocol rules, persistence, networking, or strategy logic.
- Events are processed on one thread to preserve deterministic order.
- The synthetic benchmark is not representative of CME traffic.
- This code has not been connected to CME iLink, exchange multicast feeds, FPGA hardware, or colocation infrastructure.

## Next engineering experiments

1. Profile parsing, book updates, and matching separately and report p50/p95/p99 latency.
2. Add market-order and time-in-force semantics such as IOC.
3. Compare the FIFO implementation with more cache-friendly storage.
4. Test a bounded producer-consumer queue for parsing and replay, then retain it only if measured throughput justifies synchronization complexity.
5. Export trades, spread, and order-flow metrics for downstream analysis.
