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
- market order matching
- O(1) cancellation after id lookup
- quantity reduction that preserves time priority
- quantity increase rejection
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
- cancel, reduce, and market execution semantics
- duplicate, unknown, invalid-id, invalid-price, invalid-quantity, and
  id-map-full paths
- pool-exhaustion behavior without accidental partial execution
- best bid/ask, per-price depth, per-price order counts, order lookup, and live
  order-count invariants after every operation

Failing seeded cases are reproducible because the seeds are fixed in
`tests/test_eigenbook.cpp`.

## Usage Example

`examples/basic_usage.cpp` is a standalone, compiled example showing how to:

- create an `OrderBook`
- add resting bid and ask orders
- execute a market order
- cancel by order id
- inspect best bid and best ask

Build and run it from any configured build directory:

```sh
cmake --build build-debug --target eigenbook_basic_usage
./build-debug/eigenbook_basic_usage
```

## Benchmarks

Benchmarks are built by default as `eigenbook_bench`:

```sh
cmake --build build-release --target eigenbook_bench
./build-release/eigenbook_bench
```

The benchmark target is dependency-free and measures add, cancel, modify,
market-match, and mixed workloads. See `docs/performance.md` for methodology,
local recorded results, and limitations. Do not update benchmark numbers without
rerunning locally and recording hardware/compiler context.

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

## Build Options

- `EIGENBOOK_BUILD_TESTS=ON`: build `eigenbook_tests`
- `EIGENBOOK_BUILD_BENCHMARKS=ON`: build `eigenbook_bench`
- `EIGENBOOK_BUILD_EXAMPLES=ON`: build `eigenbook_basic_usage`
- `EIGENBOOK_ENABLE_ASAN=OFF`: toggle AddressSanitizer
- `EIGENBOOK_ENABLE_UBSAN=OFF`: toggle UndefinedBehaviorSanitizer

## CI

GitHub Actions is configured in `.github/workflows/ci.yml`. It runs Debug,
Release, and combined ASAN/UBSAN builds, runs the test suite, and compiles the
benchmark target without running benchmark timing in CI.
