# Architecture

Eigen-Book is organized around fixed-capacity storage and simple hot-path ownership. The matching core does not allocate after construction and does not use tree maps. Order and trade events are written into a fixed-capacity ring buffer owned by `OrderBook`.

The core design goal is deterministic work: operations either complete in O(1), are bounded by fixed table capacity, or are bounded by the configured price-level range. There is no unbounded container growth in matching, cancellation, or modification.

## Order Lifecycle

```text
add_limit_order
  |
  +-- validate id, tick-aligned price, quantity
  +-- check duplicate id
  +-- if FOK, preflight executable quantity before any mutation
  +-- if GTC residual storage is exhausted, preflight executable quantity
  +-- emit OrderAccepted once the command is accepted for execution
  +-- match against opposite BookSide while crossed
  +-- emit one Trade event per resting-order fill
  +-- if fully filled: return Status::Filled
  +-- if IOC residual remains:
        +-- emit OrderCancelled for the residual
        +-- return Status::PartiallyFilled or Status::NoLiquidity
  +-- if residual remains:
        +-- reserve Order from MemoryPool
        +-- append to same-side PriceLevel FIFO
        +-- insert id -> Order into OrderIdMap
        +-- emit OrderResting

cancel_order
  |
  +-- lookup id once in OrderIdMap and retain the erase token
  +-- unlink Order from its PriceLevel in O(1)
  +-- erase the located id and backward-shift its hash cluster
  +-- return Order to MemoryPool
  +-- emit OrderCancelled
  +-- return Status::Cancelled

modify_order
  |
  +-- lookup id in OrderIdMap
  +-- reject quantity increases
  +-- reduce quantity in place and keep FIFO priority
  +-- emit OrderModified on success

replace_order
  |
  +-- lookup id in OrderIdMap
  +-- validate replacement price, quantity, and time-in-force
  +-- same price and lower or equal quantity:
        +-- update quantity in place and keep FIFO priority
        +-- emit OrderModified
  +-- same price increase or any price change:
        +-- if FOK, preflight executable quantity before any mutation
        +-- if GTC residual storage is exhausted, preflight executable quantity
        +-- unlink old Order and erase id
        +-- emit OrderCancelled for the old resting order
        +-- emit OrderAccepted for the replacement
        +-- match replacement against opposite BookSide while crossed
        +-- emit one Trade event per resting-order fill
        +-- if fully filled: return Status::Filled
        +-- if IOC residual remains:
              +-- emit OrderCancelled for the replacement residual
        +-- if GTC residual remains:
              +-- reset the Order with a new arrival sequence
              +-- append to same-side PriceLevel FIFO
              +-- reinsert id -> Order into OrderIdMap
              +-- emit OrderResting
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

Each `Order` stores `prev`, `next`, and `level` pointers plus timestamp, arrival
sequence, side, and state metadata. The intrusive pointers make FIFO append,
head execution, and the price-level unlink for arbitrary cancellation O(1).
Hash deletion and best/slot maintenance have the separate configured bounds
described below.

The fill path updates aggregate quantity exactly once. If a fill reduces the front order to zero, the order is then unlinked with zero remaining quantity, avoiding double-subtraction.

## Order ID Lookup

`OrderIdMap` is a fixed-capacity robin-hood open-addressed hash table from
`OrderId` to `Order*`.

Reasons for this design:

- no runtime allocation
- flat contiguous memory
- predictable ownership
- simple duplicate-id checks
- bounded probe observability without sampling allocations

Worst-case lookup and erase compaction work is bounded by configured table
capacity. Lookups stop on an empty slot, on a resident whose probe distance is
shorter than the search distance (the Robin Hood early-miss condition), or
after inspecting the full table. Deletions use backward-shift compaction:
displaced entries move one slot toward home until an empty slot or a cluster
boundary is reached. This preserves early-miss termination without persistent
tombstones. `OrderIdMap::stats()` retains the tombstone field for API
compatibility (it is always zero). For an erase, `last_probe_count` and its
fixed histogram account for both lookup probes and entries moved by
compaction. A lookup token lets cancellation and lose-priority replacement
reuse the located slot instead of hashing and probing for the same id twice.
`OrderBook::stats()` exposes those metrics with order-pool utilization;
`MatchingEngine::stats()` aggregates them across configured instruments.

Requested id-map capacity is rounded up to a power of two for mask-based indexing.
Requests above the largest representable `std::uint32_t` power of two saturate at
`OrderIdMap::kMaxCapacity` instead of wrapping to zero.

## Price Level Indexing

`BookConfig::price_level_mode` selects the price-level representation:

- `PriceLevelMode::Dense` is the default and preserves the original compact
  range design.
- `PriceLevelMode::Sparse` is for wide configured price universes with few
  occupied prices.

In dense mode, `BookSide` owns a dense array:

```text
index = (price - min_price) / tick_size
```

This makes level access O(1) for configured integer tick prices. `tick_size` defaults to `1`, and submitted prices must align to `min_price + n * tick_size`. The tradeoff is memory proportional to the configured number of price levels.

In sparse mode, `BookSide` owns fixed storage for at most `max_orders` occupied
price levels on each side. It uses:

- a fixed robin-hood price-to-level table for price lookup
- a fixed sorted slot index over occupied prices
- the same intrusive `PriceLevel` FIFO queues used by dense mode

Sparse price lookup is average O(1) and worst-case bounded by the fixed
level-map capacity. Best-price discovery and advancing to the next occupied
price are O(1) array reads through the sorted slot index. Creating or removing
an occupied price shifts the sorted slot index and is O(occupied levels), bounded
by `max_orders`. Sparse mode avoids memory proportional to the full price
universe, but it is not intended to beat dense mode when nearly every configured
tick is occupied.

## Best Price Maintenance

Dense mode keeps:

- a cached `best_index_`
- an occupancy bitset over non-empty price levels

When a new level becomes occupied, the cached best can be updated with a constant-time comparison. When the current best level becomes empty, the side scans occupancy words to find the next best. This is deterministic and bounded by the configured price range.

Sparse mode keeps a cached best slot plus a sorted occupied-level slot array.
When the current best level becomes empty, the next best is read directly from
the sorted array. The bounded work in sparse mode is paid when creating or
removing occupied levels, not while sweeping from best to next during matching.

## Cancellation Complexity

The intrusive FIFO unlink is O(1), but that is only one component of a complete
cancellation. Let:

- `C` be the normalized `OrderIdMap` capacity;
- `H = min(C, max_orders)` be the maximum number of live entries that can
  occupy one id-map cluster;
- `L` be the dense configured price-level count and `W = ceil(L / 64)` its
  occupancy-word count;
- `K` be the number of occupied sparse levels before cancellation
  (`K <= max_orders`); and
- `M` be the sparse price-to-level map capacity reported by
  `BookSideStats::level_map_capacity`.

The retained erase token prevents a second id lookup. For a successful erase,
the lookup path through the target and the backward-shift path after the target
are disjoint portions of one Robin Hood cluster, including when that cluster
wraps around the table. Their combined work is therefore bounded by `H`
inspected-or-moved entries, plus constant bookkeeping. This bound is tight: a
same-home cluster of `H` entries reaches it when its first entry is erased.

Dense cancellation computes the level index and performs the intrusive unlink
in constant time. If the order was the last order at the current best price,
best-price maintenance scans at most `W` occupancy words; otherwise it is
constant time. The complete dense cancellation is therefore `O(H + W)` (and
hence `O(C + W)`), with at most `H + W` id-map entries or occupancy words
inspected or moved, plus constant work.

Sparse cancellation probes at most `M` price-map entries to locate the level,
then unlinks in constant time. If the order was the level's last order, erasing
the already-located price-map entry is constant time and removing its sorted
slot shifts at most `K - 1` entries; the best slot is then refreshed in constant
time. The complete sparse cancellation is therefore `O(H + M + K)` (and hence
`O(C + M + K)`), bounded by `H + M + (K - 1)` id/price entries or sorted slots
inspected or moved, plus constant bookkeeping. Sparse tombstones can make a
negative price lookup inspect all `M` slots, but do not change that
fixed-capacity bound.

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

If fixed order storage or fixed id-map capacity is exhausted, a GTC
`add_limit_order` first computes whether the incoming limit order can fully
execute against currently eligible contra liquidity. If it cannot, the order is
rejected before matching, so resource exhaustion does not partially consume
resting liquidity. This preflight is deterministic and bounded by eligible
occupied levels plus dense occupancy words crossed; in sparse mode traversal is
bounded by eligible occupied levels.

Lose-priority `replace_order` paths preflight residual storage before cancelling
the old order. Because replacement reuses the existing `Order` slot and
`OrderIdMap` entry, a full order pool or full id map does not by itself reject a
GTC replacement that can rest its residual in that reused storage. If the
destination price level cannot store the residual, the replacement emits
`OrderRejected` and the original order remains unchanged.

## Time In Force

`add_limit_order` accepts an optional `TimeInForce` argument. The default is
`TimeInForce::Gtc`, preserving existing add-order behavior for older call sites.

GTC orders match eligible contra liquidity and rest any residual quantity. If a
residual might need to rest but the fixed `MemoryPool<Order>` or `OrderIdMap`
cannot accept another entry, the engine uses the bounded executable-quantity
scan before matching. A fully executable order may still run while the pool or id
map is full because it will not need residual storage.

IOC orders match eligible contra liquidity immediately and cancel any unfilled
remainder. The residual never allocates an `Order` and is never inserted into
`OrderIdMap`; the event stream reports that remainder as `OrderCancelled`.
Because IOC has no resting residual, pool and id-map exhaustion do not reject it,
although duplicate active order ids are still rejected.

FOK orders use the same bounded executable-quantity scan as a preflight before
emitting `OrderAccepted` or matching. If the full requested quantity is not
available at eligible prices, the order returns `Status::FokRejected`, emits
`OrderRejected`, and leaves resting orders unchanged. If the preflight succeeds,
the order fully fills and never rests, so pool and id-map residual capacity are
not required.

For `replace_order`, IOC and FOK apply only to replacements that lose priority:
same-price quantity reductions keep the existing resting order and do not run
matching. A lose-priority IOC replacement cancels the old order, executes any
eligible contra liquidity, and cancels the replacement residual without resting.
A lose-priority FOK replacement preflights the full replacement quantity before
cancelling the old order; insufficient liquidity returns `Status::FokRejected`
with the original order still resting.

## Multi-Instrument Engine

`MatchingEngine` is a routing layer over independent `OrderBook` instances. It
is configured with a fixed `max_instruments` and `InstrumentConfig` array. Each
`InstrumentConfig` contains an `InstrumentId`, a `BookConfig`, and optional
tick/lot metadata fields for callers that want to carry venue or symbol
metadata beside the book configuration.

`MatchingEngine::create(...)` is the primary construction API. Its
`MatchingEngineCreateResult` owns a non-null engine only on complete success.
On failure, `error` contains one of:

- `InstrumentCapacityExceeded`
- `InvalidInstrumentId`
- `InvalidBookConfiguration`
- `DuplicateInstrumentId`
- `AllocationFailure`
- `InternalInsertionFailure`

`config_index` identifies the first failing entry, or is
`kInvalidInstrumentConfigIndex` when no entry is responsible. Empty
configuration is explicitly supported as a valid zero-instrument engine.

Initialization first validates the complete configuration, including a bounded
cold-path duplicate-id scan, before allocating instrument arrays or
constructing any `OrderBook`. It then builds the fixed routing state. If that
second phase fails, all constructed books and lookup entries are destroyed.
The `noexcept` factory catches allocation failure and returns it explicitly, so
it never exposes a partial engine.

Direct constructors remain for source compatibility. A configuration failure
leaves such an instance strictly fail closed: `valid()` is false,
`instrument_count()` is zero, `initialization_error()` and
`failed_config_index()` retain the diagnostic, and no instrument can be found.
Commands targeting it therefore return `Status::UnknownInstrument`; read
surfaces return their documented empty values, and engine snapshot
serialization returns `Status::InvalidConfiguration`.

Construction allocates:

- one fixed instrument-state array
- one fixed open-addressed lookup table from `InstrumentId` to instrument index
- one `OrderBook` per configured instrument

No instruments are inserted or erased on the hot path, so routing does not grow
containers after construction. Lookup is O(1) average and bounded by the fixed
lookup-table capacity. Unknown instruments return `Status::UnknownInstrument`
from mutating APIs and do not emit events or touch any configured book.
Valid-engine routing contains no initialization-validity branch.

Order ids are scoped per instrument because each instrument owns its own
`OrderBook` and `OrderIdMap`. The same `OrderId` can rest simultaneously on two
different instruments without collision.

`MatchingEngine` delegates:

- `add_limit_order(instrument_id, ...)`
- `cancel(instrument_id, ...)`
- `modify(instrument_id, ...)`
- `replace(instrument_id, ...)`
- `match_market_order(instrument_id, ...)`

The underlying book event logs remain per instrument. `OrderBook` constructs its
`EventLog` with the configured `InstrumentId`, so every returned `BookEvent` and
nested `TradeEvent` is tagged without rewriting result spans in the router. The
span lifetime is still per underlying book: it remains valid until the next
mutating operation on that same instrument book.

## Command Wire Format And Dispatch

`include/Command.hpp` defines a fixed-size binary command record for replay and
gateway-style ingestion. The struct is packed for auditability, while
`encode`/`decode` write and read an explicit little-endian byte stream. The wire
size is `kCommandWireSize == 39` bytes.

| Offset | Size | Field | Type |
|---:|---:|---|---|
| 0 | 4 | `instrument_id` | `u32` |
| 4 | 1 | `op` | `CommandOp` (`Add`, `Cancel`, `Modify`, `Replace`, `Market`) |
| 5 | 8 | `order_id` | `u64` |
| 13 | 1 | `side` | `Side` |
| 14 | 8 | `price` | `i64` bits |
| 22 | 8 | `quantity` | `u64` |
| 30 | 1 | `time_in_force` | `TimeInForce` |
| 31 | 8 | `timestamp` | `u64` |

`decode` validates the enum fields and returns `Status::InvalidCommand` for
unknown op, side, or time-in-force values. Truncated buffers return
`Status::BufferTooSmall`; no partially decoded command is dispatched.

Dispatch flow:

```text
encoded bytes
  |
  +-- decode fixed 39-byte command
  |     +-- BufferTooSmall / InvalidCommand -> reject, no book mutation
  |
  +-- MatchingEngine::dispatch(command)
        +-- validate command enum fields
        +-- count dispatch/op attempt
        +-- fixed instrument lookup
        +-- route by CommandOp:
              Add     -> add_limit_order
              Cancel  -> cancel
              Modify  -> modify
              Replace -> replace
              Market  -> match_market_order
        +-- copy operation result into DispatchResult
        +-- update reject counters and event high-water mark
```

`DispatchResult` is a common wrapper over the existing operation result fields.
Its event span has the same lifetime as direct API results: it is owned by the
target instrument's `OrderBook` and remains valid until the next mutating call on
that same book.

## Snapshot And Restore

`Snapshot.hpp` provides warm-path recovery helpers:

- `serialize(const OrderBook&, std::span<std::byte>)`
- `serialize(const MatchingEngine&, std::span<std::byte>)`
- `restore(OrderBook&, std::span<const std::byte>)`
- `restore(MatchingEngine&, std::span<const std::byte>)`

The caller owns the fixed byte buffer. Serialization returns
`SnapshotWriteResult{status, bytes_written}` and returns
`Status::BufferTooSmall` without allocating if the buffer cannot hold the full
snapshot. Restore validates the complete buffer before rebuilding fixed storage
and rejects corrupt, truncated, version-mismatched, or configuration-mismatched
data with explicit `Status` values.

Validation also rejects crossed resting books, duplicate non-saturated arrival
sequences, and FIFO records whose arrival sequences move backward at one price.
These checks run before the destination is cleared, so malformed snapshots
return an explicit status without changing the preconfigured book.

Let `n` be the serialized order count and `l` the serialized occupied-level
count. The previous validator scanned all preceding orders for every order,
scanned all preceding levels and all orders for every level, then scanned all
levels for every order. Its validation cost was
`O(n² + n*l + l²)`, which is `O(n²)` for valid snapshots because `l <= n`.

Current validation uses the target book's preallocated
`SnapshotValidationWorkspace`:

1. Stable eight-pass radix sorts detect duplicate 64-bit order IDs and
   duplicate non-saturated 64-bit arrival sequences.
2. A stable radix sort groups orders by `(side, price index)`, verifies FIFO
   sequence order, and computes checked quantity and order-count aggregates.
3. A radix sort of serialized levels detects duplicate levels and permits one
   linear comparison against the order-derived aggregates.
4. A final stable radix sort records the restore order by side and ascending
   price. FIFO order within a level is retained, and sparse storage can append
   new sorted price slots without repeated shifts.

The radix key width and digit table are fixed, so validation is
`O(n + l)` deterministic work rather than an expected hash-table bound. No
allocation-backed associative container is used.

Each constructed `OrderBook` reserves three 24-byte workspace records per
configured order slot: `72 * config.max_orders` bytes of heap storage, plus a
1 KiB radix counter table and small pointer/counter fields in the book object.
The workspace is allocated once during construction and reused by every
restore. It does not change the snapshot wire format or public restore API.

Validation is linear in snapshot occupancy, but a complete restore also clears
fixed-capacity destination storage and rebuilds its fixed hash maps. Dense clear
work is bounded by configured order slots, id-map capacity, and the configured
price-level range. Sparse clear work is bounded by configured order slots and
sparse map capacity. Hash probes during rebuild remain bounded by those fixed
map capacities. Thus restore is a configured-capacity-bounded control-plane
operation; it is not part of the matching hot path.

Snapshots are not matching hot-path operations. They are intended for recovery,
replay checkpoints, and deterministic test or simulator handoff. The
implementation performs no heap allocation during serialize or restore after the
target `OrderBook` or `MatchingEngine` has been constructed; restore rebuilds
through the existing fixed `MemoryPool`, flat price levels, and fixed id map.

Book snapshot format version `4` is a little-endian byte stream:

```text
BookSnapshot
  magic: "EBOK"
  version: u8 = 4
  reserved: 3 bytes = 0
  instrument_id: u32
  BookConfig:
    min_price: i64 bits
    max_price: i64 bits
    max_orders: u32
    order_id_map_capacity: u32
    tick_size: i64 bits
    event_log_capacity: u32
    price_level_mode: u8
    lot_size: u64
    self_trade_policy: u8
    market_data_capacity: u32
  live_order_count: u32
  level_aggregate_count: u32
  next_sequence: u64
  event_next_sequence: u64
  market_data_sequence: u64
  live orders, in deterministic book/FIFO order:
    id: u64
    side: u8
    price: i64 bits
    quantity: u64
    timestamp: u64
    sequence: u64
    participant_id: u64
    post_only: u8
    initial_quantity: u64
    state: u8
  level aggregates:
    side: u8
    price: i64 bits
    aggregate_quantity: u64
    order_count: u32
```

`next_sequence` is the next resting-order sequence state owned by `OrderBook`.
`event_next_sequence` is also stored so event sequence numbers continue
deterministically after restore. Restore itself emits no events and clears
`last_events()`. `market_data_sequence` provides the consumer checkpoint and
the next incremental event continues at that value plus one.

The maximum live orders in a book snapshot is the book's normalized
`config.max_orders`, and the order count must also fit the normalized fixed
`OrderIdMap` capacity. The maximum level aggregate count is bounded by
`2 * config.price_level_count()` in dense mode, because each side can occupy
each configured price at most once. In sparse mode it is bounded by
`2 * config.max_orders`, matching the fixed sparse level storage. Restore
rejects snapshots whose order or level counts exceed those limits. Since every
serialized level must have at least one matching order, accepted snapshots also
require `level_count <= order_count`.

Intrusive pointers, pool indices, occupancy words, cached best prices, and
sparse-map slots are intentionally not serialized. Restore reconstructs them
from validated order and level records; aggregate, uniqueness, crossed-book,
and FIFO checks prevent corrupt logical records from producing inconsistent
derived metadata.

`Order::initial_quantity` and `Order::state` are persistent public logical
state. `initial_quantity` is the quantity with which the order most recently
joined its resting FIFO (after any aggressive execution), and does not change
on later fills or reductions. Live orders restore as their recorded `Resting`
or `PartiallyFilled` state. A `Resting` order may have been explicitly reduced,
so `initial_quantity >= quantity`; `PartiallyFilled` requires the strict
`initial_quantity > quantity` relationship. Both fields participate in
`state_checksum()`.

Engine snapshots wrap a fixed-capacity array of instrument snapshots:

```text
EngineSnapshot
  magic: "EBEN"
  version: u8 = 4
  reserved: 3 bytes = 0
  max_instruments: u32
  instrument_count: u32
  valid: u8 = 1
  reserved: 3 bytes = 0
  repeated instrument_count times:
    InstrumentConfig
    BookSnapshot
```

Engine restore requires the target engine to be constructed with the same
`max_instruments`, instrument count, instrument order, and instrument
configuration. This keeps restore allocation-free and preserves the fixed
instrument lookup topology.

The complete versioning and journal binary contracts are specified in
[`market-data-and-recovery.md`](market-data-and-recovery.md).

## Market Depth

`DepthLevel` exposes the aggregated visible state of one price level:

- `price`
- `aggregate_quantity`
- `order_count`

`OrderBook::depth(side, max_levels, out_buffer)` and
`MatchingEngine::depth(instrument_id, side, max_levels, out_buffer)` write into
caller-owned memory and return the number of levels written. They do not allocate
or grow containers. Bids are emitted best-first in descending price order; asks
are emitted best-first in ascending price order.

The traversal starts at the cached best index and advances through the
occupancy bitset. Work is bounded by the requested `max_levels` and the occupied
price words crossed while finding the next non-empty level.

`top_of_book(instrument_id)` returns both best quotes for a configured
instrument. Unknown instruments return `Status::UnknownInstrument` with invalid
bid and ask quotes.

## Event Model

`OrderBook` owns an `EventLog`, a construction-time allocated ring buffer of
`BookEvent` entries. `BookConfig::event_log_capacity == 0` selects the default
operation-safe capacity of at least `max_orders + 2`, which is enough for the
largest single operation:

- one `OrderAccepted` event
- one `Trade` event per live resting order that can be filled
- one terminal residual event: `OrderResting` for GTC or `OrderCancelled` for
  IOC
- for replace, one old-order `OrderCancelled` event plus at most `max_orders - 1`
  trade events, because the replaced order is removed before matching

A nonzero `event_log_capacity` is treated as an explicit cap. Eigen-Book uses
policy A for overflow: if a mutating operation would emit more events than the
configured cap can hold, it returns `Status::EventLogFull`, emits no events, and
does not mutate the book. The preflight happens after validation but before the
first state change. The work is bounded by the same eligible resting orders that
matching may touch because the preflight counts the exact fills that would emit
trade events.

Every mutating API call starts a new event operation. The returned result struct
contains:

- `events_emitted`: number of events emitted by the operation
- `events`: a `std::span<const BookEvent>` over internal log storage

The span is valid until the next mutating call on the same `OrderBook`. It does
not own memory; callers that need longer retention should copy the events before
submitting another command.

This lifetime rule includes direct calls rejected before normal command-event
emission: they clear or replace both the prior command-event span and the prior
market-data span. Invalid encoded commands, invalid unified command enums, and
unknown instruments do not route to an `OrderBook`, so they do not invalidate
any configured book's spans.

Event sequencing is monotonic per `EventLog`. Each `BookEvent` has a sequence,
and trade events copy that same sequence into the nested `TradeEvent`.
`BookEvent` and `TradeEvent` both carry `instrument_id`. Direct `OrderBook`
instances use `kInvalidInstrumentId` unless constructed with an instrument id;
books owned by `MatchingEngine` use their configured id. `TradeEvent` records
the incoming aggressor id, filled resting order id,
aggressor side, execution price, execution quantity, timestamp, and sequence.
Limit-order trades use the incoming limit id as `aggressor_id`. Market orders
default to `kInvalidOrderId` unless the optional market aggressor id is passed.

The sole unsequenced command event is the synchronous `OrderRejected` returned
for `Status::PriceLevelQuantityOverflow`. It uses reserved sequence zero so an
aggregate-overflow add or lose-priority replacement can report its exact cause
while leaving FIFO, command-event, and market-data sequence checkpoints
unchanged. The preflight runs before `OrderAccepted`, matching, order
allocation, or removal of the replaced order.

Accepted limit orders emit events in deterministic order:

```text
OrderAccepted
Trade...        (zero or more)
OrderResting    (GTC residual only)
OrderCancelled  (IOC residual only)
```

FOK orders either reject before `OrderAccepted` with `Status::FokRejected` or
emit only `OrderAccepted` plus the full-fill `Trade` events. Market orders emit
`Trade` events for fills. Cancel and modify operations emit `OrderCancelled` and
`OrderModified` respectively on success. Rejected add, cancel, modify, replace,
and invalid market commands emit `OrderRejected` with the `Status` reason.

Lose-priority replacements emit:

```text
OrderCancelled  (old resting order)
OrderAccepted   (replacement)
Trade...        (zero or more)
OrderResting    (GTC residual only)
OrderCancelled  (IOC residual only)
```

Same-price reductions and no-op replacements emit `OrderModified` and keep the
original order timestamp and arrival sequence. Lose-priority replacements use
the replacement call timestamp for emitted events and, if a GTC residual rests,
store that timestamp on the new resting order with a fresh internal arrival
sequence.

## Enforced Zero-Allocation Boundary

After the relevant `OrderBook`, `MatchingEngine`, fixed-capacity component, and
caller-owned command/result storage have been constructed and warmed, the
automated allocation guard defines the matching hot path as:

- dense and sparse resting limit insertion, full/partial limit matching, market
  matching, cancellation by order id, quantity reduction, and replacement;
- dense and sparse IOC no-liquidity, partial-fill, and full-fill paths;
- dense and sparse FOK rejection and full execution;
- fixed-capacity order and trade event emission, including ring wrap;
- lot-size, post-only, and participant-aware STP paths;
- fixed-capacity incremental market-data emission and capacity rejection;
- bounded journal-record generation and replay verification;
- `MatchingEngine::dispatch(const Command&)` and encoded-command decode plus
  dispatch across dense and sparse instruments;
- successful repeated command sequences and explicit rejection at order-pool,
  order-id-map, sparse price-level, event-log, and market-data capacity.

All command bytes, expected statuses, and result storage exist before tracking.
The measured region contains only the operation or repeated operation sequence.
Allocation counting is disabled before assertions, diagnostics, state
inspection, or test output.

Construction, destruction, factory initialization, depth/top-of-book queries,
statistics, Python bindings, examples, benchmarks, and external persistence are
outside this enforced hot-path boundary. Snapshot serialization uses
caller-owned storage; restore and journal replay are separately covered by the
allocation guard after target construction.

The test-only tracker replaces the C++20 global scalar and array `operator new`
forms, including aligned and nothrow overloads. It preserves over-alignment,
uses `std::new_handler` for throwing forms, throws `std::bad_alloc` on terminal
failure, and returns null from nothrow forms. It counts calls routed through
those C++ allocation functions while enabled. It does not intercept direct
`malloc`, `calloc`, `realloc`, third-party C allocators, OS allocation APIs, or
allocator implementations that bypass replaceable global `operator new`.

## Why Tree Maps Are Avoided

`std::map` and `std::set` are intentionally not used in the matching core because they imply pointer-heavy node storage, allocator interaction, branch-heavy traversal, and less predictable cache behavior.

Eigen-Book uses dense arrays and intrusive queues instead. This gives simpler memory ownership, stable pointers, and a clearer latency story for a configured price range.

## Cache-Locality Considerations

The hot objects are laid out to favor predictable access:

- Dense `BookSide` stores `PriceLevel` objects contiguously by price index.
- Dense occupancy words are stored in a compact flat bitset.
- Sparse `BookSide` stores fixed `PriceLevel`, metadata, hash table, and sorted
  occupied-slot arrays.
- `OrderIdMap` probes a flat entry array rather than following tree nodes.
- `Order` objects come from a fixed pool, giving stable pointers and avoiding allocator metadata on the hot path.
- `OrderBook`, `BookSide`, `PriceLevel`, `OrderIdMap`, `MemoryPool`, and `Order` use 64-byte alignment where it helps keep hot state cache-line aligned.

The dense price array is a deliberate tradeoff. It is fast and deterministic
for a compact configured range. Sparse mode trades bounded O(occupied levels)
level insertion/removal for much lower memory use when `min_price..max_price`
is wide and only a small number of ticks are occupied.

## Modification And Replace Semantics

`modify_order(id, new_quantity)` supports quantity reduction only:

- `new_quantity == 0` returns `Status::InvalidQuantity`.
- Unknown IDs return `Status::UnknownOrderId`.
- Unchanged quantity returns `Status::Accepted`.
- Lower quantity updates the resting order in place and keeps FIFO priority.
- Higher quantity returns `Status::QuantityIncreaseRejected`.

`modify_order` remains as a narrow reduce-only API. `replace_order(id,
new_price, new_quantity, tif)` is the explicit API for changes that may lose
time priority:

- Unknown IDs return `Status::UnknownOrderId`.
- `new_quantity == 0` returns `Status::InvalidQuantity` for known orders.
- Same price plus lower quantity updates the order in place and keeps FIFO
  priority. Tests assert this is event- and state-equivalent to
  `modify_order(id, lower_quantity)`.
- Same price plus unchanged quantity returns `Status::Accepted`, emits
  `OrderModified`, and leaves the book unchanged.
- Same price plus higher quantity, or any price change, cancels the old order
  and submits the replacement at the FIFO tail of the new price level.
- IOC and FOK are applied to lose-priority replacements as they are to new limit
  orders at the replacement price and quantity.
- GTC replacements reuse the existing order slot and id-map entry for any
  residual. Residual-storage failures still reject before the old order is
  cancelled, leaving the book unchanged.

Rejecting quantity increases through `modify_order` avoids silently granting
extra quantity at an old timestamp. Callers that want increase-or-price-change
behavior must opt in to `replace_order`, where the loss of priority is explicit.

## Validation Strategy

The CTest target does not only smoke-test compilation. `tests/test_eigenbook.cpp`
contains a slow reference order book backed by ordinary STL containers. That
oracle is intentionally not latency-oriented; it exists to make matching
semantics easy to audit.

The test harness drives both the implementation `OrderBook` and the reference
model through hand-authored edge cases plus fixed-seed command streams. After
every operation it checks:

- returned status/result fields
- emitted event streams, including sequence, kind, status, trade attribution,
  price, quantity, and timestamp
- best bid and best ask
- best-first depth API output
- per-price depth and order count
- active order lookup state and remaining quantity
- live order count versus fixed pool occupancy
- absence of a crossed resting book

The seeded streams cover valid and invalid adds with randomized time-in-force,
duplicate ids, unknown cancels, quantity reductions, rejected quantity
increases, replacements with randomized time-in-force, market orders, crossing
limit orders, fixed id-map capacity, FOK preflight rejection, IOC residual
cancellation, and pool-exhaustion preflight behavior. Seeds are fixed so any
failure is reproducible without external dependencies.

Multi-instrument tests run the same style of oracle with one reference
`OrderBook` per instrument. They cover independent order-id namespaces,
unknown-instrument rejection without side effects, depth after adds, cancels,
and market matches, and fixed-seed mixed order flow routed across instruments.
