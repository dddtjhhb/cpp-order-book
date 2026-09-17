# C++ Limit Order Book and Matching Engine

A small, reproducible market-infrastructure project for learning how ordered market events update a limit order book. Version 0.7 adds a unified engine contract, marketable replacements, strict CSV validation, and differential stateful testing against an independent reference model.

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
CSV reader -> Event -> OrderBook -> EngineResult / top of book
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

## Stable engine contract (v0.7)

All commands use `process(Event)` and return `EngineResult`: acceptance, a stable
`ResultCode`, remaining quantity, zero or more ordered trades, and echoed symbol /
request sequence. Marketable modifications now match immediately at resting prices;
same-price quantity reductions and unchanged quantities preserve FIFO priority.

See [the engine contract](docs/engine-contract.md) for the input/output tables,
validation order, state transitions, ID/sequence semantics and failure model.
`ProcessResult` and `SubmitResult` have been replaced by `EngineResult`; the public
resting-only `add` helper has been removed in favor of `submit` or `process(Add)`.

```cpp
lob::OrderBook book(1);
const auto result = book.process({1000, lob::EventType::Add,
    {42, lob::Side::Buy, 10025, 10}, 1, 7});
// result.symbol_id == 1, result.sequence == 7
// Inspect result.code and result.trades; do not parse result.message.
```

## Build and test with CMake

Requires a C++17 compiler and CMake 3.16+. Without CMake, use `make -j4 all test`
(the test target also requires Python 3), or the direct compiler commands below.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/order_book_replay data/sample_events.csv
./build/order_book_replay data/marketable_modify.csv
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
  src/order_book.cpp src/csv_reader.cpp tests/engine_contract_tests.cpp \
  -o build/engine_contract_tests
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
./build/engine_contract_tests
./build/order_book_replay data/sample_events.csv
./build/order_book_replay data/marketable_modify.csv
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

The legacy six-column schema uses symbol 1 and assigns request sequences 1, 2, ...
in data-row order. An optional eight-column schema appends `symbol_id,sequence`;
see [the marketable-modify example](data/marketable_modify.csv). The CLI runs symbol 1.
A file must use one consistent schema; headers, when supplied, must match exactly.
CRLF is supported. Numeric fields must be complete decimal integers in range;
unsigned fields reject negative values, and extra columns, empty numbers, whitespace
and numeric suffixes are rejected with a line number. Quoted CSV fields are unsupported.
Zero quantity / nonpositive price are parsed when representable and rejected by the
engine where that command requires positive values.

For `CANCEL`, only `order_id` determines which stored order is removed. For `EXECUTE`, `order_id` and `quantity` are used. The remaining columns keep the schema uniform.

## Benchmark methodology

The benchmark supports two deterministic in-memory workloads:

- `lifecycle`: alternating ADD/CANCEL pairs across multiple price levels;
- `matching`: a resting sell followed by a crossing buy, producing one trade per pair.

It runs each workload twice on fresh books. The first pass measures batch throughput without a clock read around every event. The second, instrumented pass records per-event p50, p95, p99, and maximum latency. Keeping the passes separate prevents per-event timing overhead from contaminating the throughput number.

Build in Release mode before reporting results. Latencies include the overhead and resolution limits of `std::chrono::steady_clock`, so they are useful for controlled comparisons on the same machine—not claims about exchange-grade production latency. Numbers depend on hardware, compiler, optimization flags, workload, system load, and measurement scope.

Both current workloads return the book to empty after each pair, with at most one
resting order. They measure minimal operation paths, not populated-book depth,
long FIFO queues or realistic multi-order sweeps. Historical v0.4/v0.5 figures below
have not been rerun for the v0.7 result contract and must not be treated as v0.7 results.

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

The stateful property fuzzer generates reproducible sequences of submit, cancel,
modify and execute operations from a fixed seed, including crossing replacements and
rejected commands. Every operation is submitted through `process` and compared with
an independent reference book implemented as an arrival-ordered vector with linear
scans. Comparisons cover every trade and the complete live-order/FIFO state. It also
audits properties broader than individual examples:

- every indexed order appears exactly once in a FIFO queue;
- each stored iterator, side, and price agrees with its price level;
- each level's aggregate quantity equals the sum of its resting orders;
- empty levels and zero-quantity resting orders cannot remain;
- automatic matching cannot leave a crossed resting book;
- submitted quantity equals traded quantity plus resting quantity;
- every trade respects best-price and FIFO priority, quantity and resting price;
- same-price quantity reductions preserve priority, while increases or price changes move an order to the FIFO back.

```bash
./build/order_book_property_fuzz 1 10000 fuzz_failure.txt
./build/order_book_property_fuzz 42 10000 fuzz_failure.txt
```

The seed makes a generated run reproducible. If a property fails, the runner removes chunks of operations while preserving the failure category and writes the reduced sequence, seed, failing step, and reason to the requested file. This is deterministic differential random testing rather than coverage-guided fuzzing such as libFuzzer or AFL++.

### Mutation experiment

`analysis/mutation_experiment.py` creates temporary source copies, injects five controlled faults, and runs both unit-test binaries and up to five fixed-seed property-fuzz runs (stopping after the first kill). Compilation failures abort the experiment instead of being counted as killed mutants. Mutated source files are deleted with the temporary directory and never replace production code.

The following table records the **historical v0.6 experiment**, before the v0.7
contract tests and independent reference model. Current rerun results are stored in
[mutation_results.csv](docs/testing/mutation_results.csv). In the v0.7 rerun, both
the combined unit suites and differential fuzzer killed all five selected mutants;
this is still not a general detection-rate estimate.

| Controlled mutant | Unit tests | Property fuzzer |
|---|---|---|
| Skip level-total update during execution | Survived | Killed |
| Skip level-total update during cancellation | Survived | Killed |
| Reset FIFO priority for an equal-quantity modification | Survived | Killed |
| Require strict inequality for a crossing buy | Killed | Killed |
| Require strict inequality for a crossing sell | Survived | Killed |

In that v0.6 run, for this deliberately selected mutant set, unit tests killed 1/5 and the property fuzzer killed 5/5. This does not establish a general detection rate: the mutants are few, hand-written, and related to the encoded properties. It demonstrates that invariant-driven random sequences cover interactions absent from the current example-based tests.

## Priority rules

- New orders join the back of their price level's FIFO queue.
- A quantity decrease at the same price preserves priority.
- A quantity increase or price change resets priority.
- A partial execution reduces remaining quantity without changing priority.
- A full execution removes the order and deletes an empty price level.

## Current limitations

- Only limit orders are matched; market orders and time-in-force instructions are not implemented.
- One configured symbol per book; no session sequencing, deduplication or client report routing.
- `EXECUTE` is an external replay adjustment and does not synthesize counterparty trades.
- No fees, exchange-specific protocol rules, persistence, networking, or strategy logic.
- Events are processed on one thread to preserve deterministic order.
- The synthetic benchmark is not representative of CME traffic.
- This code has not been connected to CME iLink, exchange multicast feeds, FPGA hardware, or colocation infrastructure.

## Next engineering experiments

The next milestone is a single-threaded, length-prefixed TCP replay service with
codec / fragmented-frame / slow-client tests, followed by end-to-end latency
measurements. See the [post-v0.6 roadmap](docs/roadmap.md). Further test and performance experiments:

1. Add coverage-guided fuzzing and compare reached branches with the fixed-seed generator.
2. Expand mutation operators and repeat the comparison on an independently chosen mutant set.
3. Repeat each regression scenario across processes and report uncertainty across runs.
4. Use a system profiler to identify allocation and container hot spots after an alarm.
5. Test networked replay while separating transport, parsing, and book-processing cost.
