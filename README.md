# C++ Limit Order Book and Matching Engine

A small, reproducible market-infrastructure project for learning how ordered market events update a limit order book. Version 0.7 is a correctness release: marketable modifications now match instead of leaving a crossed book, the CSV reader rejects malformed numbers instead of silently wrapping them, and the property fuzzer no longer avoids the input region that hid the modify bug. See [v0.7 correctness fixes](#v07-correctness-fixes).

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
- A price change or quantity increase is a cancel/replace: the replacement goes through the same matching path as a new order, so a modification that crosses the spread trades immediately (the modified order is the aggressor).

Each trade records a trade ID, incoming and resting order IDs, execution price, quantity, and timestamp.

## Build and test with CMake

Requires a C++17 compiler and CMake 3.16+.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/order_book_replay data/sample_events.csv
./build/order_book_benchmark 500000 lifecycle
./build/order_book_benchmark 500000 matching
./build/order_book_telemetry performance_telemetry.csv 120 20000 60 50
./build/order_book_property_fuzz 1 10000 fuzz_failure.txt
python3 analysis/performance_regression.py performance_telemetry.csv
python3 analysis/mutation_experiment.py
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
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
  src/order_book.cpp benchmarks/performance_telemetry.cpp \
  -o build/order_book_telemetry
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
  src/order_book.cpp tests/order_book_property_fuzz.cpp \
  -o build/order_book_property_fuzz

./build/order_book_tests
./build/order_book_replay data/sample_events.csv
./build/order_book_benchmark 500000 lifecycle
./build/order_book_benchmark 500000 matching
./build/order_book_telemetry performance_telemetry.csv 120 20000 60 50
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

`EXECUTE` is an externally reported fill against a resting order (market-data replay semantics). Trades produced by the engine's own matching come from `ADD` and `MODIFY`; they are returned by `OrderBook::process()` and counted by the replay CLI.

Parsing is strict: every row must have exactly six columns; integer fields must be plain base-10 digits with no whitespace, `+`, or trailing characters; `timestamp_ns`, `order_id`, and `quantity` must be non-negative and fit in 64 bits. Violations raise an error naming the line and column. Windows (CRLF) line endings are accepted. `price_ticks` is signed so that the book—not the parser—owns the rule that prices must be positive. The replay CLI exits non-zero if the final book fails `validate_invariants()`.

## Benchmark methodology

The benchmark supports two deterministic in-memory workloads:

- `lifecycle`: alternating ADD/CANCEL pairs across multiple price levels;
- `matching`: a resting sell followed by a crossing buy, producing one trade per pair.

It runs each workload twice on fresh books. The first pass measures batch throughput without a clock read around every event. The second, instrumented pass records per-event p50, p95, p99, and maximum latency. Keeping the passes separate prevents per-event timing overhead from contaminating the throughput number.

Build in Release mode before reporting results. Latencies include the overhead and resolution limits of `std::chrono::steady_clock`, so they are useful for controlled comparisons on the same machine—not claims about exchange-grade production latency. Numbers depend on hardware, compiler, optimization flags, workload, system load, and measurement scope.

The benchmark intentionally excludes event generation and CSV parsing from its timed passes to isolate order-book update and matching cost. The CLI measurement includes replay processing after parsing, but not file loading.

### Example v0.4 results

One local run on an Apple M4 MacBook Air with Apple Clang 17, Release `-O2`, and 1,000,000 events produced:

| Workload | Throughput | p50 | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| lifecycle | 22.66M events/s | 42 ns | 42 ns | 42 ns | 22,959 ns |
| matching | 19.92M events/s | 42 ns | 42 ns | 84 ns | 9,750 ns |

These are example measurements, not universal performance guarantees. The repeated 42 ns values reflect timer granularity as well as program work; maximum latency is especially sensitive to operating-system scheduling and background activity.

## Performance-regression experiment

The telemetry runner divides a deterministic event stream into time-ordered batches. The first half uses ADD/CANCEL lifecycle pairs. After a known change point, a configurable share of those pairs becomes a resting sell followed by a crossing buy, exercising the matching and trade-record path. Event generation and CSV parsing remain outside the timed region.

Each batch records throughput, p50/p95/p99/max latency, rejected events, active orders, and active price levels. Ten unrecorded lifecycle batches warm the relevant code paths before calibration begins.

```bash
./build/order_book_telemetry results/telemetry_25.csv 120 20000 60 25
./build/order_book_telemetry results/telemetry_50.csv 120 20000 60 50
./build/order_book_telemetry results/telemetry_100.csv 120 20000 60 100

python3 analysis/performance_regression.py \
  results/telemetry_25.csv results/telemetry_50.csv results/telemetry_100.csv \
  --output-dir results
```

The Python analysis calibrates both detectors from the first 30 baseline batches:

- lower-sided CUSUM accumulates standardized throughput drops and alarms when cumulative evidence exceeds a fixed threshold;
- lower-sided EWMA gives recent batches more weight and alarms when its smoothed value crosses a fixed control limit.

Neither detector reads the known change point or any future observation. The change point is used only after detection to count false alarms and calculate detection delay. The thresholds are intentionally conservative defaults for this controlled experiment, not universally tuned production settings.

This experiment asks whether a sustained execution-path shift can be detected above ordinary measurement noise. It does not diagnose the root cause, prove that matching is always slower, or represent production exchange traffic.

### Example v0.5 detection results

One local Apple M4 run with 120 batches, 20,000 events per batch, and a change at batch 60 produced:

| Post-change matching share | Mean throughput change | CUSUM false alarms / delay | EWMA false alarms / delay |
|---:|---:|---:|---:|
| 25% | +4.1% | 0 / not detected | 0 / not detected |
| 50% | -9.3% | 0 / 2 batches | 0 / 2 batches |
| 100% | -14.5% | 0 / 4 batches | 0 / 4 batches |

![Throughput time series with the controlled shift and detector alarms](docs/performance-regression/telemetry_50.svg)

The 25% workload shift was not a measurable slowdown in this run, so neither detector raised an alarm. The non-monotonic detection delays and the apparent 25% speedup are evidence that a single process run is noisy; repeated runs and uncertainty estimates are required before drawing a general performance conclusion.

## Property-based testing and fuzzing

**Research question:** can stateful invariant checking detect interaction bugs that the existing example-based unit tests miss?

The stateful property fuzzer generates reproducible sequences of submit, cancel, modify, and execute operations from a fixed seed. After every operation it audits properties that are broader than individual examples:

- every indexed order appears exactly once in a FIFO queue;
- each stored iterator, side, and price agrees with its price level;
- each level's aggregate quantity equals the sum of its resting orders;
- empty levels and zero-quantity resting orders cannot remain;
- automatic matching cannot leave a crossed resting book;
- submitted quantity equals traded quantity plus resting quantity;
- the first trade respects best-price and FIFO priority;
- same-price quantity reductions preserve priority, while increases or price changes move an order to the FIFO back.

```bash
./build/order_book_property_fuzz 1 10000 fuzz_failure.txt
./build/order_book_property_fuzz 42 10000 fuzz_failure.txt
```

The seed makes a generated run reproducible. If a property fails, the runner removes chunks of operations while preserving the failure and writes the reduced sequence, seed, failing step, and reason to the requested file. This is deterministic model-based random testing rather than coverage-guided fuzzing such as libFuzzer or AFL++.

### Mutation experiment

`analysis/mutation_experiment.py` creates temporary source copies, injects five controlled faults, and runs both the original unit suite and five fixed-seed property-fuzz runs. Mutated source files are deleted with the temporary directory and never replace production code.

| Controlled mutant | Unit tests | Property fuzzer |
|---|---|---|
| Skip level-total update during execution | Survived | Killed |
| Skip level-total update during cancellation | Survived | Killed |
| Reset FIFO priority for an equal-quantity modification | Survived | Killed |
| Require strict inequality for a crossing buy | Killed | Killed |
| Require strict inequality for a crossing sell | Survived | Killed |
| Marketable modify rests without matching (the v0.6 bug) | Killed | Killed |

For the original five mutants, unit tests killed 1/5 and the property fuzzer killed 5/5. The sixth mutant was added in v0.7 and reintroduces the modify bug; the v0.6 fuzzer does not kill it (see below). This does not establish a general detection rate: the mutants are few, hand-written, and related to the encoded properties. It demonstrates that invariant-driven random sequences cover interactions absent from the current example-based tests.

## v0.7 correctness fixes

Two bugs were confirmed in v0.6 and are now covered by regression tests in `tests/order_book_tests.cpp`.

**1. A marketable `MODIFY` left a crossed book.** With `BUY 1 @ 100` and `SELL 2 @ 105` resting, `MODIFY 1 -> BUY @ 110` rested the bid above the ask (spread `-0.05`) with no trade, and `validate_invariants()` reported `crossed resting book`. The fix routes every priority-resetting modification through `submit()`, so it matches exactly like a new order before any remainder rests. `modify()` and `process()` now return `SubmitResult`, which carries the trades.

**2. Negative CSV quantities were accepted.** `std::stoull("-5")` returns `2^64 - 5`, so a row with quantity `-5` created an order for 18,446,744,073,709,551,611 shares. The reader now uses `std::from_chars`, requires the whole field to be consumed, and reports overflow.

**Why the fuzzer missed bug 1.** The v0.6 generator clamped modify prices to the passive side of the spread (`min(price, best_ask - 1)` for bids), so the crossing path was never exercised. v0.7 draws modify prices from the full band and adds modify-specific properties: quantity conservation, the modified order as aggressor, "a marketable modify must trade", and "a fully filled modify leaves no order". Reintroducing the bug as a mutant shows the difference:

| Fuzzer | Seeds 1, 7, 42, 2026, 20260831 × 5,000 steps |
|---|---|
| v0.6 (clamped) | all pass — bug not detected |
| v0.7 (full band) | fails at step 28 of seed 1; minimized to 3 operations |

```text
# reason=marketable modify did not trade
SUBMIT,2299,BUY,10009,91
SUBMIT,2302,SELL,10020,25
MODIFY,2302,SELL,10005,64
```

The lesson is that a random-testing harness is only as good as its input distribution: a constraint added to keep generated states "valid" silently removed the interesting ones.

Verification for this release: unit and property tests under ASan/UBSan, 40 sanitized seeds × 5,000 steps, 100 release seeds × 20,000 steps, and the mutation experiment. Replay throughput is unchanged within run-to-run noise.

## Priority rules

- New orders join the back of their price level's FIFO queue.
- A quantity decrease at the same price preserves priority.
- A quantity increase or price change resets priority, and matches first if the new price is marketable.
- A partial execution reduces remaining quantity without changing priority.
- A full execution removes the order and deletes an empty price level.

## Current limitations

- Only limit orders are matched; market orders and time-in-force instructions are not implemented.
- No fees, exchange-specific protocol rules, persistence, networking, or strategy logic.
- Events are processed on one thread to preserve deterministic order.
- The synthetic benchmark is not representative of CME traffic.
- This code has not been connected to CME iLink, exchange multicast feeds, FPGA hardware, or colocation infrastructure.

## Roadmap

1. **v0.8 — differential testing.** A deliberately simple `ReferenceBook` (vector + linear scan) driven in lockstep with `OrderBook`, comparing trade streams, top of book, and per-level quantities after every operation. This replaces the current setup where the generator's model is the implementation under test.
2. **v0.9 — event log, snapshots, deterministic replay.** Sequenced input/output log; periodic snapshots; recovery from snapshot + log tail must reproduce the same state hash and trade stream as a full replay.
3. **v1.0 — mixed-workload benchmark and a profiling-driven optimization.** Deep books, realistic ADD/CANCEL/MODIFY/aggressive mix, repeated runs with uncertainty, then one measured change at a time (e.g. allocation-free result types, pooled intrusive order lists), each guarded by the differential tests.
