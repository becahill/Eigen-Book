"""Optional Gymnasium environment backed by Eigen-Book."""

from __future__ import annotations

from collections.abc import Mapping
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


def _copy_instrument_config(
    source: eb.InstrumentConfig,
) -> eb.InstrumentConfig:
    """Detach environment resets from subsequent caller mutations."""
    source_book = source.book_config
    book = eb.BookConfig()
    book.min_price = source_book.min_price
    book.max_price = source_book.max_price
    book.max_orders = source_book.max_orders
    book.order_id_map_capacity = source_book.order_id_map_capacity
    book.tick_size = source_book.tick_size
    book.event_log_capacity = source_book.event_log_capacity
    book.price_level_mode = source_book.price_level_mode

    instrument = eb.InstrumentConfig()
    instrument.instrument_id = source.instrument_id
    instrument.book_config = book
    instrument.tick_size = source.tick_size
    instrument.lot_size = source.lot_size
    return instrument


class LimitOrderBookEnv(gym.Env[np.ndarray, np.ndarray]):
    """Single-instrument limit-order submission environment.

    Observations have shape ``(2, 5, 2)`` and dtype ``float32``. Axis zero is
    bid/ask, axis one is best-to-worst, and the final axis is
    ``[price, aggregate_quantity]``. Empty levels are zeros. Float32 depth is a
    compact learning representation and can lose integer precision.

    Actions are ``[side, price_offset, quantity]``. Side is 0 for buy and 1 for
    sell. Price offsets are centered on the middle configured price level.
    Quantity code zero submits one native quantity unit.

    Reward is ``executed_quantity - residual_quantity``. Inventory counts only
    the submitted order's aggressive executions: buy fills add inventory and
    sell fills subtract it. Previously resting orders are treated as book
    liquidity for this accounting, even when an earlier action created them.
    Reaching ``max_abs_inventory`` terminates an episode. Reaching
    ``max_episode_steps`` first truncates it.

    The observation and info dictionaries are reused. Copy them before
    retaining transitions.
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
    ) -> None:
        super().__init__()

        if isinstance(max_price_offset_ticks, bool) or not isinstance(
            max_price_offset_ticks, (int, np.integer)
        ):
            raise TypeError("max_price_offset_ticks must be an integer")
        if max_price_offset_ticks < 0:
            raise ValueError("max_price_offset_ticks must be a non-negative integer")
        if isinstance(max_order_quantity, bool) or not isinstance(
            max_order_quantity, (int, np.integer)
        ):
            raise TypeError("max_order_quantity must be an integer")
        if max_order_quantity <= 0:
            raise ValueError("max_order_quantity must be a positive integer")
        int64_max = int(np.iinfo(np.int64).max)
        uint64_max = int(np.iinfo(np.uint64).max)
        if max_order_quantity > int64_max:
            raise ValueError("max_order_quantity exceeds Gymnasium int64 capacity")
        if (2 * max_price_offset_ticks) + 1 > int64_max:
            raise ValueError("price-offset action count exceeds Gymnasium int64 capacity")
        if max_episode_steps is not None:
            if isinstance(max_episode_steps, bool) or not isinstance(
                max_episode_steps, (int, np.integer)
            ):
                raise TypeError("max_episode_steps must be an integer or None")
            if max_episode_steps <= 0 or max_episode_steps >= uint64_max:
                raise ValueError(
                    "max_episode_steps must be positive and below uint64 capacity"
                )
        if max_abs_inventory is not None:
            if isinstance(max_abs_inventory, bool) or not isinstance(
                max_abs_inventory, (int, np.integer)
            ):
                raise TypeError("max_abs_inventory must be an integer or None")
            if max_abs_inventory <= 0:
                raise ValueError("max_abs_inventory must be positive")

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

        book_config = self.instrument_config.book_config
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
            self._reference_price
            - self.max_price_offset_ticks * self._tick_size
        )
        max_action_price = (
            self._reference_price
            + self.max_price_offset_ticks * self._tick_size
        )
        if (
            min_action_price < self._min_price
            or max_action_price > self._max_price
        ):
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
                [
                    2,
                    (2 * self.max_price_offset_ticks) + 1,
                    self.max_order_quantity,
                ],
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
        self.inventory = 0
        self.elapsed_steps = 0
        self._has_reset = False
        self._episode_done = False
        self._closed = False

        self._step_info: dict[str, Any] = {
            "event_count": 0,
            "status": eb.Status.ACCEPTED,
            "executed_quantity": 0,
            "residual_quantity": 0,
            "resting_quantity": 0,
            "inventory": 0,
            "elapsed_steps": 0,
        }
        self._reset_info: dict[str, Any] = {
            "inventory": 0,
            "elapsed_steps": 0,
        }

    def _get_obs(self) -> np.ndarray:
        self.engine.depth(self.instrument_id, eb.Side.BUY, self._bid_depth)
        self.engine.depth(self.instrument_id, eb.Side.SELL, self._ask_depth)
        return self._observation

    def _decode_action(self, action: np.ndarray) -> tuple[int, int, int]:
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

    def reset(
        self,
        *,
        seed: int | None = None,
        options: dict[str, Any] | None = None,
    ) -> tuple[np.ndarray, dict[str, Any]]:
        """Reset to an empty book.

        ``options`` supports ``initial_inventory``. Unknown options are
        rejected so configuration mistakes cannot silently change an episode.
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
        unknown_options = set(reset_options) - {"initial_inventory"}
        if unknown_options:
            unknown = ", ".join(sorted(str(key) for key in unknown_options))
            raise ValueError(f"unsupported reset option(s): {unknown}")

        initial_inventory = reset_options.get("initial_inventory", 0)
        if isinstance(initial_inventory, bool) or not isinstance(
            initial_inventory, (int, np.integer)
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

        self.engine = eb.MatchingEngine(self._engine_configs)
        self.event_buffer.fill(0)
        self._observation.fill(0.0)
        self._next_order_id = 1
        self._next_timestamp = 1
        self.inventory = initial_inventory
        self.elapsed_steps = 0
        self._episode_done = False
        self._has_reset = True
        self._step_info.update(
            event_count=0,
            status=eb.Status.ACCEPTED,
            executed_quantity=0,
            residual_quantity=0,
            resting_quantity=0,
            inventory=self.inventory,
            elapsed_steps=0,
        )
        self._reset_info.update(
            inventory=self.inventory,
            elapsed_steps=0,
        )
        return self._get_obs(), self._reset_info

    def step(
        self,
        action: np.ndarray,
    ) -> tuple[np.ndarray, float, bool, bool, dict[str, Any]]:
        """Submit one GTC limit order."""
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
        quantity = quantity_code + 1

        command = self._command
        command.order_id = self._next_order_id
        command.side = self._sides[side_index]
        command.price = self._reference_price + offset_ticks * self._tick_size
        command.quantity = quantity
        command.timestamp = self._next_timestamp

        result = self.engine.dispatch_result_with_buffer(
            command,
            self.event_buffer,
        )
        if result.status not in {
            eb.Status.ACCEPTED,
            eb.Status.FILLED,
            eb.Status.PARTIALLY_FILLED,
            eb.Status.POOL_EXHAUSTED,
            eb.Status.ORDER_ID_MAP_FULL,
        }:
            raise RuntimeError(f"native dispatch failed with status {result.status}")

        executed_quantity = int(result.executed_quantity)
        residual_quantity = quantity - executed_quantity
        reward = float(executed_quantity - residual_quantity)
        self.inventory += executed_quantity if side_index == 0 else -executed_quantity
        self.elapsed_steps += 1
        self._next_order_id += 1
        self._next_timestamp += 1

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

        self._step_info.update(
            event_count=int(result.events_emitted),
            status=result.status,
            executed_quantity=executed_quantity,
            residual_quantity=residual_quantity,
            resting_quantity=int(result.resting_quantity),
            inventory=self.inventory,
            elapsed_steps=self.elapsed_steps,
        )
        return self._get_obs(), reward, terminated, truncated, self._step_info

    def close(self) -> None:
        self._closed = True
        super().close()
