"""Deterministic sequenced-depth replay and approximate passive execution.

The reconstructed external depth and the agent's native own-order ledger are
deliberately separate.  A depth change never creates a fill.  Passive fills
require an aggressive trade at the exact resting price, consume the displayed
queue-ahead estimate first, and are capped by the remaining reported trade
quantity.  This is a documented approximation because price-level data cannot
reveal an individual order's true queue position.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Mapping
from dataclasses import dataclass
import heapq
import math
from pathlib import Path
from typing import Any, Final, Iterator

import gymnasium as gym
import numpy as np

from . import _eigenbook as eb
from .features import FeatureExtractor
from .market_data import (
    DATA_MODES,
    SCHEMA_VERSION as MARKET_DATA_SCHEMA_VERSION,
    DepthBook,
    MarketDataError,
    MarketEvent,
    SequencedDepthCsv,
)
from .observation import (
    CanonicalObservationEncoder,
    ObservationInput,
    PendingQuoteState,
    QuoteState,
)


BUY_SIDE: Final = 0
SELL_SIDE: Final = 1
INSTRUMENT_ID: Final = 101

FILL_MODEL_VERSION: Final = "displayed_queue_ahead_causal_trade_approximation.v2"
EXECUTION_QUALITY: Final = "approximate_price_level_queue"

DEFAULT_PRICE_SCALE: Final = 100
DEFAULT_QUANTITY_SCALE: Final = 100_000
DEFAULT_MAKER_FEE_RATE: Final = 0.0002
DEFAULT_TAKER_FEE_RATE: Final = 0.0005

FIRST_SYNTHETIC_ORDER_ID: Final = 1 << 63
LAST_AGENT_ORDER_ID: Final = FIRST_SYNTHETIC_ORDER_ID - 1
UINT64_MAX: Final = (1 << 64) - 1
UINT32_MAX: Final = (1 << 32) - 1
INT64_MAX: Final = (1 << 63) - 1

TRADE_EVENT_KIND: Final = int(eb.BookEventKind.TRADE)
ORDER_ACCEPTED_EVENT_KIND: Final = int(eb.BookEventKind.ORDER_ACCEPTED)


@dataclass(frozen=True, slots=True)
class ReplayConfig:
    """Structural, feature, risk, and economic replay assumptions."""

    symbol: str = "BTCUSDT"
    venue: str = "binance_spot"
    data_mode: str = "depth_trades"
    price_scale: int = DEFAULT_PRICE_SCALE
    quantity_scale: int = DEFAULT_QUANTITY_SCALE
    own_order_min_price: int = 0
    own_order_max_price: int = (1 << 32) - 2
    tick_size: int = 1
    lot_size: int = 1
    max_quote_distance_ticks: int = 10
    max_order_quantity_lots: int = 100
    max_abs_inventory_lots: int | None = 10_000
    max_episode_steps: int | None = None
    events_per_action: int = 1
    order_entry_latency_events: int = 1
    maker_fee_rate: float = DEFAULT_MAKER_FEE_RATE
    taker_fee_rate: float = DEFAULT_TAKER_FEE_RATE
    feature_window_size: int = 32
    feature_spread_scale_ticks: int = 8
    inventory_penalty_rate: float = 0.01
    liquidate_on_termination: bool = True

    def __post_init__(self) -> None:
        if not self.symbol.strip() or not self.venue.strip():
            raise ValueError("symbol and venue must be non-empty")
        if self.data_mode not in DATA_MODES:
            raise ValueError(f"data_mode must be one of {sorted(DATA_MODES)}")
        for name, value in (
            ("price_scale", self.price_scale),
            ("quantity_scale", self.quantity_scale),
            ("tick_size", self.tick_size),
            ("lot_size", self.lot_size),
            ("max_order_quantity_lots", self.max_order_quantity_lots),
            ("events_per_action", self.events_per_action),
            ("feature_window_size", self.feature_window_size),
            ("feature_spread_scale_ticks", self.feature_spread_scale_ticks),
        ):
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")
        for name, value in (
            ("own_order_min_price", self.own_order_min_price),
            ("own_order_max_price", self.own_order_max_price),
            ("max_quote_distance_ticks", self.max_quote_distance_ticks),
            ("order_entry_latency_events", self.order_entry_latency_events),
        ):
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise ValueError(f"{name} must be a non-negative integer")
        if self.own_order_min_price > self.own_order_max_price:
            raise ValueError("own_order_min_price cannot exceed own_order_max_price")
        if self.own_order_max_price > INT64_MAX:
            raise ValueError("own_order_max_price exceeds native int64 capacity")
        price_range = self.own_order_max_price - self.own_order_min_price
        if price_range % self.tick_size:
            raise ValueError("own-order price range must be divisible by tick_size")
        if price_range // self.tick_size >= UINT32_MAX:
            raise ValueError("own-order price range has too many tick intervals")
        for name, value in (
            ("max_abs_inventory_lots", self.max_abs_inventory_lots),
            ("max_episode_steps", self.max_episode_steps),
        ):
            if value is not None and (
                isinstance(value, bool) or not isinstance(value, int) or value <= 0
            ):
                raise ValueError(f"{name} must be positive or None")
        if self.max_order_quantity_lots * self.lot_size > UINT64_MAX:
            raise ValueError("maximum order quantity exceeds native capacity")
        if (
            self.max_abs_inventory_lots is not None
            and self.max_abs_inventory_lots * self.lot_size > UINT64_MAX
        ):
            raise ValueError("inventory limit exceeds native capacity")
        if not -1.0 < self.maker_fee_rate < 1.0 or not math.isfinite(
            self.maker_fee_rate
        ):
            raise ValueError("maker_fee_rate must be finite and between -1 and 1")
        if not 0.0 <= self.taker_fee_rate < 1.0 or not math.isfinite(
            self.taker_fee_rate
        ):
            raise ValueError("taker_fee_rate must be finite and in [0, 1)")
        if (
            not math.isfinite(self.inventory_penalty_rate)
            or self.inventory_penalty_rate < 0.0
        ):
            raise ValueError("inventory_penalty_rate must be finite and non-negative")
        if not isinstance(self.liquidate_on_termination, bool):
            raise TypeError("liquidate_on_termination must be a bool")

    @property
    def max_order_quantity(self) -> int:
        return self.max_order_quantity_lots * self.lot_size

    @property
    def max_abs_inventory(self) -> int | None:
        if self.max_abs_inventory_lots is None:
            return None
        return self.max_abs_inventory_lots * self.lot_size

    def feature_configuration(
        self,
    ) -> dict[str, int | float | bool | str | None]:
        return {
            "window_size": self.feature_window_size,
            "tick_size": self.tick_size,
            "spread_scale_ticks": self.feature_spread_scale_ticks,
            "trade_side_is_aggressor": True,
            "events_per_action": self.events_per_action,
            "order_entry_latency_events": self.order_entry_latency_events,
            "lot_size": self.lot_size,
            "own_order_min_price": self.own_order_min_price,
            "own_order_max_price": self.own_order_max_price,
            "max_quote_distance_ticks": self.max_quote_distance_ticks,
            "max_order_quantity_lots": self.max_order_quantity_lots,
            "max_abs_inventory_lots": self.max_abs_inventory_lots,
            "inventory_penalty_rate": self.inventory_penalty_rate,
            "liquidate_on_termination": self.liquidate_on_termination,
        }


@dataclass(frozen=True, slots=True)
class OwnOrder:
    order_id: int
    side: int
    price: int
    quantity: int
    queue_ahead: int
    activation_event_time: int = 0


@dataclass(frozen=True, slots=True)
class PendingOrder:
    order: OwnOrder
    activation_event: int


@dataclass(frozen=True, slots=True)
class MakerFill:
    order_id: int
    side: int
    price: int
    quantity: int
    event_id: int
    trade_id: int
    first_trade_id: int
    last_trade_id: int
    reported_trade_quantity: int
    queue_ahead_before: int
    queue_ahead_after: int


@dataclass(frozen=True, slots=True)
class Liquidation:
    side: str
    requested_quantity: int
    executed_quantity: int
    remaining_quantity: int
    notional: float
    fee: float
    legs: tuple[tuple[int, int], ...]

    @property
    def complete(self) -> bool:
        return self.remaining_quantity == 0


def make_instrument_config(config: ReplayConfig) -> eb.InstrumentConfig:
    """Build the bounded sparse native store used only for agent orders."""

    book = eb.BookConfig()
    book.min_price = config.own_order_min_price
    book.max_price = config.own_order_max_price
    book.max_orders = 64
    book.order_id_map_capacity = 128
    book.event_log_capacity = 66
    book.tick_size = config.tick_size
    book.lot_size = config.lot_size
    book.price_level_mode = eb.PriceLevelMode.SPARSE

    instrument = eb.InstrumentConfig()
    instrument.instrument_id = INSTRUMENT_ID
    instrument.book_config = book
    return instrument


class OwnOrderLedger:
    """Native own-order store with Python-side queue-ahead approximation."""

    def __init__(self, instrument: eb.InstrumentConfig) -> None:
        self.instrument = instrument
        self.instrument_id = int(instrument.instrument_id)
        self.active_orders: dict[int, OwnOrder] = {}
        self._side_order_ids: dict[int, int] = {}
        self._next_agent_id = 1
        self._next_synthetic_id = FIRST_SYNTHETIC_ORDER_ID
        self._next_timestamp = 1

        self._add = eb.Command()
        self._cancel = eb.Command()
        self._aggress = eb.Command()
        self.engine: eb.MatchingEngine
        self.event_buffer: np.ndarray
        self.reset()

    def reset(self) -> None:
        self.engine = eb.MatchingEngine((self.instrument,))
        self.event_buffer = np.empty(
            self.engine.event_buffer_capacity(self.instrument_id),
            dtype=eb.BOOK_EVENT_DTYPE,
        )
        self.active_orders.clear()
        self._side_order_ids.clear()
        self._next_agent_id = 1
        self._next_synthetic_id = FIRST_SYNTHETIC_ORDER_ID
        self._next_timestamp = 1
        for command in (self._add, self._cancel, self._aggress):
            command.instrument_id = self.instrument_id
        self._add.op = eb.CommandOp.ADD
        self._add.time_in_force = eb.TimeInForce.GTC
        self._cancel.op = eb.CommandOp.CANCEL
        self._cancel.time_in_force = eb.TimeInForce.GTC
        self._aggress.op = eb.CommandOp.ADD
        self._aggress.time_in_force = eb.TimeInForce.IOC

    def _timestamp(self) -> int:
        if self._next_timestamp > UINT64_MAX:
            raise OverflowError("native timestamp namespace exhausted")
        value = self._next_timestamp
        self._next_timestamp += 1
        return value

    def reserve_order_id(self) -> int:
        if self._next_agent_id > LAST_AGENT_ORDER_ID:
            raise OverflowError("agent order-id namespace exhausted")
        value = self._next_agent_id
        self._next_agent_id += 1
        return value

    def active_order(self, side: int) -> OwnOrder | None:
        order_id = self._side_order_ids.get(side)
        return None if order_id is None else self.active_orders[order_id]

    def add(self, order: OwnOrder) -> Any:
        if self.active_order(order.side) is not None:
            raise RuntimeError("cancel the existing side before adding a quote")
        command = self._add
        command.order_id = order.order_id
        command.side = eb.Side.BUY if order.side == BUY_SIDE else eb.Side.SELL
        command.price = order.price
        command.quantity = order.quantity
        command.timestamp = self._timestamp()
        result = self.engine.dispatch(eb.VenueCommand(command, 1, True))
        if result.status != eb.Status.ACCEPTED:
            return result
        if int(result.resting_quantity) != order.quantity:
            raise RuntimeError("accepted own order returned wrong resting quantity")
        self.active_orders[order.order_id] = order
        self._side_order_ids[order.side] = order.order_id
        return result

    def cancel(self, order_id: int) -> OwnOrder:
        order = self.active_orders.get(order_id)
        if order is None:
            raise RuntimeError(f"cannot cancel unknown own order {order_id}")
        command = self._cancel
        command.order_id = order_id
        command.side = eb.Side.BUY if order.side == BUY_SIDE else eb.Side.SELL
        command.price = order.price
        command.quantity = order.quantity
        command.timestamp = self._timestamp()
        result = self.engine.dispatch(command)
        if (
            result.status != eb.Status.CANCELLED
            or int(result.canceled_quantity) != order.quantity
        ):
            raise RuntimeError(
                f"native cancel diverged for order {order_id}: {result.status}"
            )
        self.active_orders.pop(order_id)
        if self._side_order_ids.pop(order.side, None) != order_id:
            raise RuntimeError("own-side index diverged during cancel")
        return order

    def cancel_side(self, side: int) -> OwnOrder | None:
        order = self.active_order(side)
        return None if order is None else self.cancel(order.order_id)

    def cancel_all(self) -> tuple[OwnOrder, ...]:
        return tuple(self.cancel(order_id) for order_id in sorted(self.active_orders))

    def cancel_crossed_by_market(
        self,
        market: DepthBook,
    ) -> tuple[OwnOrder, ...]:
        """Cancel quotes crossed by depth changes without inventing executions."""

        crossed: list[OwnOrder] = []
        for side in (BUY_SIDE, SELL_SIDE):
            order = self.active_order(side)
            if order is None:
                continue
            marketable = (
                market.best_ask is not None and order.price >= market.best_ask
                if side == BUY_SIDE
                else market.best_bid is not None and order.price <= market.best_bid
            )
            if marketable:
                crossed.append(self.cancel(order.order_id))
        return tuple(crossed)

    def _fill_order(
        self,
        order: OwnOrder,
        quantity: int,
        event: MarketEvent,
        *,
        queue_before: int,
        queue_after: int,
    ) -> MakerFill:
        if quantity <= 0 or quantity > order.quantity:
            raise RuntimeError("invalid evidence-capped maker fill quantity")
        if self._next_synthetic_id > UINT64_MAX:
            raise OverflowError("synthetic order-id namespace exhausted")
        synthetic_id = self._next_synthetic_id
        self._next_synthetic_id += 1

        command = self._aggress
        command.order_id = synthetic_id
        command.side = eb.Side.SELL if order.side == BUY_SIDE else eb.Side.BUY
        command.price = order.price
        command.quantity = quantity
        command.timestamp = self._timestamp()
        result = self.engine.dispatch_result_with_buffer(command, self.event_buffer)
        if (
            result.status != eb.Status.FILLED
            or int(result.accepted_quantity) != quantity
            or int(result.executed_quantity) != quantity
            or int(result.resting_quantity) != 0
        ):
            raise RuntimeError(
                "native evidence fill failed: "
                f"status={result.status}, requested={quantity}, "
                f"executed={result.executed_quantity}"
            )

        trade_quantity = 0
        trade_count = 0
        for index in range(int(result.events_emitted)):
            native_event = self.event_buffer[index]
            kind = int(native_event["kind"])
            if index == 0 and kind != ORDER_ACCEPTED_EVENT_KIND:
                raise RuntimeError("synthetic IOC did not begin with acceptance")
            if kind != TRADE_EVENT_KIND:
                continue
            trade_count += 1
            trade = native_event["trade"]
            if (
                int(trade["aggressor_id"]) != synthetic_id
                or int(trade["resting_id"]) != order.order_id
                or int(trade["price"]) != order.price
            ):
                raise RuntimeError("native evidence fill referenced the wrong order")
            trade_quantity += int(trade["quantity"])
        if trade_count != 1 or trade_quantity != quantity:
            raise RuntimeError("native evidence fill events did not reconcile")

        remaining = order.quantity - quantity
        if remaining == 0:
            self.active_orders.pop(order.order_id)
            if self._side_order_ids.pop(order.side, None) != order.order_id:
                raise RuntimeError("own-side index diverged during fill")
        else:
            self.active_orders[order.order_id] = OwnOrder(
                order.order_id,
                order.side,
                order.price,
                remaining,
                queue_after,
                order.activation_event_time,
            )
        assert event.trade_id is not None
        assert event.first_trade_id is not None
        assert event.last_trade_id is not None
        assert event.trade_size is not None
        return MakerFill(
            order_id=order.order_id,
            side=order.side,
            price=order.price,
            quantity=quantity,
            event_id=event.event_id,
            trade_id=event.trade_id,
            first_trade_id=event.first_trade_id,
            last_trade_id=event.last_trade_id,
            reported_trade_quantity=event.trade_size,
            queue_ahead_before=queue_before,
            queue_ahead_after=queue_after,
        )

    def apply_trade(
        self,
        event: MarketEvent,
        *,
        source_time_floor: int | None,
    ) -> tuple[tuple[MakerFill, ...], bool]:
        """Apply causally usable exact-price aggressive evidence.

        The boolean result identifies otherwise matching evidence that was
        ignored because its source time was not provably after activation or
        regressed behind the processed cross-stream source-time watermark.
        """

        if event.kind != "trade":
            raise TypeError("apply_trade requires a trade event")
        assert event.aggressor_side is not None
        assert event.trade_price is not None
        assert event.trade_size is not None
        resting_side = BUY_SIDE if event.aggressor_side == "sell" else SELL_SIDE
        order = self.active_order(resting_side)
        if order is None or event.trade_price != order.price:
            return (), False
        if event.event_time <= order.activation_event_time or (
            source_time_floor is not None and event.event_time < source_time_floor
        ):
            return (), True

        queue_before = order.queue_ahead
        queue_after = max(0, queue_before - event.trade_size)
        available = max(0, event.trade_size - queue_before)
        fill_quantity = min(order.quantity, available)
        if fill_quantity == 0:
            self.active_orders[order.order_id] = OwnOrder(
                order.order_id,
                order.side,
                order.price,
                order.quantity,
                queue_after,
                order.activation_event_time,
            )
            return (), False
        return (
            (
                self._fill_order(
                    order,
                    fill_quantity,
                    event,
                    queue_before=queue_before,
                    queue_after=queue_after,
                ),
            ),
            False,
        )


class SequencedMarketEnv(gym.Env[np.ndarray, np.ndarray]):
    """Gymnasium environment driven only by complete canonical source events."""

    metadata = {"render_modes": []}

    def __init__(
        self,
        market_data_path: str | Path,
        *,
        config: ReplayConfig = ReplayConfig(),
        feature_extractor: FeatureExtractor | None = None,
    ) -> None:
        super().__init__()
        self.path = Path(market_data_path)
        self.config = config
        self.market = DepthBook()
        self.own_orders = OwnOrderLedger(make_instrument_config(config))
        if feature_extractor is None:
            self.feature_extractor = FeatureExtractor(
                window_size=config.feature_window_size,
                tick_size=config.tick_size,
                spread_scale_ticks=config.feature_spread_scale_ticks,
                trade_side_is_aggressor=True,
            )
        else:
            if not isinstance(feature_extractor, FeatureExtractor):
                raise TypeError("feature_extractor must be a FeatureExtractor")
            observed_feature_config = (
                feature_extractor.window_size,
                feature_extractor.tick_size,
                feature_extractor.spread_scale_ticks,
                feature_extractor.trade_side_is_aggressor,
            )
            expected_feature_config = (
                config.feature_window_size,
                float(config.tick_size),
                float(config.feature_spread_scale_ticks),
                True,
            )
            if observed_feature_config != expected_feature_config:
                raise ValueError(
                    "feature_extractor configuration does not match ReplayConfig: "
                    f"observed={observed_feature_config}, "
                    f"expected={expected_feature_config}"
                )
            self.feature_extractor = feature_extractor
        self.observation_encoder = CanonicalObservationEncoder()

        self._pending: deque[PendingOrder] = deque()
        self._events: Iterator[MarketEvent] | None = None
        self._raw_observation = np.zeros(1, dtype=np.float32)
        self.observation_space = gym.spaces.Box(
            low=0.0,
            high=0.0,
            shape=(1,),
            dtype=np.float32,
        )
        self.action_space = gym.spaces.MultiDiscrete(
            np.asarray(
                (
                    2,
                    config.max_quote_distance_ticks + 1,
                    config.max_order_quantity_lots,
                ),
                dtype=np.int64,
            ),
            dtype=np.int64,
        )

        self.cash = 0.0
        self.inventory = 0
        self.mark_price = 0.0
        self.wealth = 0.0
        self.cumulative_maker_fee = 0.0
        self.cumulative_taker_fee = 0.0
        self.elapsed_steps = 0
        self.feed_event_count = 0
        self.feature_event_count = 0
        self.last_event_id = 0
        self.last_event_time = 0
        self.source_time_watermark: int | None = None
        self.feed_synchronized = False
        self.source_time_watermark = None
        self._has_reset = False
        self._episode_done = False
        self._closed = False
        self._resume_on_reset = False

    @property
    def inventory_base_units(self) -> float:
        return self.inventory / self.config.quantity_scale

    def pending_order(self, side: int) -> PendingOrder | None:
        return next((item for item in self._pending if item.order.side == side), None)

    def _cancel_pending_side(self, side: int) -> PendingOrder | None:
        pending = self.pending_order(side)
        if pending is not None:
            self._pending.remove(pending)
        return pending

    def _feature_update(self, event: MarketEvent) -> None:
        if event.kind in {"snapshot", "depth_update"}:
            self.feature_extractor.update(
                {
                    "type": "snapshot" if event.kind == "snapshot" else "update",
                    "changes": tuple(
                        {
                            "side": level.side,
                            "price": level.price,
                            "size": level.size,
                        }
                        for level in event.levels
                    ),
                }
            )
        else:
            assert event.trade_size is not None
            assert event.aggressor_side is not None
            self.feature_extractor.update_trades(
                {
                    "aggressor_side": event.aggressor_side,
                    "quantity": event.trade_size,
                }
            )
        self.feature_event_count += 1

    def _apply_event(
        self,
        event: MarketEvent,
    ) -> tuple[tuple[MakerFill, ...], int, tuple[OwnOrder, ...], bool]:
        was_initialized = self.market.initialized
        source_time_floor = self.source_time_watermark
        self.market.apply(event)
        self._feature_update(event)

        cancelled_for_resnapshot = 0
        if event.kind == "snapshot" and was_initialized:
            cancelled_for_resnapshot = len(self.own_orders.cancel_all()) + len(
                self._pending
            )
            self._pending.clear()
            fills: tuple[MakerFill, ...] = ()
            uncertain_crosses: tuple[OwnOrder, ...] = ()
            ignored_trade_evidence = False
            self.source_time_watermark = None
        elif event.kind == "trade":
            fills, ignored_trade_evidence = self.own_orders.apply_trade(
                event,
                source_time_floor=source_time_floor,
            )
            uncertain_crosses = ()
            self.source_time_watermark = max(
                event.event_time,
                source_time_floor if source_time_floor is not None else 0,
            )
        else:
            fills = ()
            ignored_trade_evidence = False
            uncertain_crosses = self.own_orders.cancel_crossed_by_market(self.market)
            self.source_time_watermark = max(
                event.event_time,
                source_time_floor if source_time_floor is not None else 0,
            )

        self.feed_event_count += 1
        self.last_event_id = event.event_id
        self.last_event_time = event.event_time
        self.feed_synchronized = self.market.synchronized
        return (
            fills,
            cancelled_for_resnapshot,
            uncertain_crosses,
            ignored_trade_evidence,
        )

    def _apply_required_recovery_bridge(self) -> None:
        """Consume the depth event that makes the active snapshot observable."""

        bridge = self._next_event()
        if bridge is None:
            raise MarketDataError(
                "market-data tape ended before the active snapshot was bridged"
            )
        self._validate_identity(bridge)
        if bridge.kind != "depth_update":
            raise MarketDataError(
                f"{bridge.source_context}: recovery requires an immediate "
                "depth_update after snapshot"
            )
        fills, cancelled, uncertain_crosses, ignored = self._apply_event(bridge)
        if fills or cancelled or uncertain_crosses or ignored:
            raise RuntimeError("recovery bridge produced an impossible side effect")
        if not self.feed_synchronized or self.source_time_watermark is None:
            raise MarketDataError(
                f"{bridge.source_context}: recovery bridge did not synchronize feed"
            )

    def _fail_feed_closed(self) -> None:
        self.feed_synchronized = False
        self._episode_done = True
        self.own_orders.cancel_all()
        self._pending.clear()

    def _next_event(self) -> MarketEvent | None:
        if self._events is None:
            raise RuntimeError("market-data iterator is not initialized")
        try:
            return next(self._events)
        except StopIteration:
            return None
        except MarketDataError:
            self._fail_feed_closed()
            raise

    def _validate_identity(self, event: MarketEvent) -> None:
        expected = (
            self.config.data_mode,
            self.config.venue,
            self.config.symbol,
            self.config.price_scale,
            self.config.quantity_scale,
        )
        observed = (
            event.data_mode,
            event.venue,
            event.symbol,
            event.price_scale,
            event.quantity_scale,
        )
        if observed != expected:
            raise MarketDataError(
                f"{event.source_context}: tape identity {observed} does not "
                f"match configured identity {expected}"
            )

    def _mid_price(self) -> float:
        bid = self.market.best_bid
        ask = self.market.best_ask
        if bid is None or ask is None:
            return self.mark_price
        return bid + (ask - bid) * 0.5

    def _wealth(self, mark: float) -> float:
        inventory_value = (
            self.inventory
            * mark
            / (self.config.price_scale * self.config.quantity_scale)
        )
        return self.cash + inventory_value

    def _record_fill(self, fill: MakerFill) -> float:
        notional = (
            fill.price
            * fill.quantity
            / (self.config.price_scale * self.config.quantity_scale)
        )
        fee = notional * self.config.maker_fee_rate
        if fill.side == BUY_SIDE:
            self.inventory += fill.quantity
            self.cash -= notional + fee
        else:
            self.inventory -= fill.quantity
            self.cash += notional - fee
        self.cumulative_maker_fee += fee
        return fee

    def _decode_action(self, action: np.ndarray) -> tuple[int, int, int]:
        value = np.asarray(action)
        if value.shape != (3,):
            raise ValueError("action must have shape (3,)")
        if not np.issubdtype(value.dtype, np.integer):
            raise TypeError("action values must have an integer dtype")
        if not self.action_space.contains(value):
            raise ValueError("action is outside the configured action space")
        return int(value[0]), int(value[1]), int(value[2])

    def _quote_price(self, side: int, distance_ticks: int) -> int | None:
        reference = self.market.best_bid if side == BUY_SIDE else self.market.best_ask
        if reference is None:
            return None
        direction = -1 if side == BUY_SIDE else 1
        return reference + direction * distance_ticks * self.config.tick_size

    def _is_marketable(self, order: OwnOrder) -> bool:
        if order.side == BUY_SIDE:
            return (
                self.market.best_ask is not None and order.price >= self.market.best_ask
            )
        return self.market.best_bid is not None and order.price <= self.market.best_bid

    def _would_exceed_inventory_limit(self, order: OwnOrder) -> bool:
        limit = self.config.max_abs_inventory
        if limit is None:
            return False
        projected = (
            self.inventory + order.quantity
            if order.side == BUY_SIDE
            else self.inventory - order.quantity
        )
        return abs(projected) > limit

    def _activate(self, order: OwnOrder) -> tuple[str, int]:
        cancelled = 0
        if not self.feed_synchronized or self.source_time_watermark is None:
            return "REJECTED_UNSYNCHRONIZED_FEED", cancelled
        if not (
            self.config.own_order_min_price
            <= order.price
            <= self.config.own_order_max_price
        ):
            return "REJECTED_OUTSIDE_ENGINE_RANGE", cancelled
        if (order.price - self.config.own_order_min_price) % self.config.tick_size:
            return "REJECTED_TICK_SIZE", cancelled
        if order.quantity % self.config.lot_size:
            return "REJECTED_LOT_SIZE", cancelled
        if self._is_marketable(order):
            return "REJECTED_MARKETABLE_ON_ARRIVAL", cancelled
        if self._would_exceed_inventory_limit(order):
            return "REJECTED_INVENTORY_LIMIT", cancelled

        cancelled = int(self.own_orders.cancel_side(order.side) is not None)

        levels = self.market.bids if order.side == BUY_SIDE else self.market.asks
        with_queue = OwnOrder(
            order.order_id,
            order.side,
            order.price,
            order.quantity,
            levels.get(order.price, 0),
            self.source_time_watermark,
        )
        result = self.own_orders.add(with_queue)
        if result.status == eb.Status.ACCEPTED:
            return "RESTING", cancelled
        if result.status == eb.Status.POST_ONLY_WOULD_CROSS:
            return "REJECTED_POST_ONLY_WOULD_CROSS", cancelled
        raise RuntimeError(f"native own-order add failed with {result.status}")

    def _activate_due(self) -> tuple[int, int, int, dict[int, str]]:
        activated = rejected = cancelled = 0
        statuses: dict[int, str] = {}
        while self._pending and (
            self._pending[0].activation_event <= self.feed_event_count
        ):
            pending = self._pending.popleft()
            status, cancel_count = self._activate(pending.order)
            statuses[pending.order.order_id] = status
            cancelled += cancel_count
            if status == "RESTING":
                activated += 1
            else:
                rejected += 1
        return activated, rejected, cancelled, statuses

    def _liquidate(self) -> Liquidation | None:
        if self.inventory == 0:
            return None
        if self.inventory > 0:
            side = "sell"
            requested = self.inventory
            depth = heapq.nlargest(len(self.market.bids), self.market.bids.items())
        else:
            side = "buy"
            requested = -self.inventory
            depth = heapq.nsmallest(len(self.market.asks), self.market.asks.items())
        remaining = requested
        native_notional = 0
        legs: list[tuple[int, int]] = []
        for price, available in depth:
            quantity = min(remaining, available)
            if quantity <= 0:
                continue
            legs.append((price, quantity))
            native_notional += price * quantity
            remaining -= quantity
            if remaining == 0:
                break
        executed = requested - remaining
        notional = native_notional / (
            self.config.price_scale * self.config.quantity_scale
        )
        fee = notional * self.config.taker_fee_rate
        if side == "sell":
            self.cash += notional - fee
            self.inventory -= executed
        else:
            self.cash -= notional + fee
            self.inventory += executed
        self.cumulative_taker_fee += fee
        return Liquidation(
            side,
            requested,
            executed,
            remaining,
            notional,
            fee,
            tuple(legs),
        )

    def write_canonical_observation(self, output: np.ndarray) -> np.ndarray:
        active_bid = self.own_orders.active_order(BUY_SIDE)
        active_ask = self.own_orders.active_order(SELL_SIDE)
        pending_bid = self.pending_order(BUY_SIDE)
        pending_ask = self.pending_order(SELL_SIDE)

        def active(order: OwnOrder | None) -> QuoteState | None:
            return (
                None
                if order is None
                else QuoteState(order.price, order.quantity, order.queue_ahead)
            )

        def pending(order: PendingOrder | None) -> PendingQuoteState | None:
            if order is None:
                return None
            return PendingQuoteState(
                order.order.price,
                order.order.quantity,
                max(0, order.activation_event - self.feed_event_count),
            )

        return self.observation_encoder.encode(
            ObservationInput(
                market_features=self.feature_extractor.get_state(),
                inventory=self.inventory,
                inventory_limit=self.config.max_abs_inventory,
                quantity_scale=self.config.quantity_scale,
                best_bid=self.market.best_bid,
                best_ask=self.market.best_ask,
                tick_size=self.config.tick_size,
                max_quote_distance_ticks=self.config.max_quote_distance_ticks,
                max_order_quantity=self.config.max_order_quantity,
                latency_events=self.config.order_entry_latency_events,
                active_bid=active(active_bid),
                active_ask=active(active_ask),
                pending_bid=pending(pending_bid),
                pending_ask=pending(pending_ask),
            ),
            output,
        )

    def reset(
        self,
        *,
        seed: int | None = None,
        options: dict[str, Any] | None = None,
    ) -> tuple[np.ndarray, dict[str, Any]]:
        if self._closed:
            raise RuntimeError("cannot reset a closed environment")
        super().reset(seed=seed)
        if seed is not None:
            self.action_space.seed(seed)
        reset_options: Mapping[str, Any] = {} if options is None else options
        if not isinstance(reset_options, Mapping):
            raise TypeError("options must be a mapping or None")
        unknown = set(reset_options) - {
            "initial_cash",
            "initial_inventory",
            "rewind_market_data",
        }
        if unknown:
            raise ValueError(f"unsupported reset option(s): {sorted(unknown)}")
        initial_cash = float(reset_options.get("initial_cash", 0.0))
        if not math.isfinite(initial_cash):
            raise ValueError("initial_cash must be finite")
        initial_inventory = reset_options.get("initial_inventory", 0)
        if isinstance(initial_inventory, bool) or not isinstance(
            initial_inventory, (int, np.integer)
        ):
            raise TypeError("initial_inventory must be an integer")
        initial_inventory = int(initial_inventory)
        if (
            self.config.max_abs_inventory is not None
            and abs(initial_inventory) >= self.config.max_abs_inventory
        ):
            raise ValueError("initial_inventory must be inside the risk boundary")
        rewind = reset_options.get("rewind_market_data", False)
        if not isinstance(rewind, bool):
            raise TypeError("rewind_market_data must be a bool")
        resume = self._resume_on_reset and not rewind
        resume_was_synchronized = self.feed_synchronized
        self._resume_on_reset = False

        self.feed_synchronized = False
        self._has_reset = False
        self.own_orders.reset()
        self._pending.clear()
        if not resume:
            self.market = DepthBook()
            self.source_time_watermark = None
            self.feature_extractor.reset()
            self.feature_event_count = 0
            self._events = iter(SequencedDepthCsv(self.path))
            try:
                first = self._next_event()
                if first is None:
                    raise MarketDataError("market-data tape has no snapshot")
                self._validate_identity(first)
                if first.kind != "snapshot":
                    raise MarketDataError(
                        f"{first.source_context}: first event must be a snapshot"
                    )
                self.market.apply(first)
                self._feature_update(first)
                self.last_event_id = first.event_id
                self.last_event_time = first.event_time
                self.feed_synchronized = self.market.synchronized
                self._apply_required_recovery_bridge()
            except Exception:
                self._fail_feed_closed()
                self._has_reset = False
                raise
            # Snapshot reconstruction is initialization, not simulated latency.
            # Both atomic source events still reached the market and features.
            self.feed_event_count = 0
        elif not resume_was_synchronized:
            raise RuntimeError("cannot resume an unsynchronized feed")
        else:
            self.feed_synchronized = True

        self.cash = initial_cash
        self.inventory = initial_inventory
        self.mark_price = self._mid_price()
        self.wealth = self._wealth(self.mark_price)
        self.cumulative_maker_fee = 0.0
        self.cumulative_taker_fee = 0.0
        self.elapsed_steps = 0
        self._episode_done = False
        self._has_reset = True
        info = {
            "inventory": self.inventory,
            "cash": self.cash,
            "mark_price": self.mark_price,
            "wealth": self.wealth,
            "feed_event_count": self.feed_event_count,
            "feature_event_count": self.feature_event_count,
            "source_event_id": self.last_event_id,
            "source_event_time": self.last_event_time,
            "source_time_watermark": self.source_time_watermark,
            "feed_synchronized": self.feed_synchronized,
            "resumed_market_data": resume,
            "fill_model": FILL_MODEL_VERSION,
            "execution_quality": EXECUTION_QUALITY,
        }
        return self._raw_observation, info

    def step(
        self, action: np.ndarray
    ) -> tuple[np.ndarray, float, bool, bool, dict[str, Any]]:
        if self._closed:
            raise RuntimeError("cannot step a closed environment")
        if not self._has_reset:
            raise RuntimeError("reset() must be called before step()")
        if self._episode_done:
            raise RuntimeError("episode has ended; reset before another step")
        if not self.feed_synchronized:
            raise RuntimeError("feed is unsynchronized; resnapshot/reset required")

        side, distance, quantity_code = self._decode_action(action)
        previous_wealth = self.wealth
        order_id = self.own_orders.reserve_order_id()
        quantity = (quantity_code + 1) * self.config.lot_size
        quote_price = self._quote_price(side, distance)
        action_status = "REJECTED_NO_REFERENCE_PRICE"
        replacement_cancelled = 0
        pending_replaced = 0
        if quote_price is not None:
            order = OwnOrder(order_id, side, quote_price, quantity, 0)
            if self.config.order_entry_latency_events == 0:
                action_status, replacement_cancelled = self._activate(order)
            else:
                pending_replaced = int(self._cancel_pending_side(side) is not None)
                self._pending.append(
                    PendingOrder(
                        order,
                        self.feed_event_count + self.config.order_entry_latency_events,
                    )
                )
                action_status = "PENDING"

        fills: list[MakerFill] = []
        action_filled_quantity = 0
        maker_fee = 0.0
        activated = int(action_status == "RESTING")
        rejected = int(action_status.startswith("REJECTED"))
        activation_statuses: dict[int, str] = {}
        resnapshot_cancelled = 0
        uncertain_cross_cancelled: list[OwnOrder] = []
        ignored_trade_evidence_count = 0
        feed_exhausted = False
        risk_limit = False

        for _ in range(self.config.events_per_action):
            event = self._next_event()
            if event is None:
                feed_exhausted = True
                break
            try:
                self._validate_identity(event)
                action_existed_before_event = (
                    order_id in self.own_orders.active_orders
                    or any(
                        pending.order.order_id == order_id for pending in self._pending
                    )
                )
                (
                    event_fills,
                    cancelled,
                    uncertain_crosses,
                    ignored_trade_evidence,
                ) = self._apply_event(event)
                if event.kind == "snapshot":
                    if action_existed_before_event:
                        action_status = (
                            "CANCELLED_RESNAPSHOT_AFTER_PARTIAL_FILL"
                            if action_filled_quantity
                            else "CANCELLED_RESNAPSHOT"
                        )
                    # A recovery snapshot is never exposed to the policy.  Its
                    # bridge is consumed before fills, latency activation, or
                    # the transition observation can resume.
                    self._apply_required_recovery_bridge()
            except MarketDataError:
                self._fail_feed_closed()
                raise
            resnapshot_cancelled += cancelled
            uncertain_cross_cancelled.extend(uncertain_crosses)
            ignored_trade_evidence_count += int(ignored_trade_evidence)
            if any(order.order_id == order_id for order in uncertain_crosses):
                action_status = "CANCELLED_UNCONFIRMED_CROSS"
            for fill in event_fills:
                maker_fee += self._record_fill(fill)
            fills.extend(event_fills)
            current_event_action_fill = sum(
                fill.quantity for fill in event_fills if fill.order_id == order_id
            )
            action_filled_quantity += current_event_action_fill
            if current_event_action_fill:
                action_status = (
                    "PARTIALLY_FILLED"
                    if order_id in self.own_orders.active_orders
                    else "FILLED"
                )

            active_count, rejected_count, cancel_count, statuses = self._activate_due()
            activated += active_count
            rejected += rejected_count
            replacement_cancelled += cancel_count
            activation_statuses.update(statuses)
            if order_id in statuses:
                action_status = statuses[order_id]

            limit = self.config.max_abs_inventory
            if limit is not None and abs(self.inventory) >= limit:
                risk_limit = True
                break

        self.elapsed_steps += 1
        self.mark_price = self._mid_price()
        horizon = (
            self.config.max_episode_steps is not None
            and self.elapsed_steps >= self.config.max_episode_steps
        )
        terminated = feed_exhausted or risk_limit or horizon
        truncated = False
        reason = (
            "sequence_or_validation_error"
            if not self.feed_synchronized
            else "risk_limit"
            if risk_limit
            else "end_of_data"
            if feed_exhausted
            else "configured_horizon"
            if horizon
            else None
        )

        pre_liquidation_inventory = self.inventory
        liquidation = None
        terminal_cancelled = 0
        if terminated:
            if self.config.liquidate_on_termination:
                liquidation = self._liquidate()
            action_open_at_terminal = order_id in self.own_orders.active_orders or any(
                pending.order.order_id == order_id for pending in self._pending
            )
            terminal_cancelled = len(self.own_orders.cancel_all()) + len(self._pending)
            self._pending.clear()
            if action_open_at_terminal:
                action_status = (
                    "CANCELLED_TERMINAL_AFTER_PARTIAL_FILL"
                    if action_filled_quantity
                    else "CANCELLED_TERMINAL"
                )
            self._resume_on_reset = not feed_exhausted and self.feed_synchronized
            self._episode_done = True

        self.wealth = self._wealth(self.mark_price)
        reward = float(self.wealth - previous_wealth)
        info = {
            "action_order_id": order_id,
            "action_status": action_status,
            "action_filled_quantity": action_filled_quantity,
            "quote_price": quote_price,
            "fills": tuple(
                {
                    "order_id": fill.order_id,
                    "side": "buy" if fill.side == BUY_SIDE else "sell",
                    "price": fill.price,
                    "quantity": fill.quantity,
                    "source_event_id": fill.event_id,
                    "trade_id": fill.trade_id,
                    "first_trade_id": fill.first_trade_id,
                    "last_trade_id": fill.last_trade_id,
                    "reported_trade_quantity": fill.reported_trade_quantity,
                    "queue_ahead_before": fill.queue_ahead_before,
                    "queue_ahead_after": fill.queue_ahead_after,
                }
                for fill in fills
            ),
            "fill_count": len(fills),
            "filled_quantity": sum(fill.quantity for fill in fills),
            "step_maker_fee": maker_fee,
            "cumulative_maker_fee": self.cumulative_maker_fee,
            "cumulative_taker_fee": self.cumulative_taker_fee,
            "inventory": self.inventory,
            "pre_liquidation_inventory": pre_liquidation_inventory,
            "cash": self.cash,
            "mark_price": self.mark_price,
            "wealth": self.wealth,
            "elapsed_steps": self.elapsed_steps,
            "feed_event_count": self.feed_event_count,
            "feature_event_count": self.feature_event_count,
            "source_event_id": self.last_event_id,
            "source_event_time": self.last_event_time,
            "source_time_watermark": self.source_time_watermark,
            "feed_synchronized": self.feed_synchronized,
            "active_order_count": len(self.own_orders.active_orders),
            "pending_order_count": len(self._pending),
            "activated_order_count": activated,
            "rejected_order_count": rejected,
            "activation_statuses": activation_statuses,
            "replacement_cancelled_order_count": replacement_cancelled,
            "pending_replaced_order_count": pending_replaced,
            "resnapshot_cancelled_order_count": resnapshot_cancelled,
            "uncertain_cross_cancelled_order_count": len(uncertain_cross_cancelled),
            "uncertain_cross_cancelled_order_ids": tuple(
                order.order_id for order in uncertain_cross_cancelled
            ),
            "ignored_uncertain_trade_evidence_count": (ignored_trade_evidence_count),
            "terminal_cancelled_order_count": terminal_cancelled,
            "feed_exhausted": feed_exhausted,
            "termination_reason": reason,
            "terminal_liquidation": None
            if liquidation is None
            else {
                "side": liquidation.side,
                "requested_quantity": liquidation.requested_quantity,
                "executed_quantity": liquidation.executed_quantity,
                "remaining_quantity": liquidation.remaining_quantity,
                "complete": liquidation.complete,
                "notional": liquidation.notional,
                "fee": liquidation.fee,
                "legs": tuple(
                    {"price": price, "quantity": quantity}
                    for price, quantity in liquidation.legs
                ),
            },
            "fill_model": FILL_MODEL_VERSION,
            "execution_quality": EXECUTION_QUALITY,
        }
        return self._raw_observation, reward, terminated, truncated, info

    def close(self) -> None:
        self._closed = True
        self._events = None
        super().close()


class MarketMakerRewardWrapper(gym.Wrapper):
    """Apply a causal inventory-risk penalty to exact portfolio PnL."""

    def __init__(self, env: gym.Env, *, inventory_penalty_rate: float = 0.01) -> None:
        super().__init__(env)
        if not math.isfinite(inventory_penalty_rate) or inventory_penalty_rate < 0:
            raise ValueError("inventory_penalty_rate must be finite and non-negative")
        self.inventory_penalty_rate = float(inventory_penalty_rate)

    def step(self, action: np.ndarray):
        observation, pnl, terminated, truncated, info = self.env.step(action)
        inventory_base = float(info["pre_liquidation_inventory"]) / float(
            self.env.unwrapped.config.quantity_scale
        )
        penalty = self.inventory_penalty_rate * inventory_base * inventory_base
        shaped = dict(info)
        shaped["raw_pnl"] = float(pnl)
        shaped["inventory_penalty"] = penalty
        return observation, float(pnl - penalty), terminated, truncated, shaped


__all__ = [
    "BUY_SIDE",
    "EXECUTION_QUALITY",
    "FILL_MODEL_VERSION",
    "INSTRUMENT_ID",
    "MARKET_DATA_SCHEMA_VERSION",
    "MakerFill",
    "MarketMakerRewardWrapper",
    "OwnOrder",
    "OwnOrderLedger",
    "PendingOrder",
    "ReplayConfig",
    "SELL_SIDE",
    "SequencedMarketEnv",
    "make_instrument_config",
]
