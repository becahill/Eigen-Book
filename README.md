# Eigen-Book

Eigen-Book is a C++20 low-latency limit order book and matching engine. The
core is fixed-capacity, header-only, dependency-free, and designed around
deterministic behavior after construction.

## Current Engine Scope

- fixed-capacity order storage through `MemoryPool<Order>`
- intrusive FIFO queues inside each price level
- dense price-indexed bid and ask storage
- occupancy bitsets for bounded best-price discovery
- fixed-capacity open-addressed order-id lookup
- limit order matching with residual resting
- GTC, IOC, and FOK limit order time-in-force semantics
- market order matching
- fixed-capacity event log with order and trade events
- explicit `Status::EventLogFull` rejection when a configured event log cannot
  record a full operation
- fixed-capacity `MatchingEngine` routing across configured instruments
- fixed-size binary `Command` wire format and `MatchingEngine::dispatch`
- best-first market depth API with caller-provided buffers
- deterministic snapshot/restore for books and multi-instrument engines
- O(1) cancellation after id lookup
- quantity reduction that preserves time priority
- quantity increase rejection
- explicit replace-order policy with lose-priority reinsert semantics
- resource-exhaustion preflight that avoids partial execution when residual
  storage cannot be guaranteed

See `docs/architecture.md` for the data-structure and complexity rationale.

## Requirements

- CMake 3.20 or newer
- A C++20-capable compiler

## Build

Configure and build Debug:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

Configure and build Release:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Clang, AppleClang, and GNU-like compilers build with `-Wall -Wextra -Werror`.
Release builds also add `-O3 -march=native` for those compilers.

## Tests

```sh
ctest --test-dir build-debug
ctest --test-dir build-release
```

The `eigenbook_tests` executable uses a slow reference order book as an oracle.
It checks hand-authored edge cases and seeded deterministic command streams
covering:

- FIFO price-time priority
- cancel, reduce, replace, and market execution semantics
- duplicate, unknown, invalid-id, invalid-price, invalid-quantity, and
  id-map-full paths
- pool-exhaustion behavior without accidental partial execution
- IOC residual cancellation and FOK preflight rejection/acceptance
- best bid/ask, per-price depth, per-price order counts, order lookup, and live
  order-count invariants after every operation
- multi-instrument isolation, unknown-instrument rejection, depth ordering, and
  seeded multi-instrument oracle streams

Failing seeded cases are reproducible because the seeds are fixed in
`tests/test_eigenbook.cpp`.

## Event API

Every mutating operation returns a result struct with `events_emitted` and
`events`. `events` is a `std::span<const BookEvent>` over the `OrderBook`'s
internal fixed-capacity `EventLog`; it is valid until the next mutating call on
the same book.

`BookConfig::event_log_capacity == 0` selects the default operation-safe
capacity. A nonzero capacity is treated as an explicit cap. If an operation
would emit more events than that cap, it returns `Status::EventLogFull`, emits
no events, and leaves the book unchanged.

`BookEvent` and nested `TradeEvent` both carry `instrument_id`. Direct
`OrderBook` users get `kInvalidInstrumentId` by default; books owned by
`MatchingEngine` are constructed with their configured instrument id, so the
delegated result spans are already tagged.

```cpp
OrderBook book(BookConfig{90, 110, 16, 64, 1});

const AddOrderResult resting = book.add_limit_order(1, Side::Sell, 100, 5, 10);
const AddOrderResult fill = book.add_limit_order(2, Side::Buy, 100, 5, 11);

for (const BookEvent& event : fill.events) {
    if (event.kind == BookEvent::Kind::Trade) {
        const TradeEvent& trade = event.trade;
        // trade.aggressor_id == 2, trade.resting_id == 1
    }
}
```

Limit orders emit `OrderAccepted`, one `Trade` per resting-order fill, and then
either `OrderResting` for a GTC residual or `OrderCancelled` for an IOC
remainder. FOK orders preflight executable quantity before any book mutation; an
insufficient FOK emits `OrderRejected` with `Status::FokRejected`. Successful
cancels and quantity reductions emit `OrderCancelled` and `OrderModified`.
Replacements that lose priority emit `OrderCancelled`, `OrderAccepted`, any
`Trade` events, and then `OrderResting` for a GTC residual or `OrderCancelled`
for an IOC residual. Rejected commands emit `OrderRejected` with the `Status`
reason. `TimeInForce` supports `Gtc`, `Ioc`, and `Fok`, and existing calls
default to GTC behavior.

## Multi-Instrument Routing And Depth

`MatchingEngine` owns a fixed number of instrument books and a fixed
open-addressed lookup table built at construction. Hot-path routing uses
bounded O(1) lookup by `InstrumentId`; unknown instruments return
`Status::UnknownInstrument` without mutating any book or emitting events.

```cpp
#include "MatchingEngine.hpp"

BookConfig book_config{90, 110, 64, 128, 1};
InstrumentConfig instruments[] = {
    InstrumentConfig{101, book_config},
    InstrumentConfig{202, book_config},
};

MatchingEngine engine(instruments);

static_cast<void>(engine.add_limit_order(101, 1, Side::Buy, 100, 10));
static_cast<void>(engine.add_limit_order(202, 1, Side::Sell, 105, 20));

DepthLevel levels[8]{};
const std::uint32_t written = engine.depth(101, Side::Buy, 8, levels);
const TopOfBook top = engine.top_of_book(101);
```

Depth is per instrument and side. It writes into caller-owned storage and
returns the number of levels written. Bid levels are best-first descending by
price; ask levels are best-first ascending by price. Each `DepthLevel` contains
`price`, `aggregate_quantity`, and `order_count`.

## Production API

| Surface | Contract |
|---|---|
| `MatchingEngine::dispatch(const Command&)` | Single routing entry point for add, cancel, modify, replace, and market commands. Structurally invalid commands return `Status::InvalidCommand`; unknown instruments do not mutate books. |
| `Command` wire format | Fixed 39-byte little-endian record in `include/Command.hpp`: `instrument_id`, `op`, `order_id`, `side`, `price`, `quantity`, `time_in_force`, `timestamp`. `encode`/`decode` return explicit `Status`. |
| Stats | `MatchingEngine::stats()` keeps existing utilization fields and adds dispatch op counters, total rejects, `rejects_by_status`, decode errors, and event-log high-water mark. |
| Snapshot | Snapshot wire format remains version 2 for books and engines. Callers provide storage; corrupt, truncated, mismatched, or undersized buffers return explicit status values. |
| Sparse mode | `PriceLevelMode::Sparse` avoids dense allocation across wide price universes while preserving fixed capacity and bounded behavior. |
| TIF | Limit adds and lose-priority replaces support `Gtc`, `Ioc`, and `Fok`; FOK preflights full quantity before mutation. |
| Replace policy | Same-price reductions keep priority. Price changes and same-price increases use cancel/reinsert semantics and lose priority. |
| Event lifetime | Result `events` spans are owned by the target `OrderBook` and remain valid until the next mutating call on that same book. `EventLogFull` returns an empty span with no mutation. |

## Usage Example

`examples/basic_usage.cpp` is a standalone, compiled example showing how to:

- create an `OrderBook`
- add resting bid and ask orders
- replace a resting order
- execute a market order
- cancel by order id
- inspect best bid and best ask

`examples/snapshot_usage.cpp` shows how to serialize a book into caller-owned
storage, restore into an already constructed empty book, and continue trading.

`examples/replay_usage.cpp` shows a fixed byte-array command replay through
`decode` and `MatchingEngine::dispatch` across two instruments, printing event
summaries and aggregate stats.

Build and run it from any configured build directory:

```sh
cmake --build build-debug --target eigenbook_basic_usage
./build-debug/eigenbook_basic_usage
cmake --build build-debug --target eigenbook_snapshot_usage
./build-debug/eigenbook_snapshot_usage
cmake --build build-debug --target eigenbook_replay_usage
./build-debug/eigenbook_replay_usage
```

IOC and FOK are selected with the optional final `TimeInForce` parameter:

```cpp
OrderBook book(BookConfig{90, 110, 16, 64, 1});

static_cast<void>(book.add_limit_order(1, Side::Sell, 100, 5));

const AddOrderResult ioc = book.add_limit_order(2, Side::Buy, 100, 8, 10, TimeInForce::Ioc);
// ioc.status == Status::PartiallyFilled, ioc.executed_quantity == 5.
// The unfilled quantity is reported with an OrderCancelled event and never rests.

static_cast<void>(book.add_limit_order(3, Side::Sell, 101, 3));

const AddOrderResult fok = book.add_limit_order(4, Side::Buy, 101, 3, 11, TimeInForce::Fok);
// fok.status == Status::Filled only because the full quantity was executable.
```

`replace_order(id, new_price, new_quantity, tif)` keeps FIFO priority only for
same-price reductions. Same-price increases and price changes cancel the old
order and submit the replacement at the tail of the new price level:

```cpp
OrderBook book(BookConfig{90, 110, 16, 64, 1});

static_cast<void>(book.add_limit_order(1, Side::Buy, 99, 10));
static_cast<void>(book.add_limit_order(2, Side::Buy, 100, 10));

const ReplaceResult replaced = book.replace_order(2, 99, 10);
// Order 2 moved behind order 1 at price 99.
```

## Benchmarks

Benchmarks are built by default as `eigenbook_bench`:

```sh
cmake --build build-release --target eigenbook_bench
./build-release/eigenbook_bench
```

The benchmark target is dependency-free and measures add, cancel, modify,
replace, market-match, IOC/FOK limit-order paths, mixed workloads, replay
dispatch, and snapshot workloads. See `docs/performance.md` for methodology,
local recorded results, and limitations.
Do not update benchmark numbers without rerunning locally and recording
hardware/compiler context.

Set `-DEIGENBOOK_BUILD_BENCHMARKS=OFF` to skip building benchmarks.

## Optional Sanitizers

Sanitizers are off by default and can be enabled on Clang/GNU-like compilers:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEIGENBOOK_ENABLE_ASAN=ON
cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug -DEIGENBOOK_ENABLE_UBSAN=ON
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DEIGENBOOK_ENABLE_ASAN=ON -DEIGENBOOK_ENABLE_UBSAN=ON
```

Then run:

```sh
ctest --test-dir build-sanitize
```

## Optional Fuzzing

Clang builds can enable dependency-free libFuzzer harnesses for the command
decoder and snapshot restore path. Each fuzzer is compiled with libFuzzer,
AddressSanitizer, and UndefinedBehaviorSanitizer; fuzzing remains outside the
matching engine and is disabled by default.

```sh
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DEIGENBOOK_BUILD_FUZZERS=ON \
  -DEIGENBOOK_BUILD_BENCHMARKS=OFF \
  -DEIGENBOOK_BUILD_EXAMPLES=OFF
cmake --build build-fuzz --parallel \
  --target eigenbook_command_fuzzer eigenbook_snapshot_fuzzer
```

The build deterministically generates valid command, dense snapshot, and sparse
snapshot seeds under `build-fuzz/fuzz-corpus`. Run longer local campaigns with:

```sh
./build-fuzz/eigenbook_command_fuzzer \
  -max_len=4096 -max_total_time=300 build-fuzz/fuzz-corpus/command
./build-fuzz/eigenbook_snapshot_fuzzer \
  -max_len=4096 -max_total_time=300 build-fuzz/fuzz-corpus/snapshot
```

Re-run a saved crash artifact by passing its path directly to the corresponding
fuzzer. The bounded CI-equivalent smoke runs are registered with CTest:

```sh
ctest --test-dir build-fuzz --output-on-failure -L fuzz
```

## Build Options

- `EIGENBOOK_BUILD_TESTS=ON`: build `eigenbook_tests`
- `EIGENBOOK_BUILD_BENCHMARKS=ON`: build `eigenbook_bench`
- `EIGENBOOK_BUILD_EXAMPLES=ON`: build `eigenbook_basic_usage`
- `EIGENBOOK_BUILD_FUZZERS=ON`: optionally build Clang libFuzzer + ASAN + UBSAN harnesses (default `OFF`)
- `EIGENBOOK_ENABLE_ASAN=OFF`: toggle AddressSanitizer
- `EIGENBOOK_ENABLE_UBSAN=OFF`: toggle UndefinedBehaviorSanitizer

## CI

GitHub Actions is configured in `.github/workflows/ci.yml`. It runs Debug,
Release, and combined ASAN/UBSAN builds, runs the test suite, and compiles the
benchmark target without running benchmark timing in CI. A separate Clang job
builds both fuzzers and executes their fixed 256-run smoke tests.
