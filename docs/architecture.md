# Architecture

Eigen-Book is organized around fixed-capacity storage and simple hot-path ownership. The matching core does not allocate after construction and does not use tree maps.

The core design goal is deterministic work: operations either complete in O(1), are bounded by fixed table capacity, or are bounded by the configured price-level range. There is no unbounded container growth in matching, cancellation, or modification.

## Order Lifecycle

```text
add_limit_order
  |
  +-- validate id, tick-aligned price, quantity
  +-- check duplicate id and fixed id-map capacity
  +-- if residual storage is exhausted, preflight executable quantity
  +-- match against opposite BookSide while crossed
  +-- if fully filled: return Status::Filled
  +-- if residual remains:
        +-- reserve Order from MemoryPool
        +-- append to same-side PriceLevel FIFO
        +-- insert id -> Order into OrderIdMap

cancel_order
  |
  +-- lookup id in OrderIdMap
  +-- unlink Order from its PriceLevel in O(1)
  +-- erase id
  +-- return Order to MemoryPool
  +-- return Status::Cancelled

modify_order
  |
  +-- lookup id in OrderIdMap
  +-- reject quantity increases
  +-- reduce quantity in place and keep FIFO priority
```

## Memory Pool

`MemoryPool<Order>` owns a fixed array of slots allocated during construction. Allocation pops an index from an internal free list, constructs an `Order` in place, and returns the pointer. Release destroys the object and pushes the slot back onto the free list.

Hot-path properties:

- O(1) allocate and release
- no heap allocation after construction
- stable order pointers
- explicit object lifetime through placement construction and destruction

## Intrusive Price-Level Queues

Each `PriceLevel` stores:

- price
- aggregate resting quantity
- resting order count
- head pointer
- tail pointer

Each `Order` stores `prev`, `next`, and `level` pointers plus timestamp, arrival sequence, side, and state metadata. The intrusive pointers make FIFO append, head execution, and arbitrary cancellation O(1) after lookup.

The fill path updates aggregate quantity exactly once. If a fill reduces the front order to zero, the order is then unlinked with zero remaining quantity, avoiding double-subtraction.

## Order ID Lookup

`OrderIdMap` is a fixed-capacity open-addressed hash table from `OrderId` to `Order*`.

Reasons for this design:

- no runtime allocation
- flat contiguous memory
- predictable ownership
- simple duplicate-id checks

Worst-case lookup probes are bounded by configured table capacity. A future version can add probe-length telemetry or a custom robin-hood table.

Requested id-map capacity is rounded up to a power of two for mask-based indexing.
Requests above the largest representable `std::uint32_t` power of two saturate at
`OrderIdMap::kMaxCapacity` instead of wrapping to zero.

## Price Level Indexing

`BookSide` owns a dense array:

```text
index = (price - min_price) / tick_size
```

This makes level access O(1) for configured integer tick prices. `tick_size` defaults to `1`, and submitted prices must align to `min_price + n * tick_size`. The tradeoff is memory proportional to the configured number of price levels.

## Best Price Maintenance

Each side keeps:

- a cached `best_index_`
- an occupancy bitset over non-empty price levels

When a new level becomes occupied, the cached best can be updated with a constant-time comparison. When the current best level becomes empty, the side scans occupancy words to find the next best. This is deterministic and bounded by the configured price range.

## Matching Flow

For a buy aggressor, the engine matches against asks while:

```text
best_ask <= limit_price
```

For a sell aggressor, it matches against bids while:

```text
best_bid >= limit_price
```

Within each price level, execution always starts at the FIFO head. Fully filled resting orders are removed from the level, erased from the id map, cleared, and returned to the pool.

If fixed order storage or fixed id-map capacity is exhausted, `add_limit_order` first computes whether the incoming limit order can fully execute against currently eligible contra liquidity. If it cannot, the order is rejected before matching, so resource exhaustion does not partially consume resting liquidity. This preflight is deterministic and bounded by eligible occupied levels plus occupancy words crossed.

## Why Tree Maps Are Avoided

`std::map` and `std::set` are intentionally not used in the matching core because they imply pointer-heavy node storage, allocator interaction, branch-heavy traversal, and less predictable cache behavior.

Eigen-Book uses dense arrays and intrusive queues instead. This gives simpler memory ownership, stable pointers, and a clearer latency story for a configured price range.

## Cache-Locality Considerations

The hot objects are laid out to favor predictable access:

- `BookSide` stores `PriceLevel` objects contiguously by price index.
- Occupancy words are stored in a compact flat bitset.
- `OrderIdMap` probes a flat entry array rather than following tree nodes.
- `Order` objects come from a fixed pool, giving stable pointers and avoiding allocator metadata on the hot path.
- `OrderBook`, `BookSide`, `PriceLevel`, `OrderIdMap`, `MemoryPool`, and `Order` use 64-byte alignment where it helps keep hot state cache-line aligned.

The dense price array is a deliberate tradeoff. It is fast and deterministic for a compact configured range, but it should not be used with a huge sparse price universe without changing the price-level representation.

## Modification Semantics

`modify_order(id, new_quantity)` supports quantity reduction only:

- `new_quantity == 0` returns `Status::InvalidQuantity`.
- Unknown IDs return `Status::UnknownOrderId`.
- Unchanged quantity returns `Status::Accepted`.
- Lower quantity updates the resting order in place and keeps FIFO priority.
- Higher quantity returns `Status::QuantityIncreaseRejected`.

Rejecting increases avoids silently granting extra quantity at an old timestamp. A future implementation could support increases by unlinking the order and reinserting it at the tail of the same price level, but that policy needs explicit tests and documentation because it changes time priority.

## Validation Strategy

The CTest target does not only smoke-test compilation. `tests/test_eigenbook.cpp`
contains a slow reference order book backed by ordinary STL containers. That
oracle is intentionally not latency-oriented; it exists to make matching
semantics easy to audit.

The test harness drives both the production `OrderBook` and the reference model
through hand-authored edge cases plus fixed-seed command streams. After every
operation it checks:

- returned status/result fields
- best bid and best ask
- per-price depth and order count
- active order lookup state and remaining quantity
- live order count versus fixed pool occupancy
- absence of a crossed resting book

The seeded streams cover valid and invalid adds, duplicate ids, unknown cancels,
quantity reductions, rejected quantity increases, market orders, crossing limit
orders, fixed id-map capacity, and pool-exhaustion preflight behavior. Seeds are
fixed so any failure is reproducible without external dependencies.
