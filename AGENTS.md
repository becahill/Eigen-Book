# Eigen-Book Agent Instructions

Eigen-Book is a C++20 low-latency limit order book and matching engine. Treat this repository as a quant developer resume project: correctness, deterministic latency, and clear explanations matter more than cleverness.

## Engineering Rules

- Use C++20 only.
- Put engine code in `namespace eigenbook`.
- Keep public headers focused, documented, and auditable.
- Use `#pragma once` in headers.
- Prefer `std::uint64_t`, `std::int64_t`, and explicit aliases from `Types.hpp`.
- Prefer `noexcept`, `[[nodiscard]]`, explicit types, and cache-conscious structures.
- Use `alignas(64)` where it reduces false sharing or keeps hot objects cache-line aligned.
- Do not use exceptions in hot-path engine code. Return explicit status/result types.
- Avoid RTTI and virtual dispatch in hot-path classes.

## Hot-Path Allocation Rules

- Zero runtime heap allocation on the critical path after initialization.
- No `new`, `delete`, `malloc`, `free`, unbounded `std::vector` growth, `std::map`, or `std::set` in hot-path matching code.
- Use preallocated memory pools for order objects.
- Use intrusive doubly linked lists for FIFO time priority inside each price level.
- Use dense flat arrays or flat price-indexed storage for price levels.
- Avoid hidden allocations from standard containers in the matching core.

## Complexity Rules

- Preserve deterministic O(1) behavior where the design claims O(1).
- Be explicit when an operation is bounded by configured price range or occupancy words.
- Cancellation after order-id lookup must unlink the order in O(1).
- Reducing quantity keeps time priority.
- Increasing quantity is rejected unless a future change explicitly implements lose-priority reinsert semantics with tests.

## Validation Rules

- Tests are required for every meaningful engine change.
- Benchmarks are required for every meaningful performance-sensitive engine change.
- Add or update docs when public behavior, complexity, or matching semantics change.
- Do not claim benchmark numbers unless they were run locally and recorded with hardware/compiler context.
- Do not add external dependencies unless the benefit is clear and documented.

## Build Expectations

- CMake must support Debug and Release builds.
- Strict warnings are required: `-Wall -Wextra -Werror`.
- Release builds use `-O3 -march=native` on Clang/GNU-like compilers.
- Optional sanitizers should remain available through CMake.
- Keep test and benchmark targets easy to run from a clean build directory.
