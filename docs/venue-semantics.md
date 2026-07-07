# Venue Semantics

Eigen-Book keeps its original behavior when venue features are disabled:
`lot_size` and `market_data_capacity` are zero,
`self_trade_policy` is `Disabled`, participant ids are zero, and orders are not
post-only. All checks return explicit `Status` values and run before mutation
where an operation can be rejected.

## Lot size

`BookConfig::lot_size` is the quantity increment. Values zero and one disable
increment enforcement. Limit entry, market entry, modify, and replace require a
positive quantity that is an exact multiple of the configured lot size.
Violations return `Status::LotSizeViolation`; zero remains
`Status::InvalidQuantity`.

Resting quantities are valid lots, so each execution and residual is also a lot.
For example, with lot size 10, a buy of 30 may fill resting orders of 10 and 20.
A buy, modify, or replace of 25 is rejected before book mutation.

`InstrumentConfig::lot_size` overrides `BookConfig::lot_size` when nonzero. The
effective value is copied into both stored configurations during engine
construction.

## Post-only

Post-only is an instruction on limit add and replace, represented by the final
`post_only` argument or by `VenueCommand::post_only`.

Python exposes the same venue dispatch boundary through `VenueCommand`.
Construct a normal `Command`, wrap it with `VenueCommand(command,
participant_id=..., post_only=...)`, then pass the wrapper to
`MatchingEngine.dispatch(...)` or the event-buffer dispatch methods. The
direct C++ `add_limit_order`, `replace`, and `match_market_order` overloads
remain C++-only.

- Post-only is valid only with GTC. IOC/FOK combinations return
  `Status::InvalidPostOnlyTimeInForce`.
- Any opposite best price that crosses the limit causes
  `Status::PostOnlyWouldCross`. This check precedes STP, so same-participant
  liquidity also causes rejection.
- Rejection does not reserve the order id, change book state, emit market data,
  or consume a market-data sequence number. The normal command-event log emits
  an `OrderRejected` event and advances its independent event sequence.
- A same-price reduction keeps FIFO priority. A no-op replace is accepted.
- A quantity increase or price change loses priority and follows cancel/re-enter
  semantics. A post-only replace preflights crossing and all fixed capacities
  before removing the original order.
- The post-only attribute of a resting order is snapshot/checksum state. A later
  replace sets it to that replace instruction's value.

Example: with an ask at 100, post-only buys at 99 rest and post-only buys at 100
reject without removing the ask.

## Participant identity and STP

`ParticipantId` is a fixed-width `std::uint64_t`. Zero is anonymous and never
triggers STP. Nonzero equal ids are self liquidity. STP is configured per
instrument:

| Policy | Resting self order | Aggressor remainder | Continue matching |
|---|---|---|---|
| `Disabled` | trades normally | normal | yes |
| `CancelAggressor` | remains in FIFO | cancelled | no |
| `CancelResting` | removed from FIFO | remains active | yes |
| `CancelBoth` | removed from FIFO | cancelled | no |

Removing a resting order preserves the relative FIFO order of every remaining
order. A third-party order ahead of a self order trades first. Under
`CancelResting`, matching then continues with the next FIFO order, including a
third-party order at the same price.

For IOC, STP is applied during the normal sweep and any remaining IOC quantity
is cancelled. For FOK, Eigen-Book performs a complete participant-aware
preflight:

- `CancelAggressor` or `CancelBoth` makes FOK fail atomically if self liquidity
  is reached before the requested quantity can fill.
- `CancelResting` skips self quantity when calculating executable liquidity.
  Resting self orders are removed only when enough eligible third-party
  liquidity exists for the complete FOK fill.
- A rejected FOK performs no trades, STP cancellations, market-data emission,
  or sequence advancement.

STP cancellation emits an `OrderCancelled` command event with
`Status::SelfTradePrevented`. An aggressor stopped before any trade returns
`SelfTradePrevented`; one stopped after third-party fills returns
`PartiallyFilled`.

## Replace and priority summary

| Change | Priority | Matching |
|---|---|---|
| Same price, same quantity | retained | none |
| Same price, quantity reduction | retained | none |
| Same price, quantity increase | lost | cancel/re-enter |
| Price change | lost | cancel/re-enter |

Modify remains reduction-only. Quantity increases through modify return
`Status::QuantityIncreaseRejected`.

## Complexity and capacity

Order-id lookup plus cancel unlink remains O(1). Dense best-price traversal is
bounded by crossed occupancy words; sparse traversal is bounded by occupied
levels, while sparse level insertion/removal can shift at most `max_orders`
sorted slots. STP preflight and execution are bounded by crossed FIFO orders.

All order objects, id-map entries, price levels, command events, and market-data
events are preallocated. Event capacity is reserved before mutation. Market
data reserves a deterministic worst-case count (including optional best-quote
events); insufficient capacity returns `MarketDataLogFull` with no partial
mutation.
