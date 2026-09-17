# Engine Contract

This document defines the stable in-process boundary that the future wire protocol will use.

## Command envelope

`Event` carries:

- `timestamp_ns`: event timestamp copied to generated trades;
- `type`: ADD, CANCEL, MODIFY, or EXECUTE;
- `order`: order ID, side, price in integer ticks, and quantity;
- `symbol_id`: routing key reserved for the multi-symbol service;
- `sequence`: caller-assigned correlation sequence.

An `OrderBook` instance owns one symbol. It records neither `symbol_id` nor `sequence` in book state. A future router will select the instance using `symbol_id`; `process` echoes `sequence` in the result.

## Result envelope

Every public mutation returns `EngineResult`:

- `accepted`: command success;
- `code`: stable machine-readable result code;
- `message`: diagnostic text for humans;
- `trades`: executions caused by ADD or MODIFY;
- `resting_quantity`: remaining quantity under the command order ID;
- `sequence`: correlation value echoed by `process`.

Consumers must branch on `code`, not `message`.

## Result codes

| Code | Meaning | State change |
|---|---|---|
| `ACCEPTED` | Command applied | Yes, except a no-op quantity reduction to the same value |
| `INVALID_QUANTITY` | Quantity is zero | No |
| `INVALID_PRICE` | Price is not positive | No |
| `DUPLICATE_ORDER_ID` | ADD reuses a live order ID | No |
| `UNKNOWN_ORDER_ID` | CANCEL, MODIFY, or EXECUTE refers to no live order | No |
| `EXECUTION_QUANTITY_EXCEEDS_REMAINING` | External execution is larger than the resting quantity | No |
| `UNSUPPORTED_EVENT_TYPE` | Event type is outside the protocol | No |
| `INTERNAL_INVARIANT_VIOLATION` | Matching reached an impossible internal state | Processing stops; caller should treat the book as unhealthy |

## MODIFY semantics

MODIFY behaves as follows:

1. Validate order existence, price, and quantity without changing state.
2. A same-price quantity reduction (or equal quantity) updates in place and preserves FIFO priority.
3. A price change or quantity increase removes the old order and submits a replacement with the same ID and side.
4. The replacement may match zero or more resting orders.
5. Any remainder rests at the back of its price-level FIFO.

For an accepted MODIFY, the requested new quantity always equals the sum of trade quantities and `resting_quantity`.

## Determinism

Given the same initial state and ordered command sequence, the logical result stream and final book state are deterministic. Wall-clock timing and benchmark measurements are explicitly outside this guarantee.
