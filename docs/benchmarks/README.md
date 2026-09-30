# Engine benchmarks

`order_book_engine_benchmark` measures `OrderBook::process()` on a **deep book with
a mixed workload**. It replaces the older `order_book_benchmark` as the number to
quote: that benchmark adds and immediately cancels (or immediately matches) orders
on an almost empty book, so it measures a best case that no real stream produces.

## Reproduce

```bash
scripts/run_engine_benchmark.sh
```

This builds in Release mode, runs about 30 seconds, and writes
`docs/benchmarks/<date>-<os>-<arch>.md` with the CPU, core count, memory, OS,
power source, compiler, flags and commit next to the numbers. Extra arguments go to
the benchmark, for example `--events 2000000 --runs 11 --depth 20000`. For stable
numbers, plug in power, disable Low Power Mode, and leave the machine idle.

## Workload

- **Pre-fill:** 5,000 resting orders per side over 500 price levels, exponentially
  denser toward the touch. The pre-fill is not timed.
- **Timed stream:** 1,000,000 requests, about 44% passive ADD, 36% CANCEL of a
  random live order, 10% MODIFY and 10% aggressive ADD. MODIFY is half same-price
  size-down (priority kept) and half passive re-price (priority reset). An
  aggressive ADD has its limit 0–2 ticks through the touch and a size of up to 200.
- **Trades:** about 176k per run, of which 18k sweep more than one price level.
- **Book size:** the cancel share adapts so the book stays near its pre-filled size
  (about 10k orders at the end).
- **Determinism:** the generator drives a model book, so every CANCEL or MODIFY
  targets a live order and every generated request is accepted. The stream is
  fixed for a given `--seed`.

## Method

| Choice | Reason |
|---|---|
| Events pre-generated; no parsing or I/O in timed regions | Measure the engine, not the harness |
| One untimed warm-up pass | Page faults, allocator and branch-predictor warm-up |
| Throughput runs time the whole replay with two clock reads | Per-event clock reads would inflate the cost per event |
| Latency runs are separate and read the clock around every `process()` | Per-operation distributions without distorting throughput |
| Fresh, identically pre-filled book for every run | Runs are independent and comparable |
| Median, min and max over runs | Shows run-to-run noise instead of a single best run |
| Clock resolution and read cost measured and printed | On Apple Silicon `steady_clock` ticks about every 41.7 ns, which quantizes p50 |
| `validate_invariants()` after each throughput run; a sink over trade counts | Proves the work happened and was correct; prevents dead-code elimination |

## Reading the numbers

- **Throughput** is the number to quote for "events per second". The mean time per
  event is its reciprocal.
- **Latency percentiles include one clock read** (the read cost is printed). p50
  and p90 are close to the clock's granularity on Apple Silicon. p99 and p99.9 are
  well above it, so they are meaningful.
- **`max` is dominated by OS preemption and page faults**, not by the engine. Do
  not quote it.
- These are single-thread, in-process numbers. They exclude networking; the TCP
  path is a separate measurement (roadmap v0.9).

## Recorded runs

Each file in this directory is one invocation of the script, committed unedited.
`2026-09-29-linux-x86_64-cloud-vm.md` is a reference run on a shared 2-vCPU cloud
VM. It checks that the harness works; it is not a hardware claim.
