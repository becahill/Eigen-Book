# Sequenced Depth, Replay, and Policy Artifacts

This document is the contract for the external market-data and PPO experiment
path. It is separate from the matching engine's own incremental event stream,
which is documented in
[`market-data-and-recovery.md`](market-data-and-recovery.md).

This is an Alpha-stage offline research pipeline. It validates a bounded
snapshot-plus-update tape, but it is not a historical backfill service, a
complete L2 reconstruction, an order-level queue simulator, or a live
execution system. Its fills and evaluation results are model outputs, not
evidence of achievable venue execution.

## Source contract

Binance `bookTicker` contains only the best bid and best offer (BBO/L1). It
does not contain price-level depth, update ranges, trades, or individual queue
state. Eigen-Book does not accept `bookTicker` as input to this pipeline. The
old five-column `timestamp,type,side,price,size` file is left untouched, but the
canonical reader rejects it instead of guessing its meaning. It cannot support
top-five depth, queue, or full-book claims.

The supported live source is Binance Spot:

1. open the diff-depth stream;
2. request an initial REST depth snapshot;
3. discard source depth messages whose final update id is not newer than the
   snapshot;
4. require the first retained range to bridge `snapshot.lastUpdateId + 1`;
5. retain subsequent depth ranges only when they are continuous; and
6. optionally retain aggregate trades after the first depth update has
   synchronized the snapshot, excluding delayed trades older than that
   synchronizing update.

Trades received before synchronization are not written because they cannot be
placed causally against the reconstructed book. A capture that never receives
a bridging depth update fails and publishes no tape.

The live downloader does not backfill past dates. `--date` schedules the UTC
day and starts no earlier than invocation; `--start`/`--end` schedules an
inclusive/exclusive UTC interval. If the interval has already ended, the
command fails.

```sh
python fetch_l2_data.py \
  --symbol BTCUSDT \
  --start 2026-07-16T13:00:00Z \
  --end 2026-07-16T13:05:00Z \
  --destination market-data/BTCUSDT-binance-spot-depth-trades.csv \
  --data-mode depth_trades \
  --price-scale 100 \
  --quantity-scale 100000
```

`--data-mode depth` records snapshots and depth updates only.
`--data-mode depth_trades` also records aggregate trades. The snapshot limit
is configurable from the limits accepted by the Binance Spot endpoint; the
pipeline therefore does not claim a complete exchange book beyond that
bounded snapshot. If a known level is deleted, a deeper level outside the
snapshot is still unknown until the source updates it. The scale values must
exactly represent every source decimal
for the selected symbol and must be passed unchanged to replay; the example
values are for a cents-and-five-decimal-place BTCUSDT configuration.

The downloader writes to a temporary file in the destination directory,
flushes and fsyncs it, replays it through the canonical validator, and uses an
atomic rename only after validation succeeds. A source error, sequence gap,
invalid/crossed event, or failed validation removes the temporary file,
preserves any existing destination, and returns a nonzero exit status.

## Canonical CSV v2

The exact header for `eigenbook.market_data.v2` is:

```text
schema_version,data_mode,venue,symbol,price_scale,quantity_scale,event_id,event_time,event_kind,first_update_id,last_update_id,previous_update_id,trade_id,first_trade_id,last_trade_id,aggressor_side,side,price,size
```

The rows have these rules:

- `event_id` starts at 1 and increases by one. All rows with one id must be
  adjacent. Timestamps never define an event boundary.
- `event_kind` is `snapshot`, `depth_update`, or `trade`.
- One snapshot or depth-update event may contain many bid and ask rows. Those
  rows are one atomic source event and share identical metadata.
- Depth sizes are absolute. Zero size deletes a level and is permitted only in
  `depth_update` rows. A snapshot must contain positive bid and ask liquidity.
- A snapshot has equal `first_update_id` and `last_update_id` and no
  `previous_update_id`. A later snapshot starts an explicit recovery epoch and
  must have a source update id newer than the preceding depth update. Every
  snapshot must be followed immediately by the depth update that bridges it;
  a trade, another snapshot, or end of file before that bridge is invalid.
- A depth update after id `u` must satisfy
  `first_update_id <= u + 1 <= last_update_id`. When
  `previous_update_id` is present, it must equal `u`.
- A trade occupies exactly one row and preserves Binance's aggregate trade id
  plus its first and last raw trade ids. After the accepted baseline,
  aggregate ids and raw-id ranges must both be continuous. The row identifies
  the aggressor as `buy` or `sell`. Trade rows are permitted only in
  `depth_trades` mode.
- Schema version, mode, venue, symbol, and both scales are invariant for the
  complete tape.

Prices and sizes are decimal strings on disk. The reader uses exact decimal
arithmetic and requires multiplication by the declared scale to produce an
integer within the native signed-price or unsigned-quantity range. A float is
never used as a price-level identity.

`SequencedDepthCsv` validates and assembles every complete event before it is
yielded. `DepthBook.apply` builds a candidate state and commits only after the
whole event is valid. A locked or crossed result (`best_bid >= best_ask`),
duplicate level, precision error, identity change, or sequence gap raises
`MarketDataError`; the diagnostic includes the source event/update or trade id
and the physical row or row range. The bad event is not silently discarded and
the prior book remains unchanged.

## Atomic replay order

`SequencedMarketEnv` processes each source event in this order:

1. validate and commit the complete event to the external `DepthBook`;
2. send that event once to `FeatureExtractor` (`update` for snapshot/depth,
   `update_trades` for a trade);
3. apply any evidence-based passive fill for a trade event;
4. activate orders whose event-count latency has elapsed; and
5. construct policy state after the configured number of complete events.

A multi-level source message is still one feed event; its physical rows never
become separate latency ticks. Initial reset applies both the snapshot and its
required bridge before returning the first policy observation, then starts the
policy-facing event counter at zero. A later snapshot atomically replaces
external depth, cancels active and pending agent orders, and causes replay to
consume its required bridge before inference resumes. Both recovery events
reach `FeatureExtractor` exactly once, but no order survives to activate or
fill between them and no intermediate recovery observation is exposed. A
sequence or validation error marks the feed unsynchronized, cancels orders,
ends the episode, and requires reset/resnapshot; replay never continues across
the gap.

Entry latency may span policy transitions. Pending-quote fields expose the
remaining complete-event count; a newer same-side action deterministically
replaces the older pending quote. Activation rechecks marketability, configured
price range, lot/tick rules, and worst-case inventory exposure.

## Passive-fill assumptions

The fill model is versioned as
`displayed_queue_ahead_causal_trade_approximation.v2`. Results identify
their execution quality as `approximate_price_level_queue`.

- A depth movement, including a cancellation-only price move, never creates a
  fill.
- An order records the displayed aggregate size already resting at its exact
  price when it activates. This is only a queue-ahead estimate; depth data
  cannot reveal the order's actual position.
- A reported aggressive trade must have the opposite aggressor side and the
  exact agent-order price. Its quantity consumes the displayed queue-ahead
  estimate first. Only the remainder can fill the agent order, and a fill is
  capped by both reported trade quantity and remaining order quantity.
- The trade source time must be strictly later than the order's activation
  source time and must not regress behind the replay's source-time high
  watermark for the recovery epoch. Same-time or late cross-stream evidence
  still reaches `FeatureExtractor`, but cannot consume queue or fill an order;
  returned info counts it as ignored uncertain trade evidence.
- Partial fills update remaining quantity, inventory, cash, configured maker
  fees, wealth, and reward in the same replay transition.
- Depth reductions without a trade are treated conservatively as cancellations
  and do not reduce the stored queue-ahead estimate. Additions after activation
  are not placed ahead of the order.

Important limitations remain. True queue position, hidden liquidity,
order-level cancellations, and venue matching rules are not observable from
price-level depth. A trade printed beyond an agent order's price does not fill
it because the current model requires exact-price evidence. If a depth-only
move makes an already resting agent order marketable, the replay cancels that
order as an unconfirmed cross; newly activating marketable quotes are rejected.
Neither case invents a fill. These choices can understate fills and are not
evidence of achievable execution. Requested terminal inventory liquidation
consumes only the validated displayed quantity, charges the configured taker
fee only on the executed portion, and reports requested, executed, remaining,
and completion fields. Insufficient displayed quantity leaves residual
inventory marked in the completed terminal transition. There is no
market-impact model.

For command-driven integrations built directly on `LimitOrderBookEnv`, use
`dispatch_external_transition(command)` for external flow. It validates the
native dispatch result and returns its copied result plus observation, reward,
termination flags, and info only after maker fills, inventory, cash, fees,
mark, and wealth have been reconciled. Rejected statuses raise
`ExternalDispatchError`. The corrected executable pattern is in
`experiments/historical_replay.py`; `dispatch_external(command)` remains only
for callers that intentionally accept deferred transition reporting.

## Canonical policy interface

Training, evaluation, model reload, and paper replay construct the same wrapper
stack:

```text
SequencedMarketEnv -> MarketMakerRewardWrapper -> CanonicalObservationWrapper
```

Observation schema `eigenbook.causal_depth_observation.v2` is a bounded
23-value `float32` vector in this fixed order:

```text
order_flow_imbalance
microprice_pressure
microprice_drift
spread
known_depth_imbalance_up_to_5
trade_flow_imbalance
inventory_fraction
active_bid_present, active_bid_distance, active_bid_quantity, active_bid_queue_ahead
active_ask_present, active_ask_distance, active_ask_quantity, active_ask_queue_ahead
pending_bid_present, pending_bid_distance, pending_bid_quantity, pending_bid_latency
pending_ask_present, pending_ask_distance, pending_ask_quantity, pending_ask_latency
```

The named depth imbalance uses up to five nearest levels currently known from
the bounded snapshot and subsequent updates; it is not a claim that five
complete venue levels are always known. The first six values are causal
`FeatureExtractor` outputs. The remainder
exposes scaled inventory plus active and event-latency-delayed quote state.
Queue-ahead estimates use a bounded transform and remain explicitly
approximate. Absolute prices are not policy inputs. Action schema
`eigenbook.passive_quote_action.v1` is
`[side, passive_distance_ticks, quantity_code]`; the quote is placed relative
to the current best price for its side and quantity is
`(quantity_code + 1) * lot_size`.

Run training and paper replay with:

```sh
python experiments/train_market_data.py \
  --market-data market-data/BTCUSDT-binance-spot-depth-trades.csv \
  --evaluation-market-data market-data/BTCUSDT-binance-spot-evaluation.csv \
  --symbol BTCUSDT \
  --venue binance_spot \
  --data-mode depth_trades \
  --price-scale 100 \
  --quantity-scale 100000 \
  --model-path training-output/ppo_eigenbook_depth

python experiments/paper_trader.py \
  --market-data market-data/BTCUSDT-binance-spot-evaluation.csv \
  --model training-output/ppo_eigenbook_depth.zip \
  --maximum-steps 10000
```

Paper replay transmits no orders. With one fixed tape, model archive, seed,
engine build, and numerical dependency stack, its event ordering and integer
engine transitions are repeatable. This is not a bit-for-bit portability
guarantee for PPO parameters, floating-point actions, or evaluation rewards:
Torch/BLAS builds, CPU/GPU backends, operating systems, and hardware may
produce different ML results. The replay does not substitute Coinbase Spot
data for a Binance-trained policy or otherwise transfer venues. Before
inference it validates the complete tape and checks feed synchronization, tape
mode/symbol/venue/scales, model compatibility, the configured price range,
worst-case quote exposure against inventory limits, and the model's action
space.

## Model sidecar and migration

Each Stable-Baselines3 archive has a required adjacent
`.metadata.json` sidecar with schema `eigenbook.model_metadata.v1`. It records:

- market-data schema and mode, observation, action, and fill-model versions;
- ordered observation/action names and exact Gymnasium space signatures;
- price and quantity scales, symbol, and venue;
- maker/taker fee assumptions, inventory-penalty rate, and the complete
  feature/replay configuration;
  and
- Python, Eigen-Book, NumPy, Gymnasium, Stable-Baselines3, and Torch versions.

The model archive and sidecar are each built in temporary files, validated,
fsynced, and renamed only after both temporary artifacts are ready. Reload
compares every saved field, current dependency version, and runtime observation
and action space before model parameters are loaded. Missing metadata, a
partially published pair, or any mismatch fails closed.

Migration is intentionally explicit:

- The old five-column CSV and any `bookTicker` download are not converted into
  depth. Preserve them only as separately labeled BBO artifacts or recapture a
  canonical snapshot-plus-depth tape. Renaming columns is not a valid
  migration because source update ids and atomic message boundaries are absent.
- Development `eigenbook.market_data.v1` tapes lack raw trade-id ranges and
  cannot establish the v2 trade-continuity contract. Recapture them; renaming
  the schema version is not a migration.
- Old PPO archives using 20-, 16-, 6-, or 4-value feature layouts, and
  development `causal_depth_observation.v1` archives with the former 21-value
  layout, have no compatible policy schema. Retrain them with the canonical
  wrapper. Copying a new sidecar beside an old archive is not a migration.
- Models whose sidecars name the former fill-model v1 lack the recovery and
  causal-time evidence gates and are deliberately incompatible with v2;
  retrain them rather than editing the sidecar.
- Model archives and sidecars form one artifact. Move, retain, or delete them
  together.
