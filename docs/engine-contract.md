# Engine contract (v0.7)

The engine is a synchronous, single-owner C++17 state machine. `OrderBook(symbol_id)`
owns one symbol; the default symbol is `1`. It is movable but cannot be copied,
because its index contains iterators into its FIFO queues.

## Input and output

`process(const Event&)` is the uniform command boundary. Every event contains:

| Field | Type | Meaning |
|---|---|---|
| `timestamp_ns` | `uint64_t` | Caller-supplied logical timestamp, copied into generated trades |
| `type` | `EventType` | Add, Cancel, Modify, Execute |
| `order.id` | `uint64_t` | ID unique among currently resting orders in this book |
| `order.side` | `Side` | Buy or Sell; used for Add only |
| `order.price` | `int64_t` | Positive integer ticks; used for Add and Modify |
| `order.quantity` | `uint64_t` | Positive quantity; used for Add, Modify and Execute |
| `symbol_id` | `uint32_t` | Must match this book's symbol; defaults to 1 |
| `sequence` | `uint64_t` | Opaque correlation value; defaults to 0 |

All commands return `EngineResult`:

| Field | Meaning |
|---|---|
| `accepted` | True exactly when `code == ResultCode::Accepted` |
| `code` | Stable numeric reason code; never parse diagnostic text |
| `message` | Human-readable diagnostic, not part of the stable logical contract |
| `trades` | Zero or more trades in matching order, including trades caused by Modify |
| `resting_quantity` | Requested order's remaining quantity after success; zero for Cancel or rejection |
| `symbol_id`, `sequence` | Echo the request, including rejected requests |

Each trade contains a book-local monotonically increasing ID starting at 1, incoming
and resting order IDs, resting execution price, executed quantity and the request's
logical timestamp. The containing result identifies the symbol and request for all
its trades. One trade describes both counterparties; it is not a per-client delivery
notification. Rejections produce no trades and do not consume trade IDs.

The direct `submit`, `cancel`, `modify` and `execute` helpers return the same result
type, with the book's symbol and sequence 0. `modify` accepts an optional timestamp
(default 0); use `process` whenever request correlation matters. The unsafe public
resting-only `add` entry point has been removed; use `submit(order, timestamp)` or
`process(Add)` so matching and validation cannot be bypassed.

## State transitions

| Command | Success behavior |
|---|---|
| Add | Match at best opposite price, then FIFO; rest any remainder at the back |
| Cancel | Remove the stored order and any empty price level |
| Modify, same price and quantity unchanged/reduced | Replace remaining quantity in place, preserving FIFO |
| Modify, price changed or quantity increased | Remove the original, match the replacement, then rest any remainder at the back |
| Execute | Reduce a stored order by the specified amount, preserve FIFO on partial execution, remove on full execution |

Modify quantity is the **new remaining quantity**, not original lifetime quantity.
Modify preserves the stored side and order ID; the side field in its input is ignored.
A fully matched replacement leaves no resting order. Decreasing quantity to zero is
rejected; use Cancel explicitly.

Execute is a replay adjustment for externally supplied execution quantity. It does
not infer a counterparty and therefore emits no `Trade`. It is separate from the
automatic trades produced by Add and Modify and need not be exposed by a future
client-facing Submit/Cancel/Replace protocol.

IDs can be reused after cancellation or full fill. Request sequences are echoed,
not validated for order and not used for deduplication. Repeating a sequence can
mutate the book again. Multi-client sequencing, sessions and idempotency belong to
a future service contract. The arrival order of `process` calls is the engine order.

## Rejections and validation order

All business rejections leave the original state and FIFO priority unchanged.
Symbol validation happens before command dispatch. Each command then validates in
the order listed below; the first failing condition supplies the reason code.

| Command | Validation order |
|---|---|
| Add | Side, positive quantity, positive price, duplicate live ID, level capacity |
| Modify | Existing ID, positive price, positive quantity, destination level capacity |
| Cancel | Existing ID |
| Execute | Existing ID, positive quantity, quantity not exceeding remainder |

| Code | Name | Condition |
|---:|---|---|
| 0 | Accepted | Accepted |
| 1 | InvalidQuantity | Zero quantity where positive quantity is required |
| 2 | InvalidPrice | Zero or negative price for Add/Modify |
| 3 | DuplicateOrderId | Add reuses a currently resting ID |
| 4 | UnknownOrderId | Cancel/Modify/Execute addresses a missing ID |
| 5 | ExecutionQuantityExceedsRemaining | Execute exceeds remaining quantity |
| 6 | UnsupportedEventType | Event type outside supported enum values |
| 7 | InternalInvariantViolation | Reserved numeric code; internal logic failures throw and require stopping the instance |
| 8 | InvalidSide | Add side outside Buy/Sell |
| 9 | UnknownSymbol | Event symbol differs from the book's symbol |
| 10 | QuantityOverflow | Destination price-level total would exceed `UINT64_MAX` |

Capacity checks subtract the original order for a same-level replacement and run
before removal or matching. In an uncrossed book, an existing same-side level at an
incoming price cannot also cross the opposite book, so preflight capacity checking
does not reject an order whose partial execution would otherwise make it fit.
The invariant auditor also checks aggregate overflow explicitly, rather than
allowing both its sum and the stored total to wrap identically.

Allocation failure and internal logic errors are fatal to the replay/service
instance, not recoverable business rejections. The engine does not offer transaction
rollback after allocation failure; a caller must stop using that instance. There is
no crash recovery, persistence, cross-symbol atomicity or thread safety. Exhaustion
of lifetime trade-ID space is outside this educational implementation's supported
operating bounds.

## Determinism and verification

For the same initially empty symbol, command order, fields and timestamps, the engine
produces identical logical results and FIFO state. Tests serialize explicit integer
fields and trades in order under the classic locale and compare the resulting bytes.
They exclude diagnostic messages, container layout, padding and wall-clock timing.
This is a test representation, not a network protocol or native-struct encoding.

`engine_contract_tests` covers rejection codes, rejection atomicity, both directions
of marketable replacement, multi-level sweeps, priority rules, maximum quantities,
request correlation, CSV boundaries and deterministic replay.

The stateful fuzzer checks every result and resting state against an independent
linear-vector reference implementation, including every trade's price, quantity,
IDs and timestamp. It also audits production container invariants after every
command. Its random generator uses the reference book, so production mutations do not change
the generated input for a seed. It permits crossing Modify operations, duplicate IDs,
unknown IDs, zero quantities, invalid modify prices and over-execution. Symbol and
numeric boundaries are covered by table-driven tests. Sequence minimization keeps
the same failure category; it does not claim to identify the exact same root cause.
