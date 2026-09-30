# Engine benchmark — 2026-09-29

| Machine | Value |
|---|---|
| CPU | Intel(R) Xeon(R) Processor @ 2.10GHz |
| Cores | 2 logical CPUs |
| Memory | 7 GiB |
| OS | Ubuntu 24.04.4 LTS (x86_64), kernel 6.18.44-fc-v50 |
| Power | n/a |
| Commit | `ba87fc2` |
| Command | `order_book_engine_benchmark ` |

## Results

| Setting | Value |
|---|---|
| Compiler | gcc 13.3.0 |
| Build flags | `Release  -O3 -DNDEBUG` |
| Book pre-fill | 5000 orders/side over 500 levels/side |
| Timed events per run | 1000000 (passive_add 43.8%, cancel 36.2%, modify 10.0%, aggressive_add 10.0%) |
| Trades per run | 175627 (18229 multi-level sweeps) |
| Resting orders at end | 10077 |
| Runs | 7 throughput, 3 latency (pooled), after 1 warm-up |
| Clock | steady_clock, resolution 26.0 ns, read cost 30.3 ns |

### Throughput (no per-event timing)

| Median | Min | Max | Mean time per event |
|---:|---:|---:|---:|
| 4.42 M events/s | 4.14 M | 4.60 M | 226.3 ns |

### Per-event latency (ns, includes one clock read)

| Operation | Samples | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| passive_add | 1314702 | 199 | 282 | 408 | 901 | 231791 |
| cancel | 1084671 | 217 | 288 | 428 | 1119 | 336514 |
| modify | 300318 | 346 | 504 | 686 | 4327 | 796789 |
| aggressive_add | 300309 | 321 | 595 | 980 | 4212 | 270923 |
| **all** | 3000000 | 219 | 367 | 648 | 2713 | 796789 |

Throughput min/max spread: 10.30% of median. Latencies below ~52 ns are quantized by the clock. `max` reflects OS scheduling and is not a property of the engine.
