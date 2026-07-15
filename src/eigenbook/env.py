"""Gymnasium environment backed by Eigen-Book's fixed-capacity engine."""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

try:
    import gymnasium as gym
    from gymnasium import spaces
except ModuleNotFoundError as error:
    if error.name != "gymnasium":
        raise
    raise ModuleNotFoundError(
        "Eigen-Book's Gymnasium environment requires the optional RL extra; "
        "install it with `pip install 'eigenbook[rl]'`."
    ) from error

import numpy as np

from . import _eigenbook as eb


# Generic simulation assumptions expressed as decimal fractions.
# Override these for the target venue and the account's actual fee tier.
DEFAULT_MAKER_FEE_RATE = 0.0002
DEFAULT_TAKER_FEE_RATE = 0.0005

# BookEvent::Kind uses a stable uint8 wire representation in Types.hpp.
_TRADE_EVENT_KIND = 0
_ORDER_CANCELLED_EVENT_KIND = 3
_ORDER_MODIFIED_EVENT_KIND = 4
_ORDER_REJECTED_EVENT_KIND = 5

_BUY_SIDE_INDEX = 0
_SELL_SIDE_INDEX = 1

_KNOWN_EVENT_KINDS = frozenset(range(6))
_EXTERNAL_TRANSITION_STATUSES = frozenset(
    {
        eb.Status.ACCEPTED,
        eb.Status.CANCELLED,
        eb.Status.FILLED,
        eb.Status.FOK_REJECTED,
        eb.Status.NO_LIQUIDITY,
        eb.Status.ORDER_ID_MAP_FULL,
        eb.Status.PARTIALLY_FILLED,
        eb.Status.POOL_EXHAUSTED,
        eb.Status.POST_ONLY_WOULD_CROSS,
        eb.Status.SELF_TRADE_PREVENTED,
    }
)


@dataclass(frozen=True, slots=True)
class ExternalTransition:
    """One fully accounted external-flow transition.

    ``observation`` and ``info`` are borrowed environment-owned objects. Copy
    them before retaining the transition across subsequent calls.
    """

    observation: np.ndarray
    reward: float
    terminated: bool
    truncated: bool
    info: dict[str, Any]
    result: eb.DispatchResult


class ExternalDispatchError(RuntimeError):
    """A rejected external command with its native result attached."""

    def __init__(self, result: eb.DispatchResult, event_count: int) -> None:
        self.result = result
        self.event_count = event_count
        super().__init__(
            "external dispatch failed with status "
            f"{result.status} after emitting {event_count} event(s)"
        )


def _copy_instrument_config(
    source: eb.InstrumentConfig,
) -> eb.InstrumentConfig:
    """Detach environment configuration from subsequent caller mutations."""
    source_book = source.book_config
    book = eb.BookConfig()
    book.min_price = source_book.min_price
    book.max_price = source_book.max_price
    book.max_orders = source_book.max_orders
    book.order_id_map_capacity = source_book.order_id_map_capacity
    book.tick_size = source_book.tick_size
    book.event_log_capacity = source_book.event_log_capacity
    book.price_level_mode = source_book.price_level_mode
    book.lot_size = source_book.lot_size
    book.self_trade_policy = source_book.self_trade_policy
    book.market_data_capacity = source_book.market_data_capacity

    instrument = eb.InstrumentConfig()
    instrument.instrument_id = source.instrument_id
    instrument.book_config = book
    instrument.tick_size = source.tick_size
    instrument.lot_size = source.lot_size
    instrument.self_trade_policy = source.self_trade_policy
    instrument.market_data_capacity = source.market_data_capacity
    return instrument


def _validate_fee_rate(value: float, name: str) -> float:
    """Validate a non-negative, finite decimal fee rate."""
    if isinstance(value, bool) or not isinstance(
        value,
        (int, float, np.integer, np.floating),
    ):
        raise TypeError(f"{name} must be a real number")

    rate = float(value)
    if not math.isfinite(rate):
        raise ValueError(f"{name} must be finite")
    if rate < 0.0:
        raise ValueError(f"{name} must be non-negative")
    return rate


class LimitOrderBookEnv(gym.Env[np.ndarray, np.ndarray]):
    """Single-instrument limit-order submission environment.

    Observations have shape ``(2, 5, 2)`` and dtype ``float32``. Axis zero is
    bid/ask, axis one is best-to-worst, and the final axis is
    ``[price, aggregate_quantity]``. Empty levels are zeros.

    Actions are ``[side, price_offset, quantity]``. Side is zero for buy and
    one for sell. Price offsets are centered on the middle configured price
    level. Quantity code zero submits one configured lot.

    Cash and inventory are updated at each fill's exact execution price.
    Maker and taker fees are debited from cash. The baseline reward is the
    change in mark-to-market wealth:

        wealth = cash + inventory * level_1_mid
        reward = current_wealth - previous_wealth

    A true midpoint requires both Level 1 quotes. During empty or one-sided
    states, the last valid two-sided midpoint is retained. At reset, that mark
    is initialized to the configured reference price.

    External commands that may interact with agent orders must be submitted
    through :meth:`dispatch_external_transition` or :meth:`dispatch_external`.
    Dispatching directly through ``engine`` bypasses fill accounting.

    Prices, quantities, cash, and wealth use the engine's native units. The
    caller is responsible for applying any venue-specific price or quantity
    scale when interpreting them as dollars.

    Observation and info objects are reused. Copy them before retaining a
    transition.
    """

    metadata = {"render_modes": []}

    def __init__(
        self,
        instrument_config: eb.InstrumentConfig,
        *,
        max_price_offset_ticks: int = 10,
        max_order_quantity: int = 100,
        max_episode_steps: int | None = 1_000,
        max_abs_inventory: int | None = None,
        maker_fee_rate: float = DEFAULT_MAKER_FEE_RATE,
        taker_fee_rate: float = DEFAULT_TAKER_FEE_RATE,
    ) -> None:
        super().__init__()

        if isinstance(max_price_offset_ticks, bool) or not isinstance(
            max_price_offset_ticks,
            (int, np.integer),
        ):
            raise TypeError("max_price_offset_ticks must be an integer")
        if max_price_offset_ticks < 0:
            raise ValueError("max_price_offset_ticks must be a non-negative integer")

        if isinstance(max_order_quantity, bool) or not isinstance(
            max_order_quantity,
            (int, np.integer),
        ):
            raise TypeError("max_order_quantity must be an integer")
        if max_order_quantity <= 0:
            raise ValueError("max_order_quantity must be a positive integer")

        int64_max = int(np.iinfo(np.int64).max)
        uint64_max = int(np.iinfo(np.uint64).max)
        uint32_max = int(np.iinfo(np.uint32).max)

        if max_order_quantity > int64_max:
            raise ValueError("max_order_quantity exceeds Gymnasium int64 capacity")
        if (2 * max_price_offset_ticks) + 1 > int64_max:
            raise ValueError(
                "price-offset action count exceeds Gymnasium int64 capacity"
            )

        if max_episode_steps is not None:
            if isinstance(max_episode_steps, bool) or not isinstance(
                max_episode_steps,
                (int, np.integer),
            ):
                raise TypeError("max_episode_steps must be an integer or None")
            if max_episode_steps <= 0 or max_episode_steps >= uint64_max:
                raise ValueError(
                    "max_episode_steps must be positive and below uint64 capacity"
                )

        if max_abs_inventory is not None:
            if isinstance(max_abs_inventory, bool) or not isinstance(
                max_abs_inventory,
                (int, np.integer),
            ):
                raise TypeError("max_abs_inventory must be an integer or None")
            if max_abs_inventory <= 0:
                raise ValueError("max_abs_inventory must be positive")

        self.maker_fee_rate = _validate_fee_rate(
            maker_fee_rate,
            "maker_fee_rate",
        )
        self.taker_fee_rate = _validate_fee_rate(
            taker_fee_rate,
            "taker_fee_rate",
        )

        self.instrument_config = _copy_instrument_config(instrument_config)
        self.instrument_id = int(self.instrument_config.instrument_id)
        self.max_price_offset_ticks = int(max_price_offset_ticks)
        self.max_order_quantity = int(max_order_quantity)
        self.max_episode_steps = (
            None if max_episode_steps is None else int(max_episode_steps)
        )
        self.max_abs_inventory = (
            None if max_abs_inventory is None else int(max_abs_inventory)
        )
        self._quantity_step = max(1, int(self.instrument_config.lot_size))

        if self.max_order_quantity * self._quantity_step > uint64_max:
            raise ValueError(
                "max_order_quantity and lot_size exceed native uint64 capacity"
            )

        # Exact cash accounting requires every trade event. Ensure the private
        # environment copy has sufficient capacity for the largest possible
        # matching operation without mutating the caller's configuration.
        book_config = self.instrument_config.book_config
        required_event_capacity = int(book_config.max_orders) + 2
        if required_event_capacity > uint32_max:
            raise ValueError("max_orders is too large for the required event buffer")
        if int(book_config.event_log_capacity) < required_event_capacity:
            book_config.event_log_capacity = required_event_capacity
            self.instrument_config.book_config = book_config

        self._engine_configs = (self.instrument_config,)
        self.engine = eb.MatchingEngine(self._engine_configs)

        self._min_price = int(book_config.min_price)
        self._max_price = int(book_config.max_price)
        self._tick_size = int(book_config.tick_size)

        price_interval_count = (self._max_price - self._min_price) // self._tick_size
        self._reference_price = (
            self._min_price + (price_interval_count // 2) * self._tick_size
        )

        min_action_price = (
            self._reference_price - self.max_price_offset_ticks * self._tick_size
        )
        max_action_price = (
            self._reference_price + self.max_price_offset_ticks * self._tick_size
        )
        if min_action_price < self._min_price or max_action_price > self._max_price:
            raise ValueError(
                "max_price_offset_ticks produces prices outside the configured book"
            )

        self.event_buffer = np.empty(
            self.engine.event_buffer_capacity(self.instrument_id),
            dtype=eb.BOOK_EVENT_DTYPE,
        )

        depth_shape = (2, 5, 2)
        self._observation = np.zeros(depth_shape, dtype=np.float32)
        self._bid_depth = self._observation[0]
        self._ask_depth = self._observation[1]

        observation_low = np.zeros(depth_shape, dtype=np.float32)
        observation_high = np.empty(depth_shape, dtype=np.float32)
        observation_low[..., 0] = min(0, self._min_price)
        observation_high[..., 0] = max(0, self._max_price)
        observation_high[..., 1] = np.iinfo(np.uint64).max

        self.observation_space = spaces.Box(
            low=observation_low,
            high=observation_high,
            dtype=np.float32,
        )
        self.action_space = spaces.MultiDiscrete(
            np.array(
                (
                    2,
                    (2 * self.max_price_offset_ticks) + 1,
                    self.max_order_quantity,
                ),
                dtype=np.int64,
            ),
            dtype=np.int64,
        )

        self._command = eb.Command()
        self._command.instrument_id = self.instrument_id
        self._command.op = eb.CommandOp.ADD
        self._command.time_in_force = eb.TimeInForce.GTC
        self._sides = (eb.Side.BUY, eb.Side.SELL)

        self._next_order_id = 1
        self._next_timestamp = 1

        # order_id -> (side_index, remaining_quantity)
        self._agent_orders: dict[int, tuple[int, int]] = {}

        self.cash = 0.0
        self.inventory = 0
        self.mark_price = float(self._reference_price)
        self.wealth = 0.0
        self.fees_paid = 0.0
        self.elapsed_steps = 0

        self._last_valid_mid = float(self._reference_price)
        self._pending_maker_quantity = 0
        self._pending_taker_quantity = 0
        self._pending_maker_fees = 0.0
        self._pending_taker_fees = 0.0
        self._pending_external_event_count = 0

        self._has_reset = False
        self._episode_done = False
        self._closed = False

        self._step_info: dict[str, Any] = {
            "event_count": 0,
            "external_event_count": 0,
            "status": eb.Status.ACCEPTED,
            "executed_quantity": 0,
            "external_executed_quantity": 0,
            "external_resting_quantity": 0,
            "maker_executed_quantity": 0,
            "taker_executed_quantity": 0,
            "residual_quantity": 0,
            "resting_quantity": 0,
            "inventory": 0,
            "cash": 0.0,
            "mark_price": self.mark_price,
            "wealth": 0.0,
            "step_fees": 0.0,
            "maker_fees": 0.0,
            "taker_fees": 0.0,
            "fees_paid": 0.0,
            "elapsed_steps": 0,
        }
        self._external_info = dict(self._step_info)
        self._reset_info: dict[str, Any] = {
            "inventory": 0,
            "cash": 0.0,
            "mark_price": self.mark_price,
            "wealth": 0.0,
            "fees_paid": 0.0,
            "elapsed_steps": 0,
        }

    def _get_obs(self) -> np.ndarray:
        self.engine.depth(
            self.instrument_id,
            eb.Side.BUY,
            self._bid_depth,
        )
        self.engine.depth(
            self.instrument_id,
            eb.Side.SELL,
            self._ask_depth,
        )
        return self._observation

    def observe(self) -> np.ndarray:
        """Return the current reused depth observation without taking a step."""
        if self._closed:
            raise RuntimeError("cannot observe a closed environment")
        if not self._has_reset:
            raise RuntimeError("reset() must be called before observe()")
        return self._get_obs()

    def _current_mid_price(self) -> float:
        """Return the current two-sided L1 midpoint or the last valid mark."""
        top = self.engine.top_of_book(self.instrument_id)
        if top.status != eb.Status.ACCEPTED:
            raise RuntimeError(f"top-of-book lookup failed with status {top.status}")

        if top.bid.valid and top.ask.valid:
            bid = float(top.bid.price)
            ask = float(top.ask.price)
            self._last_valid_mid = bid + (ask - bid) * 0.5

        return self._last_valid_mid

    def _decode_action(
        self,
        action: np.ndarray,
    ) -> tuple[int, int, int]:
        action_array = np.asarray(action)
        if action_array.shape != (3,):
            raise ValueError("action must have shape (3,)")
        if not np.issubdtype(action_array.dtype, np.integer):
            raise TypeError("action values must have an integer dtype")

        side_index = int(action_array[0])
        offset_code = int(action_array[1])
        quantity_code = int(action_array[2])

        if (
            side_index < 0
            or side_index > 1
            or offset_code < 0
            or offset_code > 2 * self.max_price_offset_ticks
            or quantity_code < 0
            or quantity_code >= self.max_order_quantity
        ):
            raise ValueError("action is outside the configured action space")

        return side_index, offset_code, quantity_code

    def _decrease_open_quantity(
        self,
        order_id: int,
        fill_quantity: int,
    ) -> None:
        """Reduce an agent order's tracked open quantity after a fill."""
        order = self._agent_orders.get(order_id)
        if order is None:
            raise RuntimeError(f"fill references unknown agent order id {order_id}")

        side_index, remaining = order
        if fill_quantity > remaining:
            raise RuntimeError(
                f"fill quantity {fill_quantity} exceeds tracked quantity "
                f"{remaining} for agent order {order_id}"
            )

        remaining -= fill_quantity
        if remaining == 0:
            self._agent_orders.pop(order_id, None)
        else:
            self._agent_orders[order_id] = (side_index, remaining)

    def _record_fill(
        self,
        *,
        side_index: int,
        price: int,
        quantity: int,
        is_maker: bool,
    ) -> None:
        """Apply one exact execution to cash, inventory, and fee ledgers."""
        if quantity <= 0:
            raise RuntimeError("native trade event contains zero quantity")

        fee_rate = self.maker_fee_rate if is_maker else self.taker_fee_rate
        notional = float(price) * float(quantity)
        fee = abs(notional) * fee_rate

        if side_index == _BUY_SIDE_INDEX:
            self.inventory += quantity
            self.cash -= notional + fee
        elif side_index == _SELL_SIDE_INDEX:
            self.inventory -= quantity
            self.cash += notional - fee
        else:
            raise RuntimeError(f"invalid fill side index {side_index}")

        self.fees_paid += fee
        if is_maker:
            self._pending_maker_quantity += quantity
            self._pending_maker_fees += fee
        else:
            self._pending_taker_quantity += quantity
            self._pending_taker_fees += fee

    def _account_event_buffer(
        self,
        *,
        event_count: int,
        agent_aggressor_order_id: int | None,
    ) -> tuple[int, int]:
        """Account agent legs in the valid event-buffer prefix.

        Returns the total number and quantity of trade events emitted by the
        dispatched command. External aggressors are never inferred from
        order-id membership, preventing accidental attribution after
        identifier reuse.
        """
        if event_count < 0 or event_count > len(self.event_buffer):
            raise RuntimeError(
                f"native event count {event_count} exceeds buffer capacity "
                f"{len(self.event_buffer)}"
            )

        trade_count = 0
        trade_quantity = 0

        for index in range(event_count):
            event = self.event_buffer[index]
            kind = int(event["kind"])
            if kind not in _KNOWN_EVENT_KINDS:
                raise RuntimeError(
                    f"native event buffer contains unknown kind {kind} at index {index}"
                )

            if kind == _ORDER_CANCELLED_EVENT_KIND:
                cancelled_id = int(event["order_id"])
                self._agent_orders.pop(cancelled_id, None)
                continue

            if kind == _ORDER_MODIFIED_EVENT_KIND:
                modified_id = int(event["order_id"])
                tracked_order = self._agent_orders.get(modified_id)
                if tracked_order is not None:
                    side_index, remaining = tracked_order
                    old_quantity = int(event["old_quantity"])
                    new_quantity = int(event["new_quantity"])
                    if old_quantity != remaining or new_quantity > old_quantity:
                        raise RuntimeError(
                            "native modification diverged from tracked agent "
                            f"order {modified_id}: tracked={remaining}, "
                            f"old={old_quantity}, new={new_quantity}"
                        )
                    if new_quantity == 0:
                        self._agent_orders.pop(modified_id, None)
                    else:
                        self._agent_orders[modified_id] = (
                            side_index,
                            new_quantity,
                        )
                continue

            if kind != _TRADE_EVENT_KIND:
                continue

            trade = event["trade"]
            price = int(trade["price"])
            quantity = int(trade["quantity"])
            aggressor_id = int(trade["aggressor_id"])
            resting_id = int(trade["resting_id"])
            trade_count += 1
            trade_quantity += quantity

            if agent_aggressor_order_id is not None:
                if aggressor_id != agent_aggressor_order_id:
                    raise RuntimeError(
                        "native trade references the wrong dispatched "
                        f"aggressor: expected={agent_aggressor_order_id}, "
                        f"actual={aggressor_id}"
                    )
                aggressor_order = self._agent_orders.get(aggressor_id)
                if aggressor_order is None:
                    raise RuntimeError(
                        "native trade references an untracked agent aggressor"
                    )

                aggressor_side, _ = aggressor_order
                self._record_fill(
                    side_index=aggressor_side,
                    price=price,
                    quantity=quantity,
                    is_maker=False,
                )
                self._decrease_open_quantity(aggressor_id, quantity)

            resting_order = self._agent_orders.get(resting_id)
            if resting_order is not None:
                resting_side, _ = resting_order
                self._record_fill(
                    side_index=resting_side,
                    price=price,
                    quantity=quantity,
                    is_maker=True,
                )
                self._decrease_open_quantity(resting_id, quantity)

        return trade_count, trade_quantity

    @staticmethod
    def _reconcile_dispatch_result(
        result: eb.DispatchResult,
        *,
        trade_count: int,
        trade_quantity: int,
    ) -> None:
        """Require the native result and emitted trade prefix to agree."""

        native_fill_count = int(result.fills)
        native_executed_quantity = int(result.executed_quantity)
        if trade_count != native_fill_count:
            raise RuntimeError(
                "fill-ledger mismatch: native result reports "
                f"{native_fill_count} fills but emitted {trade_count} "
                "trade events"
            )
        if trade_quantity != native_executed_quantity:
            raise RuntimeError(
                "fill-ledger mismatch: native result reports "
                f"{native_executed_quantity} executed units but emitted "
                f"trade events totaling {trade_quantity}"
            )

    def _dispatch_external_accounted(
        self,
        command: Any,
        *,
        require_transition_status: bool,
    ) -> tuple[eb.DispatchResult, int]:
        """Dispatch and reconcile one external command exactly once."""

        result = self.engine.dispatch_result_with_buffer(
            command,
            self.event_buffer,
        )
        event_count = int(result.events_emitted)
        if event_count < 0 or event_count > len(self.event_buffer):
            raise RuntimeError(
                f"native event count {event_count} exceeds buffer capacity "
                f"{len(self.event_buffer)}"
            )

        if (
            require_transition_status
            and result.status not in _EXTERNAL_TRANSITION_STATUSES
        ):
            for index in range(event_count):
                kind = int(self.event_buffer[index]["kind"])
                if kind not in _KNOWN_EVENT_KINDS:
                    raise RuntimeError(
                        f"rejected external dispatch emitted unknown event "
                        f"kind {kind} at index {index}"
                    )
                if kind != _ORDER_REJECTED_EVENT_KIND:
                    raise RuntimeError(
                        "rejected external dispatch emitted non-rejection "
                        f"event kind {kind} at index {index}"
                    )
            if int(result.fills) != 0 or int(result.executed_quantity) != 0:
                raise RuntimeError("rejected external dispatch reported executed flow")
            raise ExternalDispatchError(result, event_count)

        trade_count, trade_quantity = self._account_event_buffer(
            event_count=event_count,
            agent_aggressor_order_id=None,
        )
        self._reconcile_dispatch_result(
            result,
            trade_count=trade_count,
            trade_quantity=trade_quantity,
        )
        self._pending_external_event_count += event_count
        return result, event_count

    def _drain_pending_accounting(
        self,
    ) -> tuple[int, int, float, float, int]:
        """Return and clear quantities, fees, and external event count."""

        pending = (
            self._pending_maker_quantity,
            self._pending_taker_quantity,
            self._pending_maker_fees,
            self._pending_taker_fees,
            self._pending_external_event_count,
        )
        self._pending_maker_quantity = 0
        self._pending_taker_quantity = 0
        self._pending_maker_fees = 0.0
        self._pending_taker_fees = 0.0
        self._pending_external_event_count = 0
        return pending

    def dispatch_external(self, command: Any) -> Any:
        """Dispatch external flow with deferred transition reporting.

        This compatibility API immediately books agent cash, inventory, and
        fees, but reports mark, wealth, reward, termination, and fill info on
        the next :meth:`step`. New feed/replay integrations should use
        :meth:`dispatch_external_transition` so the same call returns the
        complete financial transition.
        """
        if self._closed:
            raise RuntimeError(
                "cannot dispatch external flow into a closed environment"
            )
        if not self._has_reset:
            raise RuntimeError(
                "reset() must be called before dispatching external flow"
            )
        if self._episode_done:
            raise RuntimeError(
                "the episode has ended; call reset() before dispatching external flow"
            )

        result, _ = self._dispatch_external_accounted(
            command,
            require_transition_status=False,
        )
        return result

    def dispatch_external_transition(
        self,
        command: Any,
    ) -> ExternalTransition:
        """Dispatch external flow and return its complete financial effect.

        The external aggressor is never attributed to the agent. Any fills
        against tracked resting agent orders are maker executions. Native
        status, fill count, executed quantity, and the valid event-buffer
        prefix are checked before the transition is returned. Rejected native
        statuses raise :class:`ExternalDispatchError`, whose ``result`` field
        preserves the checked dispatch result.

        External transitions do not increment the agent-action step counter,
        so they cannot trigger a time-limit truncation. They can terminate the
        episode immediately when the configured inventory boundary is reached.
        """

        if self._closed:
            raise RuntimeError(
                "cannot dispatch external flow into a closed environment"
            )
        if not self._has_reset:
            raise RuntimeError(
                "reset() must be called before dispatching external flow"
            )
        if self._episode_done:
            raise RuntimeError(
                "the episode has ended; call reset() before dispatching external flow"
            )

        previous_wealth = self.wealth
        self._current_mid_price()
        result, event_count = self._dispatch_external_accounted(
            command,
            require_transition_status=True,
        )

        self.mark_price = self._current_mid_price()
        self.wealth = self.cash + self.inventory * self.mark_price
        reward = float(self.wealth - previous_wealth)
        terminated = (
            self.max_abs_inventory is not None
            and abs(self.inventory) >= self.max_abs_inventory
        )
        truncated = False
        self._episode_done = terminated

        (
            maker_quantity,
            taker_quantity,
            maker_fees,
            taker_fees,
            external_event_count,
        ) = self._drain_pending_accounting()
        self._external_info.update(
            event_count=event_count,
            external_event_count=external_event_count,
            status=result.status,
            executed_quantity=0,
            external_executed_quantity=int(result.executed_quantity),
            external_resting_quantity=int(result.resting_quantity),
            maker_executed_quantity=maker_quantity,
            taker_executed_quantity=taker_quantity,
            residual_quantity=0,
            resting_quantity=0,
            inventory=self.inventory,
            cash=self.cash,
            mark_price=self.mark_price,
            wealth=self.wealth,
            step_fees=maker_fees + taker_fees,
            maker_fees=maker_fees,
            taker_fees=taker_fees,
            fees_paid=self.fees_paid,
            elapsed_steps=self.elapsed_steps,
        )
        return ExternalTransition(
            observation=self._get_obs(),
            reward=reward,
            terminated=terminated,
            truncated=truncated,
            info=self._external_info,
            result=result,
        )

    def reset(
        self,
        *,
        seed: int | None = None,
        options: dict[str, Any] | None = None,
    ) -> tuple[np.ndarray, dict[str, Any]]:
        """Reset to an empty book and a fresh financial ledger.

        Supported options are ``initial_inventory`` and ``initial_cash``.
        """
        if self._closed:
            raise RuntimeError("cannot reset a closed environment")

        super().reset(seed=seed)
        if seed is not None:
            self.action_space.seed(seed)

        if options is None:
            reset_options: Mapping[str, Any] = {}
        elif isinstance(options, Mapping):
            reset_options = options
        else:
            raise TypeError("options must be a mapping or None")

        unknown_options = set(reset_options) - {
            "initial_inventory",
            "initial_cash",
        }
        if unknown_options:
            unknown = ", ".join(sorted(str(key) for key in unknown_options))
            raise ValueError(f"unsupported reset option(s): {unknown}")

        initial_inventory = reset_options.get("initial_inventory", 0)
        if isinstance(initial_inventory, bool) or not isinstance(
            initial_inventory,
            (int, np.integer),
        ):
            raise TypeError("initial_inventory must be an integer")
        initial_inventory = int(initial_inventory)

        if (
            self.max_abs_inventory is not None
            and abs(initial_inventory) >= self.max_abs_inventory
        ):
            raise ValueError(
                "initial_inventory must be inside the termination boundary"
            )

        initial_cash = reset_options.get("initial_cash", 0.0)
        if isinstance(initial_cash, bool) or not isinstance(
            initial_cash,
            (int, float, np.integer, np.floating),
        ):
            raise TypeError("initial_cash must be a real number")
        initial_cash = float(initial_cash)
        if not math.isfinite(initial_cash):
            raise ValueError("initial_cash must be finite")

        self.engine = eb.MatchingEngine(self._engine_configs)
        self.event_buffer.fill(0)
        self._observation.fill(0.0)
        self._agent_orders.clear()

        self._next_order_id = 1
        self._next_timestamp = 1

        self.cash = initial_cash
        self.inventory = initial_inventory
        self.fees_paid = 0.0
        self.elapsed_steps = 0

        self._last_valid_mid = float(self._reference_price)
        self.mark_price = self._current_mid_price()
        self.wealth = self.cash + self.inventory * self.mark_price

        self._pending_maker_quantity = 0
        self._pending_taker_quantity = 0
        self._pending_maker_fees = 0.0
        self._pending_taker_fees = 0.0
        self._pending_external_event_count = 0

        self._episode_done = False
        self._has_reset = True

        self._step_info.update(
            event_count=0,
            external_event_count=0,
            status=eb.Status.ACCEPTED,
            executed_quantity=0,
            external_executed_quantity=0,
            external_resting_quantity=0,
            maker_executed_quantity=0,
            taker_executed_quantity=0,
            residual_quantity=0,
            resting_quantity=0,
            inventory=self.inventory,
            cash=self.cash,
            mark_price=self.mark_price,
            wealth=self.wealth,
            step_fees=0.0,
            maker_fees=0.0,
            taker_fees=0.0,
            fees_paid=0.0,
            elapsed_steps=0,
        )
        self._external_info.update(self._step_info)
        self._reset_info.update(
            inventory=self.inventory,
            cash=self.cash,
            mark_price=self.mark_price,
            wealth=self.wealth,
            fees_paid=0.0,
            elapsed_steps=0,
        )

        return self._get_obs(), self._reset_info

    def step(
        self,
        action: np.ndarray,
    ) -> tuple[np.ndarray, float, bool, bool, dict[str, Any]]:
        """Submit one agent GTC limit order and return mark-to-market PnL."""
        if self._closed:
            raise RuntimeError("cannot step a closed environment")
        if not self._has_reset:
            raise RuntimeError("reset() must be called before step()")
        if self._episode_done:
            raise RuntimeError(
                "the episode has ended; call reset() before another step()"
            )

        side_index, offset_code, quantity_code = self._decode_action(action)
        offset_ticks = offset_code - self.max_price_offset_ticks
        quantity = (quantity_code + 1) * self._quantity_step

        previous_wealth = self.wealth

        # Capture any valid two-sided midpoint established by external flow
        # before this action can consume one side of the market. The previous
        # wealth remains unchanged, so intervening market moves still accrue
        # to this step's reward; this call only advances the one-sided fallback
        # mark used after an aggressive fill removes the best quote.
        self._current_mid_price()
        order_id = self._next_order_id

        command = self._command
        command.order_id = order_id
        command.side = self._sides[side_index]
        command.price = self._reference_price + offset_ticks * self._tick_size
        command.quantity = quantity
        command.timestamp = self._next_timestamp

        # Register before dispatch so trade events can attribute the aggressive
        # leg. Rejected orders are removed after the result is inspected.
        self._agent_orders[order_id] = (side_index, quantity)

        try:
            result = self.engine.dispatch_result_with_buffer(
                command,
                self.event_buffer,
            )
        except (RuntimeError, TypeError, ValueError, OverflowError):
            self._agent_orders.pop(order_id, None)
            raise

        accepted_statuses = {
            eb.Status.ACCEPTED,
            eb.Status.FILLED,
            eb.Status.PARTIALLY_FILLED,
            eb.Status.POOL_EXHAUSTED,
            eb.Status.ORDER_ID_MAP_FULL,
            eb.Status.SELF_TRADE_PREVENTED,
        }
        if result.status not in accepted_statuses:
            self._agent_orders.pop(order_id, None)
            raise RuntimeError(f"native dispatch failed with status {result.status}")

        event_count = int(result.events_emitted)
        trade_count, trade_quantity = self._account_event_buffer(
            event_count=event_count,
            agent_aggressor_order_id=order_id,
        )
        executed_quantity = int(result.executed_quantity)
        self._reconcile_dispatch_result(
            result,
            trade_count=trade_count,
            trade_quantity=trade_quantity,
        )

        resting_quantity = int(result.resting_quantity)
        if resting_quantity > 0:
            self._agent_orders[order_id] = (
                side_index,
                resting_quantity,
            )
        else:
            self._agent_orders.pop(order_id, None)

        residual_quantity = quantity - executed_quantity

        self.elapsed_steps += 1
        self._next_order_id += 1
        self._next_timestamp += 1

        self.mark_price = self._current_mid_price()
        self.wealth = self.cash + self.inventory * self.mark_price
        reward = float(self.wealth - previous_wealth)

        terminated = (
            self.max_abs_inventory is not None
            and abs(self.inventory) >= self.max_abs_inventory
        )
        truncated = (
            not terminated
            and self.max_episode_steps is not None
            and self.elapsed_steps >= self.max_episode_steps
        )
        self._episode_done = terminated or truncated

        (
            maker_quantity,
            taker_quantity,
            maker_fees,
            taker_fees,
            external_event_count,
        ) = self._drain_pending_accounting()
        step_fees = maker_fees + taker_fees

        self._step_info.update(
            event_count=event_count,
            external_event_count=external_event_count,
            status=result.status,
            executed_quantity=executed_quantity,
            external_executed_quantity=0,
            external_resting_quantity=0,
            maker_executed_quantity=maker_quantity,
            taker_executed_quantity=taker_quantity,
            residual_quantity=residual_quantity,
            resting_quantity=resting_quantity,
            inventory=self.inventory,
            cash=self.cash,
            mark_price=self.mark_price,
            wealth=self.wealth,
            step_fees=step_fees,
            maker_fees=maker_fees,
            taker_fees=taker_fees,
            fees_paid=self.fees_paid,
            elapsed_steps=self.elapsed_steps,
        )

        return (
            self._get_obs(),
            reward,
            terminated,
            truncated,
            self._step_info,
        )

    def close(self) -> None:
        self._closed = True
        super().close()
