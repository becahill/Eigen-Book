# Changelog

This project is on the `0.1.x` Alpha development line. The authoritative
version is the CMake `project(VERSION)` value; Python packaging reads that value
instead of maintaining a second version string.

## Unreleased — 0.1.0 Alpha

This hardening candidate remains an experimental matching-engine and simulation
project. It is intended for correctness, bounded-work, recovery, and
performance experiments. It is not presented as a production trading venue,
an execution gateway, or a complete exchange simulator, and its public and
persisted interfaces may change before a stable release.

### Current contracts

- The matching core uses fixed-capacity storage and explicit failure statuses.
  The intrusive FIFO unlink is O(1); complete cancellation also includes
  configured-capacity-bounded hash deletion and dense/sparse level maintenance.
- Native snapshot format v4 is exact-version-only. Older snapshots require an
  explicit migration tool. The CRC-protected journal record remains v1.
- The external policy pipeline accepts a bounded initial snapshot followed by
  continuous sequenced depth updates and optional aggregate trades. It does not
  accept `bookTicker` as depth and does not claim complete L2 fidelity.
- Passive fills use a documented same-price displayed-queue approximation.
  True queue position, hidden liquidity, price-through execution, order-level
  cancellations, market impact, and achievable fills are not modeled.
- Seeds support controlled repeat runs, but PPO parameters, floating-point
  actions, and evaluation rewards are not guaranteed bit-for-bit across
  dependency builds, operating systems, CPU/GPU backends, or hardware.
- Benchmark results are local development evidence, not portable latency
  guarantees. New preserved records must include the Git commit and dirty
  state as well as hardware, compiler, flags, timestamp, and workload.
- Package builds target CPython 3.10–3.14 on macOS and Linux. Windows, PyPy,
  cross-compilation, and portable binary wheels are not currently supported
  claims.

### Compatibility notes

- Snapshot v4 is intentionally incompatible with v3 because it persists each
  live order's public `initial_quantity` and lifecycle `state`.
- Canonical external market-data schema v2 and model metadata v1 reject legacy
  or mismatched artifacts rather than inferring missing source continuity,
  observation, fill-model, venue, or scale information.

## Historical tag note

The published lightweight tag `v1.0.0` points to commit
`8a6329a3c41d92a1a1204aa5197d5477c1626847`. That source declares CMake version
`0.1.0` and predates `pyproject.toml`; the tag therefore does not establish a
1.0/general-availability maturity claim. It is retained unchanged so this
hardening pass does not rewrite a published ref. Deleting or replacing the
local or remote tag requires an explicit maintainer decision.
