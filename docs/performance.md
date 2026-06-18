# Performance

This document records the current dependency-free microbenchmark methodology and local results. Do not update benchmark numbers without rerunning `eigenbook_bench`.

## Hardware And Compiler

Local run recorded on 2026-06-16:

| Item | Value |
|---|---|
| CPU | Apple M4 |
| Cores | 10 physical / 10 logical |
| Memory | 16 GiB |
| OS | macOS 26.5.1 (25F80), Darwin 25.5.0 arm64 |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| CMake | 4.3.2, Unix Makefiles |
| Configure | `cmake -S . -B build-codex-release -DCMAKE_BUILD_TYPE=Release` |
| Build | `cmake --build build-codex-release --parallel` |
| CXX | `/usr/bin/c++` |
| CXX flags | `-O3 -DNDEBUG -std=c++20 -arch arm64 -Wall -Wextra -Werror -O3 -march=native` |

## Methodology

The benchmark executable is intentionally simple and dependency-free.

Build and run it from a Release build directory:

```sh
cmake -S . -B build-codex-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-codex-release --target eigenbook_bench
./build-codex-release/eigenbook_bench
```

- Each scenario runs 50,000 operations.
- Synthetic order flow and setup liquidity are generated before the timed region.
- The timed region performs only engine operations and writes latency samples into preallocated vectors.
- Reported latency uses `std::chrono::steady_clock` around fixed 64-operation sample blocks and reports nanoseconds per operation for each block.
- Reported total time and throughput use wall-clock time around the full scenario loop.
- p50, p95, and p99 are computed after sorting samples outside the timed region.

Because timing still has measurement overhead and is not CPU-pinned, the absolute nanosecond numbers should be treated as local development signals, not exchange-grade latency claims.

## Workloads

| Scenario | Description |
|---|---|
| Add N orders | Add non-crossing buy limit orders |
| Cancel N orders | Preload orders, then cancel by id |
| Modify N orders | Preload orders, then reduce quantity |
| Replace N orders | Preload orders, then replace at a new non-crossing price |
| Match market orders | Preload sell liquidity, then execute buy market orders |
| IOC partial matches | Preload sell liquidity, then submit IOC buys with cancelled residual |
| FOK rejects | Submit FOK buys with no executable liquidity |
| FOK full matches | Preload sell liquidity, then submit fully executable FOK buys |
| Mixed workload | 50% adds, 25% cancels, 15% modifies, 10% executions |
| Serialize book snapshot | Serialize a 256-order book into caller-provided storage |
| Restore book snapshot | Restore a 256-order book into an already constructed target |

## Results

Release run from this workspace on the hardware and compiler above. This table
predates the replace, IOC/FOK, and snapshot workload rows, so it should not be
used for those latency claims until `eigenbook_bench` is rerun and the
hardware/compiler context is recorded with the new output.

```text
Eigen-Book microbenchmarks (50000 operations per scenario)
| Scenario                      | Operations | Total ms   | Ops/sec        | Avg ns   | p50 ns | p95 ns | p99 ns |
|-------------------------------|------------|------------|----------------|----------|--------|--------|--------|
| Add N limit orders            |      50000 |     10.460 |        4780305 |    209.2 |    213 |    267 |    337 |
| Cancel N orders               |      50000 |      1.271 |       39333966 |     25.4 |     24 |     28 |     30 |
| Modify N orders               |      50000 |      0.542 |       92265052 |     10.8 |      9 |     14 |     16 |
| Match market orders           |      50000 |      0.937 |       53359401 |     18.7 |     18 |     21 |     23 |
| Mixed 50/25/15/10 workload    |      50000 |      1.321 |       37846532 |     26.4 |     25 |     29 |     31 |
```

## Known Limitations

- The process is not pinned to a core.
- CPU frequency and macOS scheduler effects are not controlled.
- The timer cost is amortized across 64-operation samples.
- There are no Linux `perf` counters yet.
- There is no probe-length histogram for `OrderIdMap` yet.
- The dense price array works best when the configured price range is compact.

## Future Improvements

- custom flat hash map with probe-length metrics
- arena-backed event log
- binary protocol ingestion
- market data replay benchmarks
- lock-free SPSC queue integration boundary
- larger multi-instrument snapshot benchmarks
- add larger configurable batch timing mode
- add occupancy-word crossing counters
- add Linux perf counter support
- add flamegraphs/cache profiling
- add cache-miss and branch-miss profiling
