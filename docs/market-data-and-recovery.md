# Market Data, Checksums, Snapshots, and Recovery

## Incremental market data

`BookConfig::market_data_capacity == 0` disables incremental market data and
preserves the original matching path. A nonzero value creates a fixed-capacity
`MarketDataLog` during initialization. No allocation, locking, I/O, or
formatting occurs while processing a command.

Each instrument has an independent `SequenceNumber`. The first event is 1.
One command can emit zero, one, or many contiguous events. Commands with no
visible book change, including rejected commands, emit no market data and
consume no market-data sequence.

Event kinds are:

- `LevelCreated`
- `LevelQuantityChanged`
- `LevelDeleted`
- `Trade`
- `BestBidChanged`
- `BestAskChanged`

Passive add/cancel/modify ordering is level update, then best update if the best
price, aggregate quantity, or order count changed. Each match fill emits trade,
then level update/deletion, then best update if applicable. A replace emits the
old-level change, match events in execution order, then the new-level change.
STP resting cancellation emits its level and optional best events where it is
encountered in the FIFO sweep.

`OrderBook::last_market_data_events()` exposes the engine-owned span;
`DispatchResult` also carries that span for the unified dispatcher. It is valid
until the next mutating call on that book. Keeping the direct operation result
types unchanged avoids widening every disabled-feature hot-path return object.
`SequenceGapDetector` checks one instrument stream without allocation.
Initialize it with the sequence carried by a snapshot, then require
`SequenceCheck::Contiguous`; `Gap`, `DuplicateOrOld`, and `WrongInstrument` are
explicit outcomes.

## State checksum

`OrderBook::state_checksum()` is canonical 64-bit FNV-1a over explicit
little-endian logical fields. It includes:

- instrument id and semantic price/lot/STP configuration;
- FIFO insertion sequence and current market-data sequence;
- bid levels best-to-worst, then ask levels best-to-worst;
- price, aggregate quantity, order count, and every FIFO order's id, quantity,
  timestamp, sequence, participant id, and post-only flag.

It excludes object bytes, padding, pointers, allocator state, unused capacity,
event-ring contents, and dense/sparse representation. Equivalent dense and
sparse books therefore produce the same checksum.
`MatchingEngine::state_checksum()` combines book checksums in ascending
instrument-id order.

This is a deterministic divergence detector, not a cryptographic
authentication primitive.

## Snapshot contract

Snapshot format version 3 is a deterministic little-endian stream. Book
snapshots include effective venue configuration, participant/post-only order
state, FIFO sequence, command-event sequence, and current market-data sequence.
Restore validates the complete target configuration and complete logical book
before clearing the target. Successful restore emits no events and the next
event continues from each stored sequence.

Version compatibility is exact: a v3 reader rejects all other versions with
`SnapshotVersionMismatch`. Version 2 snapshots are intentionally incompatible
because they lack participant, post-only, market-data sequence, and effective
venue configuration. A migration tool must decode the old contract explicitly;
the engine does not infer missing fields.

## Journal v1 binary record

Every journal record is exactly 144 bytes. Integers are unsigned little-endian;
signed `Price` uses its 64-bit object representation. Records should be written
in the order returned by the matching thread.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII `EBJR` |
| 4 | 2 | version = 1 |
| 6 | 2 | header length = 24 |
| 8 | 4 | record length = 144 |
| 12 | 4 | payload length = 120 |
| 16 | 4 | CRC32 of the whole record with this field treated as zero |
| 20 | 4 | reserved, zero |
| 24 | 8 | caller-assigned global journal sequence |
| 32 | 4 | instrument id |
| 36 | 1 | command op |
| 37 | 1 | side |
| 38 | 1 | time in force |
| 39 | 1 | flags; bit 0 is post-only |
| 40 | 8 | order id |
| 48 | 8 | price |
| 56 | 8 | quantity |
| 64 | 8 | timestamp |
| 72 | 8 | participant id |
| 80 | 1 | expected result status |
| 81 | 7 | reserved, zero |
| 88 | 8 | market-data sequence before |
| 96 | 8 | market-data sequence after |
| 104 | 8 | engine checksum before |
| 112 | 8 | engine checksum after |
| 120 | 4 | command-event count |
| 124 | 4 | market-data event count |
| 128 | 8 | command-event digest |
| 136 | 8 | market-data event digest |

Rejected commands are journaled exactly like accepted commands. They normally
have identical before/after book checksums and market-data sequences, while
their status and rejection event digest remain replay-verifiable.

`decode_journal_record` distinguishes bad magic, unsupported version, bad
length/truncation, CRC failure, and invalid/reserved fields.
`dispatch_and_record` checks caller storage before dispatch, so a short journal
buffer cannot cause an unjournaled mutation.

## Recovery procedure

Full recovery constructs an engine with the original configuration and calls
`replay_journal` from global journal sequence 1. Snapshot-tail recovery:

1. Construct the target engine with an exactly compatible configuration.
2. Restore snapshot v3.
3. Set `JournalReplayCursor::next_journal_sequence` to the first record after
   the snapshot.
4. Replay the journal tail.

Every replayed command verifies the before checksum/sequence, result status,
command-event count and digest, market-data count and digest, after
sequence, and after checksum. Any mismatch returns `ReplayDiverged`.

## Responsibility boundary

The matching engine owns validation, mutation, bounded event/record production,
sequence assignment, checksums, snapshot encoding/restore, and deterministic
replay verification. It performs no file or network I/O.

An external persistence/transport component owns file creation, batching,
flush/fsync policy, replication, retention, snapshot scheduling, record framing
across reads, and operational recovery selection. CRC32 detects accidental
record corruption; authenticated storage or transport is an external concern.
