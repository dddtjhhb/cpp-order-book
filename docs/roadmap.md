# cpp-order-book：v0.6.0 之后的项目路线图

> 进度：v0.7 的统一 EngineResult、稳定拒绝码、可成交改单、symbol/request sequence、
> 严格 CSV 校验、独立参考模型对拍和状态转换文档已实现。网络层尚未实现。
> `sequence` 当前只用于关联响应，不提供去重或跨客户端排序。
> 详细行为见 [engine contract](engine-contract.md)。以下 v0.6 基线为历史判断。

## 1. 项目定位

主线定位：**Quant SWE / Systems SDE / Performance Engineering**。

这个项目不需要被包装成交易策略、Quant Research 或 MLE 项目。它应该证明四件事：

1. 能正确实现复杂、可变的状态机；
2. 能把 C++ 核心封装成有真实输入输出边界的服务；
3. 能设计可信的性能实验，而不只报告一个吞吐数字；
4. 能解释正确性、延迟、吞吐、并发和复杂度之间的工程取舍。

目标的一句话描述：

> A tested C++17 price-time-priority matching engine exposed through a framed TCP replay service, with deterministic per-symbol processing and stage-level latency benchmarks.

## 2. v0.6.0 基线判断

当前版本已经具备：

- 整数 tick 价格；
- price-time priority 和 resting-price execution；
- ADD / CANCEL / MODIFY / EXECUTE；
- `std::map + std::list + std::unordered_map` 的清晰数据结构；
- 可复现的生命周期与撮合 benchmark；
- CUSUM / EWMA 性能回归实验；
- stateful property fuzzing、失败序列缩减和 mutation experiment；
- CI 中的单元测试、smoke benchmark、ASan 和 UBSan。

这已经不是“只有一个 order book 数据结构”的项目。当前最大缺口是系统边界：输入仍来自进程内事件或 CSV，输出没有形成稳定的 execution-report contract，也没有 transport、backpressure、multi-client、multi-symbol 或端到端延迟。

在联网前有两个必须收口的语义问题：

- `process(ADD)` 调用 `submit()` 后只保留 accepted/message，成交明细没有通过统一结果类型返回；
- `MODIFY` 把订单改成 marketable price 时不会重新撮合，README 也将其列为限制。

因此，不建议直接从 v0.6.0 跳到多线程 TCP server。

## 3. 推荐版本路线

### v0.7.0 — Stable Engine Contract（约 1 周）

目标：先让撮合核心拥有唯一、完整、可供网络层调用的输入输出合同。

范围：

- 用统一的 `EngineResult` 替代分裂的 `ProcessResult` / `SubmitResult`；
- 每次请求返回 request status、reason code、resting quantity 和 0..N 个 trade/execution reports；
- 为拒单原因定义稳定 enum，不再让调用方解析字符串；
- 明确 MODIFY 语义：同价减量或数量不变保留优先级；改价或增量采用 cancel-replace，新价格可成交时执行正常撮合；
- 为事件增加 `symbol_id`，即使第一版仍只运行一个 symbol；
- 增加 request sequence number，为重放、去重和审计留出边界；
- 补齐 table-driven 单元测试和 property invariants；
- README 写清楚状态转换和所有拒绝条件。

验收标准：

- 所有命令通过同一个入口返回完整结果；
- marketable modify 有明确且被测试的行为；
- 相同输入序列产生字节级一致的 logical outputs；
- 现有 property fuzzing 和 sanitizer CI 全部通过；
- 不引入网络代码。

建议拆分的 GitHub issues：

1. Define unified engine command/result model
2. Replace string errors with stable reason codes
3. Implement and test marketable cancel-replace
4. Add symbol and sequence fields
5. Extend stateful properties for the new result model

### v0.8.0 — Framed TCP Replay Service（约 2 周）

目标：把 in-process matching engine 升级成可由外部客户端调用的 stateful service。

第一版保持简单：**一个线程内运行 poll 事件循环、连接处理与撮合 + 单 symbol**。不要一开始上复杂 async framework。

协议建议：

- length-prefixed binary frames；
- header 至少包含 protocol version、message type、payload length、client/session sequence；
- 固定整数类型、明确 network byte order；
- request：Submit / Cancel / Replace；
- response：Accepted / Rejected / ExecutionReport；
- 限制最大 frame size；
- 解析层和撮合层完全分离。

必须正确处理：

- TCP partial read 和 partial write；
- 一次 read 收到多个 frame；
- malformed/oversized frame；
- client disconnect；
- unknown message version/type；
- slow client；
- 请求和多个 execution reports 的关联。

建议使用 Linux/POSIX sockets，先做阻塞式或 `poll` 版本。此阶段的学习目标是理解 TCP stream、framing 和 ownership，而不是证明会使用 Boost.Asio。

测试分层：

- codec unit tests；
- fragmented-frame tests；
- `socketpair` integration tests；
- loopback client/server tests；
- malformed-input fuzz target；
- 断线和慢消费者测试。

验收标准：

- client 可从文件读取事件，通过 TCP 发送并验证返回结果；
- 任意拆分 frame 都能得到相同的解码结果；
- 非法 frame 不会破坏 book state；
- 每个已接受请求都有可关联的响应；
- README 有 protocol table 和 sequence diagram。

建议拆分的 GitHub issues：

1. Specify versioned wire protocol
2. Implement incremental frame decoder/encoder
3. Add single-threaded TCP server
4. Add replay client and response verifier
5. Add socket-level integration and malformed-frame tests
6. Fuzz frame decoder with libFuzzer or AFL++

### v0.9.0 — End-to-End Performance Study（约 1–2 周）

目标：回答“时间花在哪里”，而不是只把 events/s 变大。

测量范围分成四层：

1. codec decode/encode；
2. socket transport；
3. queueing（单线程版本记录等待处理时间；跨线程版本再分离 ingress queue 时间）；
4. matching engine。

实验矩阵：

- event mix：add/cancel/replace/match 的不同比例；
- active price levels：10 / 100 / 1,000；
- live order count；
- match sweep depth：1 / 5 / 20 levels；
- steady load 与 burst load；
- 1 个和多个 client；
- payload batching on/off。

每个场景报告：

- throughput；
- p50 / p95 / p99 / max latency；
- rejection count；
- CPU utilization；
- allocation count；
- 至少 10 次独立进程运行的 median 和区间，而不是只给一次最好成绩。

方法要求：

- 预热与正式测量分开；
- 原始结果保存为 CSV；
- 固定 compiler、flags、hardware、OS、CPU governor/affinity 条件；
- CI 只跑 correctness 和 benchmark smoke test，不用共享 runner 做硬性能门禁；
- 使用 profiler 后再优化；
- 每次优化同时报告速度变化、内存变化和代码复杂度代价。

验收标准：

- 一条命令可生成全部原始数据和图表；
- 结果可复现并注明环境；
- 至少识别并解释一个真实 hotspot；
- README 明确区分 engine-only 与 end-to-end latency。

### v0.10.0 — Multi-client / Multi-symbol Concurrency（约 2 周）

目标：做一个有控制、有对照组的并发实验。

推荐架构：

```text
TCP clients
    |
    v
I/O + frame parsing
    |
    v
bounded ingress queues -- hash(symbol_id) --> single-writer shard(s)
                                            |
                                            v
                                    order books + reports
```

核心原则：

- 每个 symbol 同一时刻只有一个 owner/thread 修改状态；
- 同一 symbol 由服务端明确的排序点分配处理序号，并记录该顺序用于确定性重放；
- client/session sequence 用于请求关联及未来去重，不直接决定跨客户端的先后顺序；
- symbol 之间可以并行；
- bounded queue 提供可观察的 backpressure；
- 不承诺跨 symbol 的全局事件顺序。

至少比较三种实现：

1. 单线程 baseline；
2. 一个全局锁保护多个 books；
3. 按 symbol 分片的 single-writer workers。

测量：1/2/4/8 clients、1/4/16/64 symbols，不同 skew（均匀与单热点 symbol），并报告吞吐、p99、queue depth、contention/backpressure。

验收标准：

- ThreadSanitizer 通过；
- 热点 symbol 不发生乱序；
- queue 满时行为明确：block、reject 或 disconnect，只选一种并记录理由；
- 并发提升和退化都能被实验解释；
- 不以“lock-free”作为版本目标。

### v1.0.0 — Reproducible Portfolio Release（约 1 周）

目标：把前面成果收口成招聘者和面试官能快速理解的版本。

必须交付：

- architecture document；
- wire-protocol specification；
- correctness and failure-model document；
- benchmark methodology、raw results 和生成脚本；
- 一张 architecture diagram；
- 一张 latency breakdown 图；
- 一张 concurrency scaling 图；
- Docker 或明确的 Linux build instructions；
- license、versioning policy、limitations；
- 2–3 分钟 demo：启动 server、运行 replay client、展示 execution reports 和 metrics。

可选 stretch goal：

- append-only journal + snapshot/recovery；
- crash 后 replay 并验证最终 state hash；
- Prometheus-style metrics endpoint。

只有在 v0.7–v0.10 完成后再做这些。持久化是有价值的，但对当前项目，network boundary、端到端测量和并发 ownership 更优先。

## 4. 八周执行安排

| 周 | 主要任务 | 可展示产物 |
|---|---|---|
| 1 | 统一 engine result；修复 marketable modify；加入 symbol/sequence | v0.7.0 + 状态转换文档 |
| 2 | wire protocol、codec、fragmentation tests | protocol spec + codec tests |
| 3 | TCP server/client、socket integration tests | v0.8.0 demo |
| 4 | 端到端 benchmark harness 与阶段计时 | raw CSV + latency breakdown |
| 5 | workload matrix、重复实验、profiling | v0.9.0 report |
| 6 | bounded queues、single-writer shard | 并发实现 |
| 7 | baseline 对照、TSan、hot-symbol 实验 | v0.10.0 scaling report |
| 8 | README、图、demo、release polish | v1.0.0 portfolio release |

每周如果只有 5–7 小时，把每一行拆成两周，总体改成 12–16 周，不要删测试和实验设计去赶版本号。

## 5. 明确不做的内容

在 v1.0 前不建议加入：

- market/stop/iceberg 等大量新订单类型；
- FIX、CME iLink 或 NASDAQ ITCH 的半成品模仿；
- Kafka、Kubernetes、云部署全家桶；
- FPGA、CUDA、DPDK；
- lock-free everywhere；
- alpha model、价格预测或随手拼出的交易策略；
- GUI。

这些内容会扩大表面积，但不能补上项目当前最关键的系统边界和实验可信度。

## 6. 做到什么程度就可以写进简历

完成 v0.8 后可写：

> Built a versioned, length-prefixed TCP replay service around a C++17 price-time-priority matching engine, handling fragmented streams, malformed frames, and multi-report order executions with socket-level integration tests.

完成 v0.9 后可再写：

> Designed reproducible end-to-end benchmarks separating transport, parsing, queueing, and matching costs; reported throughput and p50/p95/p99 latency across workload mix, depth, and burst scenarios.

完成 v0.10 后可再写：

> Implemented deterministic single-writer, per-symbol sharding with bounded backpressure; compared single-threaded, globally locked, and sharded designs under multi-client load and checked for data races with ThreadSanitizer under tested workloads.

数字只在实验完成后填入，不提前承诺“sub-microsecond”“production-grade”或“HFT-ready”。

## 7. 面试时必须能回答的问题

- 为什么 price levels 用 `std::map`，什么时候 vector/flat map 更合适？
- 为什么 queue 用 `std::list`，它的 cache locality 代价是什么？
- cancel 为什么是平均 O(1) lookup，erase 的 iterator 为什么有效？
- partial fill、quantity decrease、quantity increase 如何影响 FIFO priority？
- TCP 为什么需要 framing？怎样处理半包和粘包？
- 为什么选择 single-writer ownership？同一 symbol 如何保证确定性？
- queue 满了如何 backpressure？为什么选 block/reject 中的某一个？
- 如何区分 engine latency 和 end-to-end latency？
- 为什么 p99 会抖动？怎样减少 benchmark 偏差？
- property fuzzing 能证明什么，不能证明什么？mutation result 为什么不能推广成总体检出率？

如果这些问题都能结合代码和数据回答，项目就已经达到一段很强的 Quant SWE / Systems SDE 作品集项目应有的深度。

## 8. 最小成功标准

如果时间不足，只完成以下三项：

1. v0.7：统一且正确的 engine contract；
2. v0.8：单线程 framed TCP service；
3. v0.9：可信的端到端 latency study。

到这里就可以停。它比一个功能更多、但协议不稳、没有可靠实验、无法解释并发语义的“大而全交易系统”更有说服力。
