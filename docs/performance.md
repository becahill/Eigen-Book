# Performance Verification

Eigen-Book uses a dependency-free microbenchmark for local latency signals and
a separate allocation-guard test for the zero-allocation hot-path invariant.
Benchmark results are not CI pass/fail criteria.

## Reproducing A Release Run

The supported workflow requires a nonexistent build directory so stale CMake
state or objects cannot affect the result:

```sh
cmake -E remove_directory build-benchmark-release
./scripts/run_benchmarks.sh build-benchmark-release 50000 1 text \
  | tee benchmark-results.txt
```

The positional arguments are build directory, operations per workload, and
complete workload iterations, followed by an optional output format. Operations
must be a multiple of 20 from 20 through 1,000,000; iterations must be from 1
through 100. Format defaults to `text` and may be `text` or `json`.

Use JSON when preserving results for comparison tooling:

```sh
cmake -E remove_directory build-benchmark-json
./scripts/run_benchmarks.sh build-benchmark-json 50000 1 json \
  > benchmark-results.json
python3 -m json.tool benchmark-results.json >/dev/null
```

In JSON mode the script sends CMake configure/build output to stderr and emits
only the benchmark JSON document on stdout.

The script configures a clean Release build with tests, examples, fuzzers, and
Python bindings disabled. In text mode it builds and runs CMake's
`run_benchmarks` target. The equivalent commands are:

```sh
cmake -S . -B build-benchmark-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DEIGENBOOK_BUILD_TESTS=OFF \
  -DEIGENBOOK_BUILD_EXAMPLES=OFF \
  -DEIGENBOOK_BUILD_FUZZERS=OFF \
  -DEIGENBOOK_BUILD_PYTHON=OFF \
  -DEIGENBOOK_BUILD_BENCHMARKS=ON \
  -DEIGENBOOK_BENCHMARK_OPERATIONS=50000 \
  -DEIGENBOOK_BENCHMARK_ITERATIONS=1 \
  -DEIGENBOOK_BENCHMARK_FORMAT=text
cmake --build build-benchmark-release --parallel --target run_benchmarks
```

For machine-readable output from an existing build, run the executable directly:

```sh
cmake --build build-benchmark-release --parallel --target eigenbook_bench
./build-benchmark-release/eigenbook_bench \
  --operations 50000 --iterations 1 --format json \
  > benchmark-results.json
```

The default text output prints its UTC start time, CPU model, OS/kernel,
compiler and path, build type, optimization flags, CMake version/generator,
iteration count, workload sizes, sampling block size, and units before any
result rows. JSON output records the benchmark run context under `context`,
with per-iteration rows under `results`.

## Python Boundary Benchmark

`benchmarks/bench_python_env.py` is intentionally separate from correctness
tests and does not run in CI. Install `eigenbook[benchmark]` from the checkout,
then run it from an otherwise idle machine:

```sh
python -m pip install '.[benchmark]'
python benchmarks/bench_python_env.py --iterations 100000 --warmup 10000
```

The report records hardware, OS, Python, Eigen-Book, NumPy, Gymnasium, compiler,
native build mode, workload, warm-up count, and measured iteration count. It
reports three distinct measurements:

- a trivial Python-to-native round trip with no engine operation;
- command-field updates followed by one bound dispatch and caller-owned
  event-buffer copy;
- one complete Gymnasium step, including reward accounting and two depth
  queries.

The second measurement includes both engine and binding work, so subtracting
the first is not an engine-latency measurement. Engine-only latency remains the
responsibility of the C++ benchmark above. Do not publish Python or C++ timing
numbers without preserving the full emitted context.

## Measurement Method

- Each timed row contains 50,000 operations in the recorded run below.
- Object construction, fixed-capacity engine storage, input vectors, setup
  liquidity, and latency-sample vectors are initialized before timing.
- `std::chrono::steady_clock` measures blocks of 64 operations. Each block's
  duration is divided by its operation count, then block values are sorted
  outside the timed region for p50, p95, and p99.
- Total time covers the operation loop, including per-block clock reads.
  Throughput and average latency are derived from that total.
- Percentiles describe amortized block time, not independently timed
  single-operation latency. The final partial block contains 16 operations
  when the workload size is 50,000.
- Replay commands are encoded before timing. The timed operation decodes one
  39-byte command and dispatches it through `MatchingEngine`.
- Snapshot buffers are allocated and source snapshots are populated before
  timing. One serialize or restore call counts as one operation.
- There is no separate warm-up pass. With multiple iterations, the executable
  prints one complete table per iteration in text mode or one result object per
  iteration in JSON mode.

## Workload Definitions

`N` is the requested operations per workload.

| Scenario | Timed operation and pre-timed state |
|---|---|
| Add N limit orders | Add `N` non-crossing buy GTC orders of quantity 100 at prices 100-149. |
| Dense wide 20 prices | Add `N` buy GTC orders of quantity 100 across 20 fixed prices from 10 to 950,000 in a dense `[1, 1,000,000]` book. |
| Sparse wide 20 prices | Same flow and range as the dense-wide case, using fixed-capacity sparse price-level storage. |
| Cancel N orders | Preload `N` buys at price 100 and quantity 100; cancel each by id. |
| Modify N orders | Preload `N` buys at price 100 and quantity 100; reduce each quantity to 99 without losing priority. |
| Replace N orders | Preload `N` buys at price 100 and quantity 100; replace each at price 101 and quantity 100. |
| Match market orders | Preload `N` sells at price 100 and quantity 1; submit `N` buy market orders of quantity 1. |
| IOC partial matches | Preload one sell of quantity 1 at each price `100 + i`; submit a buy IOC of quantity 2 at that price, producing one fill and cancelling one residual unit. |
| FOK rejects | Submit `N` buy FOK orders of quantity 1 against an empty book. |
| FOK full matches | Preload `N` sells at price 100 and quantity 1; submit executable buy FOK orders of quantity 1. |
| Mixed 50/25/15/10 | Every 20 timed operations contain 10 non-crossing adds, 5 cancels, 3 quantity reductions, and 2 one-unit market matches. Cancel, modify, and match liquidity is preloaded. |
| Replay dispatch commands | Every 20 pre-encoded commands contain 8 adds, 4 cancels, 3 modifies, 3 replaces, and 2 market matches. |
| Serialize book snapshot | Serialize the same 256-order book (128 bids, 128 asks, 20 prices per side, quantity 100) into a caller-owned 64 KiB buffer. |
| Restore book snapshots | Restore dense snapshots containing 64, 256, 1,024, and 4,096 orders into compatible 4,096-order-capacity books. Every order occupies a distinct level; half are bids and half are asks. |

Book capacities are fixed before timing. The normal workloads use `N + 16`
order slots (the mixed/replay workloads reserve additional bounded headroom);
the FOK-reject workload uses 16 slots because no order rests.

## Allocation Guard

`eigenbook_no_allocation_tests` installs test-only replacements for all C++20
global scalar/array `operator new` overloads used by allocation expressions:
throwing, nothrow, aligned throwing, and aligned nothrow. A positive control
calls each of the eight forms and requires all eight calls to be detected. The
aligned implementation retains the original allocation pointer for correct
deallocation and verifies 64-byte alignment.

Every engine fixture, command byte array, expected-status array, and result
object is constructed and warmed before tracking. Tracking surrounds only the
operation under test; it is disabled before any assertion, formatting, state
inspection, or output. Dense and sparse scenarios cover:

- resting insertion, full/partial limit matching, market orders, cancellation,
  quantity reduction, replacement, IOC, and FOK;
- fixed-capacity event recording and wraparound;
- repeated struct and encoded command dispatch across two instruments;
- explicit order-pool, order-id-map, sparse price-level, and event-log
  exhaustion results.

The exact boundary and exclusions are documented in
[`architecture.md`](architecture.md#enforced-zero-allocation-boundary).
Snapshots and read-side/control-plane APIs are not part of this matching-path
guard.

Run only the guard:

```sh
ctest --test-dir build-debug --output-on-failure -L allocation
```

The tracker is process-wide while enabled and the test is single-threaded. It
counts calls routed through replaceable global C++ `operator new` functions; it
does not claim to intercept direct `malloc`/`calloc`/`realloc`, third-party C
allocators, OS allocation APIs, or allocators that bypass global `operator
new`. Direct allocation APIs remain prohibited in engine hot-path code by
`AGENTS.md`.

## Current Local Results

Measured locally with:

```sh
./scripts/run_benchmarks.sh build-benchmark-release-20260626 50000 1 \
  | tee /tmp/eigenbook-benchmark-20260626.txt
```

Run context printed by the executable:

| Item | Recorded value |
|---|---|
| Timestamp | 2026-06-26T20:59:01Z |
| CPU | Apple M4, 10 physical / 10 logical cores |
| Memory | 16 GiB |
| OS | macOS 26.5.1 (25F80); Darwin 25.5.0 arm64 |
| Compiler | AppleClang 21.0.0.21000101; Apple clang build 21.0.0 (clang-2100.1.1.101) |
| Compiler path | `/usr/bin/c++` |
| Build | CMake Release |
| Optimization flags | `-O3 -DNDEBUG -O3 -march=native` |
| CMake | 4.3.2, Unix Makefiles |
| Iterations | 1 complete iteration |
| Workload size | 50,000 operations per row |
| Sampling | 64 operations per latency sample |
| Units | total ms; operations/s; ns/operation |

```text
| Scenario                      | Operations | Total ms   | Ops/sec        | Avg ns   | p50 ns | p95 ns | p99 ns |
|-------------------------------|------------|------------|----------------|----------|--------|--------|--------|
| Add N limit orders            |      50000 |      4.563 |       10957403 |     91.3 |     85 |    108 |    130 |
| Dense wide 20 prices          |      50000 |      4.193 |       11924756 |     83.9 |     73 |     98 |    294 |
| Sparse wide 20 prices         |      50000 |      3.152 |       15861057 |     63.0 |     61 |     74 |     81 |
| Cancel N orders               |      50000 |      1.056 |       47326077 |     21.1 |     20 |     23 |     24 |
| Modify N orders               |      50000 |      0.698 |       71599080 |     14.0 |     13 |     16 |     18 |
| Replace N orders              |      50000 |      2.374 |       21062609 |     47.5 |     46 |     51 |     54 |
| Match market orders           |      50000 |      1.041 |       48011554 |     20.8 |     20 |     22 |     24 |
| IOC partial matches           |      50000 |      8.118 |        6159184 |    162.4 |    164 |    254 |    266 |
| FOK rejects                   |      50000 |      0.486 |      102969008 |      9.7 |      9 |     10 |     10 |
| FOK full matches              |      50000 |      1.774 |       28191520 |     35.5 |     34 |     40 |     42 |
| Mixed 50/25/15/10 workload    |      50000 |      1.266 |       39508795 |     25.3 |     24 |     29 |     31 |
| Replay dispatch commands      |      50000 |      2.164 |       23100461 |     43.3 |     42 |     48 |     50 |
| Serialize book snapshot       |      50000 |    148.165 |         337461 |   2963.3 |   2985 |   3080 |   3173 |
```

These are one local run, not portable latency guarantees.

## Matching-Engine Construction Contract Comparison

The command-dispatch workload was run immediately before and after the
construction-contract change. Both binaries used the same benchmark source,
host, compiler, Release flags, 50,000 operations per workload, and five
complete iterations:

```sh
cmake -S . -B <build-directory> \
  -DCMAKE_BUILD_TYPE=Release \
  -DEIGENBOOK_BUILD_TESTS=OFF \
  -DEIGENBOOK_BUILD_EXAMPLES=OFF \
  -DEIGENBOOK_BUILD_PYTHON=OFF \
  -DEIGENBOOK_BUILD_BENCHMARKS=ON
cmake --build <build-directory> --parallel \
  --target eigenbook_bench
./<build-directory>/eigenbook_bench \
  --operations 50000 --iterations 5
```

| Item | Recorded value |
|---|---|
| CPU / OS | Apple M4; Darwin 25.5.0 arm64 |
| Compiler | AppleClang 21.0.0.21000101 (`clang-2100.1.1.101`) |
| Compiler path | `/usr/bin/c++` |
| CMake | 4.3.2, Unix Makefiles |
| Build/flags | Release; `-O3 -DNDEBUG -O3 -march=native` |
| Before timestamp | 2026-06-26T21:31:57Z |
| After timestamp | 2026-06-26T21:42:06Z |
| Workload | 50,000 pre-encoded mixed commands per iteration |

| Replay dispatch metric | Before | After |
|---|---:|---:|
| Median average latency | 41.3 ns/op | 39.6 ns/op |
| Average-latency range | 38.0-42.0 ns/op | 38.7-40.9 ns/op |
| Median throughput | 24,195,991 ops/s | 25,277,522 ops/s |
| Median block p50 | 41 ns/op | 39 ns/op |

The median average-latency difference was -1.7 ns/op (-4.1%). The valid-engine
dispatch implementation is unchanged and contains no new validity branch; the
apparent improvement is within the variance of this unpinned local
microbenchmark. No CI threshold is derived from these values.

## Snapshot Restore Validation Comparison

Measured before and after the validation refactor with the same benchmark
source workload, Release configuration, compiler, host, flags, and command:

```sh
cmake -S . -B <build-directory> \
  -DCMAKE_BUILD_TYPE=Release \
  -DEIGENBOOK_BUILD_TESTS=OFF \
  -DEIGENBOOK_BUILD_EXAMPLES=OFF \
  -DEIGENBOOK_BUILD_FUZZERS=OFF \
  -DEIGENBOOK_BUILD_PYTHON=OFF \
  -DEIGENBOOK_BUILD_BENCHMARKS=ON
cmake --build <build-directory> --parallel --target eigenbook_bench
./<build-directory>/eigenbook_bench --operations 50000 --iterations 1
```

| Item | Recorded value |
|---|---|
| CPU | Apple M4 |
| OS | Darwin 25.5.0 arm64 |
| Compiler | AppleClang 21.0.0.21000101 (`clang-2100.1.1.101`) |
| Compiler path | `/usr/bin/c++` |
| CMake | 4.3.2, Unix Makefiles |
| Build/flags | Release; `-O3 -DNDEBUG -O3 -march=native` |
| Before timestamp | 2026-06-26T21:15:25Z |
| After timestamp | 2026-06-26T21:26:50Z |
| Workload | Dense `[1, 8192]` book; 4,096 order slots; 8,192 id-map slots; one occupied level per order |
| Sampling | 64 restore operations per latency sample where the row has at least 64 operations |

The benchmark scales iteration count inversely with the square of snapshot
size so the old quadratic implementation remains practical to measure. The
operation counts are identical before and after.

| Live orders / levels | Restore operations | Before avg ns | After avg ns | Speedup |
|---:|---:|---:|---:|---:|
| 64 | 80,000 | 80,537.0 | 32,755.7 | 2.5x |
| 256 | 5,000 | 944,379.1 | 54,018.8 | 17.5x |
| 1,024 | 312 | 14,542,874.3 | 136,718.6 | 106.4x |
| 4,096 | 19 | 236,217,447.4 | 586,614.0 | 402.7x |

The 64-order row is dominated by clearing the same 4,096-order-capacity target
and its configured dense price storage. At larger occupancies, removing the
quadratic validation scans produces the expected scaling improvement. These
single local runs are development evidence, not portable latency guarantees or
CI thresholds.

## Venue, Market-Data, and Recovery Change (2026-06-26)

The production-semantics change was measured locally before and after on the
same checkout and machine:

- Apple M4, Darwin 25.5.0 arm64
- AppleClang 21.0.0.21000101
- CMake 4.3.2, Unix Makefiles
- Release: `-O3 -DNDEBUG -O3 -march=native`
- 20,000 operations per workload, three complete iterations
- 64-operation latency sample blocks

The table uses the mean `Avg ns` of warmed iterations 2 and 3. It is not a CI
threshold.

| Existing workload | Before ns/op | After ns/op | Change |
|---|---:|---:|---:|
| Add N limit orders | 44.75 | 55.15 | +23.2% |
| Cancel N orders | 15.05 | 14.10 | -6.3% |
| Modify N orders | 10.85 | 10.60 | -2.3% |
| Replace N orders | 39.00 | 45.85 | +17.6% |
| Match market orders | 17.85 | 19.70 | +10.4% |
| IOC partial matches | 74.10 | 81.15 | +9.5% |
| FOK rejects | 8.55 | 9.25 | +8.2% |
| FOK full matches | 26.70 | 32.60 | +22.1% |
| Mixed 50/25/15/10 | 19.90 | 23.75 | +19.3% |
| Replay dispatch | 35.90 | 41.90 | +16.7% |
| Serialize 256-order snapshot | 3001.10 | 3567.15 | +18.9% |

The added same-shape scenarios measured 31.25 ns/op for add with lot/STP
checks enabled and 38.10 ns/op for add with market-data emission enabled.
These scenarios run after the ordinary add workload and therefore benefit from
warmer allocator/page/cache state; they are useful absolute workload records,
not valid subtractions from the first scenario.

Disabled market data branches before level/best-quote capture and uses the
legacy matching loop. Cancel and modify are neutral within run noise. Remaining
disabled-feature cost is concentrated in entry/replace preflight, wider order
state (participant/post-only), STP-capable result handling, and the larger v3
snapshot record. The regressions are recorded rather than hidden; a future
optimization pass should use randomized scenario order or process-isolated
fixtures before making stronger causal claims.

## Variance And Limitations

- The process was not pinned to a core. macOS scheduling, background activity,
  thermal state, and dynamic CPU frequency were not controlled.
- `-march=native` intentionally specializes the binary for the build host.
- One recorded iteration is enough to document the current local state, but
  not enough to characterize variance. For comparisons, run at least five
  iterations on an otherwise idle machine:

  ```sh
  ./scripts/run_benchmarks.sh build-benchmark-repeat 50000 5
  ```

  Compare the median of each row's average latency or throughput across
  iterations, and report the full range. Do not average percentile columns.
- Multi-iteration runs share one process; later iterations may benefit from
  warmer instruction/data caches. Separate process runs are appropriate when
  cold-start effects matter.
- Block timing amortizes clock overhead but hides within-block outliers.
- The workloads are deterministic synthetic microbenchmarks, not a production
  exchange feed, multi-thread contention test, or end-to-end network latency
  measurement.
- Snapshot restore includes validation, fixed-capacity clearing, and rebuilding
  64 through 4,096 orders per operation, so it is not directly comparable to a
  single add/cancel operation.
- Linux hardware-counter collection may be run manually with `perf stat`, but
  it is optional and is not part of CI.
- CI explicitly builds `eigenbook_bench` in Release and runs correctness and
  allocation tests. It does not run or enforce machine-dependent latency
  thresholds.
