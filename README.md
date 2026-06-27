# Eigen-Book
[![CI](https://github.com/becahill/Eigen-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/becahill/Eigen-Book/actions/workflows/ci.yml)
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
- configurable lot-size, post-only, participant, and self-trade-prevention rules
- fixed-capacity incremental level/trade/best-quote market data with
  per-instrument sequencing and consumer gap detection
- representation-independent logical state checksums
- versioned CRC-protected journal records and deterministic full or
  snapshot-tail replay
- resource-exhaustion preflight that avoids partial execution when residual
  storage cannot be guaranteed

See [`docs/architecture.md`](docs/architecture.md) for data structures,
[`docs/venue-semantics.md`](docs/venue-semantics.md) for matching rules, and
[`docs/market-data-and-recovery.md`](docs/market-data-and-recovery.md) for
sequence, checksum, snapshot, and journal contracts.

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

The opt-in `eigenbook_stateful_fuzzer` reuses that same oracle and executes each
decoded command against dense and sparse two-instrument engines. It compares
full dispatch results, event payload and ordering, depth, FIFO links, active
order lookup, and live counts after every command. Input-selected checkpoints
are restored into separate engines and replay continues against originals and
restored copies. Field-aware corrupt snapshot mutations must fail without
changing the restore target.

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
open-addressed lookup table built during initialization. Use
`MatchingEngine::create(...)` to receive either a fully initialized engine or a
`MatchingEngineCreateResult` with the exact `MatchingEngineInitError` and
failing configuration index. Hot-path routing uses bounded O(1) lookup by
`InstrumentId`; unknown instruments return `Status::UnknownInstrument` without
mutating any book or emitting events.

```cpp
#include "MatchingEngine.hpp"

#include <cstdio>

BookConfig book_config{90, 110, 64, 128, 1};
InstrumentConfig instruments[] = {
    InstrumentConfig{101, book_config},
    InstrumentConfig{202, book_config},
};

MatchingEngineCreateResult created = MatchingEngine::create(instruments);
if (!created) {
    std::fprintf(stderr,
                 "instrument[%zu]: %s\n",
                 created.config_index,
                 matching_engine_init_error_name(created.error));
    return 1;
}
MatchingEngine& engine = *created.engine;

static_cast<void>(engine.add_limit_order(101, 1, Side::Buy, 100, 10));
static_cast<void>(engine.add_limit_order(202, 1, Side::Sell, 105, 20));

DepthLevel levels[8]{};
const std::uint32_t written = engine.depth(101, Side::Buy, 8, levels);
const TopOfBook top = engine.top_of_book(101);
```

The factory is `noexcept`, reports capacity overflow, invalid instrument ids,
invalid per-book configuration, duplicate ids, and allocation failure, and
never returns a partially configured engine. Empty configuration is supported
and creates a valid zero-instrument engine. Compatibility constructors remain
available; if one rejects configuration, `valid()` is false,
`initialization_error()` and `failed_config_index()` identify the cause, all
instrument state is discarded, commands return `Status::UnknownInstrument`,
reads are empty, and snapshot serialization returns
`Status::InvalidConfiguration`. No validity branch is added to valid-engine
command routing.

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
| Venue command | `VenueCommand` extends the legacy command with participant id and post-only without changing the 39-byte command wire contract. |
| Market data | Per-instrument fixed-capacity events use contiguous sequence numbers. Rejected/no-change commands consume no market-data sequence. |
| Snapshot | Snapshot wire format version 3 preserves venue state plus command-event and market-data sequences. Versions are exactly matched. |
| Journal | `dispatch_and_record` produces one 144-byte little-endian CRC32-protected v1 record; `replay_journal` verifies status, events, sequences, and state checksum after every command. |
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
Snapshot validation uses constructor-preallocated workspace and deterministic
linear radix passes over orders and occupied levels. Restore is an
allocation-free control-plane operation after book construction; workspace
memory and configured-capacity bounds are documented in
[`docs/architecture.md`](docs/architecture.md#snapshot-and-restore).

`examples/replay_usage.cpp` shows a fixed byte-array command replay through
`decode` and `MatchingEngine::dispatch` across two instruments, printing event
summaries and aggregate stats.

`examples/journal_usage.cpp` shows venue-aware bounded record production and
deterministic recovery into a separately constructed engine.

Build and run it from any configured build directory:

```sh
cmake --build build-debug --target eigenbook_basic_usage
./build-debug/eigenbook_basic_usage
cmake --build build-debug --target eigenbook_snapshot_usage
./build-debug/eigenbook_snapshot_usage
cmake --build build-debug --target eigenbook_replay_usage
./build-debug/eigenbook_replay_usage
cmake --build build-debug --target eigenbook_journal_usage
./build-debug/eigenbook_journal_usage
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
replace, market-match, venue-check, market-data, IOC/FOK limit-order paths,
mixed workloads, replay dispatch, and snapshot workloads. See
`docs/performance.md` for methodology,
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
decoder, snapshot restore path, and stateful differential engine behavior. Each
fuzzer is compiled with libFuzzer, AddressSanitizer, and
UndefinedBehaviorSanitizer; fuzzing remains outside the matching engine and is
disabled by default.

```sh
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DEIGENBOOK_BUILD_FUZZERS=ON \
  -DEIGENBOOK_BUILD_BENCHMARKS=OFF \
  -DEIGENBOOK_BUILD_EXAMPLES=OFF
cmake --build build-fuzz --parallel \
  --target eigenbook_command_fuzzer eigenbook_snapshot_fuzzer \
  eigenbook_stateful_fuzzer
```

The build deterministically generates valid command, dense snapshot, sparse
snapshot, and stateful regression seeds under `build-fuzz/fuzz-corpus`. Run
longer local campaigns with:

```sh
./build-fuzz/eigenbook_command_fuzzer \
  -max_len=4096 -max_total_time=300 build-fuzz/fuzz-corpus/command
./build-fuzz/eigenbook_snapshot_fuzzer \
  -max_len=4096 -max_total_time=300 build-fuzz/fuzz-corpus/snapshot
./build-fuzz/eigenbook_stateful_fuzzer \
  -max_len=4096 -max_total_time=300 \
  -artifact_prefix=build-fuzz/fuzz-artifacts/ \
  build-fuzz/fuzz-corpus/stateful
```

Re-run a saved crash artifact by passing its path directly to the corresponding
fuzzer. CI uploads files written under `build-fuzz/fuzz-artifacts`; libFuzzer
also prints the exact artifact path and reproducer bytes. The bounded
CI-equivalent smoke runs are registered with CTest:

```sh
ctest --test-dir build-fuzz --output-on-failure -L fuzz
```

## Optional Static Analysis

Focused `clang-tidy` analysis is opt-in and does not affect normal builds. It
requires `clang-tidy` 18 or newer; Eigen-Book CI is pinned to version 18. The
configured checks cover Clang's core, C++ lifetime, and dead-code analyzers plus
selected dangling-handle, narrowing, undefined-memory, use-after-move, and
unnecessary-copy performance checks. Style-only check families are
intentionally excluded.

```sh
cmake -S . -B build-analysis \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DEIGENBOOK_BUILD_TESTS=OFF \
  -DEIGENBOOK_BUILD_BENCHMARKS=OFF \
  -DEIGENBOOK_BUILD_EXAMPLES=OFF \
  -DEIGENBOOK_ENABLE_CLANG_TIDY=ON \
  -DEIGENBOOK_CLANG_TIDY_EXECUTABLE=clang-tidy
cmake --build build-analysis --target eigenbook_static_analysis
```

## Python Bindings

The installable package uses a `src/eigenbook` layout and builds
`eigenbook._eigenbook` through the same CMake configuration as the C++ targets.
`pyproject.toml` uses scikit-build-core only as the standards-based PEP 517
bridge to CMake, while its pybind11 build requirement supplies the binding
headers discovered by CMake. The package version is read from CMake's
`project(VERSION)`; compiler flags are not repeated in Python packaging
configuration.

Supported package builds are CPython 3.10 through 3.14 on macOS or Linux with a
C++20 Clang/GNU-like compiler. Windows, PyPy, and cross-compiled wheels are not
currently tested. Release builds retain `-O3 -march=native`, so a locally built
wheel requires the build machine's CPU instruction set and is not a portable
binary-distribution artifact.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install .
python -c "import eigenbook; print(eigenbook.__version__)"
```

NumPy is the only core runtime dependency: it provides the fixed-layout event
and depth buffers. Gymnasium is optional. The extras have narrow purposes:

- `eigenbook[rl]` installs Gymnasium for `eigenbook.env`.
- `eigenbook[benchmark]` installs Gymnasium for the Python environment
  benchmark.
- `eigenbook[test]` installs pytest for the Python correctness suite.

Python construction uses the C++ factory contract. Invalid configurations
raise `ValueError`, invalid configuration element types raise `TypeError`, and
native command outcomes remain explicit `Status` values. Python never receives
a partially initialized engine. Python `DispatchResult` values contain copied
scalars, not native event spans.

`dispatch_with_buffer(command, events)` copies the emitted native records into
the caller-owned NumPy array and returns the valid prefix length.
`dispatch_result_with_buffer` performs the same copy and returns the scalar
dispatch result. The buffer must be writable, naturally aligned,
C-contiguous, one-dimensional, use the exact `BOOK_EVENT_DTYPE`, and have at
least `engine.event_buffer_capacity(instrument_id)` entries:

```python
import numpy as np
import eigenbook

events = np.empty(
    engine.event_buffer_capacity(instrument_id),
    dtype=eigenbook.BOOK_EVENT_DTYPE,
)
event_count = engine.dispatch_with_buffer(command, events)
latest_events = events[:event_count]
```

The valid prefix is a copy and remains valid after the engine is mutated or
destroyed. Entries after `event_count` are unchanged and unspecified. Depth is
written into an exact `float32`, C-contiguous `(levels, 2)` caller buffer.
Implicit dtype conversion is disabled. The bindings do not release the GIL.

### Gymnasium Environment

Install and import the optional environment explicitly:

```sh
python -m pip install '.[rl]'
```

```python
import eigenbook
from eigenbook.env import LimitOrderBookEnv

book = eigenbook.BookConfig()
book.min_price = 90
book.max_price = 110
book.max_orders = 64
book.order_id_map_capacity = 128
book.tick_size = 1

instrument = eigenbook.InstrumentConfig()
instrument.instrument_id = 101
instrument.book_config = book

env = LimitOrderBookEnv(
    instrument,
    max_episode_steps=1_000,
    max_abs_inventory=100,
)
observation, info = env.reset(seed=7)
observation, reward, terminated, truncated, info = env.step(
    env.action_space.sample()
)
```

Actions are `[side, centered_price_offset, quantity_code]`. Observations are a
reused `(2, 5, 2)` float32 bid/ask depth buffer. Reward is aggressive executed
quantity minus residual quantity. Quantity code `n` submits `n + 1` configured
lots; when lot-size enforcement is disabled, one lot is one quantity unit.
Inventory counts aggressive buy fills
positively and sell fills negatively; previously resting actions are treated
as book liquidity for this accounting. The inventory boundary terminates an
episode and the step limit truncates it. Calling `step` before `reset` or after
episode completion is an error. `reset(options=...)` accepts
`initial_inventory` and rejects unknown options. Seeded action sampling and
engine transitions are deterministic. Copy returned observations and info
mappings before retaining them.

Importing `eigenbook` does not import Gymnasium. Accessing the environment
without the extra raises an error containing the exact installation command.

### Python Tests And Wheel Validation

Run core and RL tests from an installed package:

```sh
python -m pip install '.[test]'
python -m pytest tests/test_python_bindings.py \
  tests/test_numpy_event_buffer.py tests/test_packaging.py

python -m pip install '.[rl]'
python -m pytest tests/test_eigenbook_env.py
```

Build a local wheel with `python -m build --wheel`. CI installs that wheel into
a fresh virtual environment and runs imports and core tests outside the source
tree.

The Python benchmark is separate from correctness tests:

```sh
python -m pip install '.[benchmark]'
python benchmarks/bench_python_env.py --iterations 100000 --warmup 10000
```

It reports a trivial Python/native round trip, command-field updates plus
binding dispatch and event-copy cost, and the full Gymnasium step separately.
It does not infer engine-only latency; use `eigenbook_bench` for that
measurement.

For direct CMake development, install pybind11 in the selected interpreter and
pass its CMake package directory:

```sh
python -m pip install pybind11 numpy
cmake -S . -B build-python \
  -DCMAKE_BUILD_TYPE=Release \
  -DEIGENBOOK_BUILD_PYTHON=ON \
  -Dpybind11_DIR="$(python -m pybind11 --cmakedir)"
cmake --build build-python --target _eigenbook
cmake --install build-python --component python --prefix staging
```

The final command stages only the native artifact; the PEP 517 build is the
supported way to combine it with the Python sources into an installable wheel.

## Build Options

- `EIGENBOOK_BUILD_TESTS=ON`: build `eigenbook_tests`
- `EIGENBOOK_BUILD_BENCHMARKS=ON`: build `eigenbook_bench`
- `EIGENBOOK_BUILD_EXAMPLES=ON`: build all C++ usage examples
- `EIGENBOOK_BUILD_FUZZERS=ON`: optionally build Clang libFuzzer + ASAN + UBSAN harnesses (default `OFF`)
- `EIGENBOOK_BUILD_PYTHON=ON`: find pybind11 and build `eigenbook._eigenbook` (default `OFF`)
- `EIGENBOOK_ENABLE_ASAN=OFF`: toggle AddressSanitizer
- `EIGENBOOK_ENABLE_UBSAN=OFF`: toggle UndefinedBehaviorSanitizer

## CI

GitHub Actions is configured in `.github/workflows/ci.yml`. It runs Debug,
Release, and combined ASAN/UBSAN builds, runs deterministic tests under the
sanitizers, explicitly runs the zero-allocation hot-path guard in Debug and
Release, and compiles the benchmark target without running benchmark timing in
CI. Installed core-package tests cover CPython 3.10 through 3.14 without
Gymnasium; separate jobs install the RL extra and build/install a wheel in a
clean environment. No job publishes packages. Separate Clang 18 jobs run
focused static analysis and fixed-run libFuzzer smoke tests, preserving any
failing fuzz artifact. The guarded boundary and tracker limitations are
documented in
[`docs/architecture.md`](docs/architecture.md#enforced-zero-allocation-boundary).
