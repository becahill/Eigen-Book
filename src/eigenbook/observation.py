"""Versioned causal observation contract shared by every policy surface.

The first six values are written by :class:`eigenbook.features.FeatureExtractor`.
The remaining values describe inventory, working quotes, and in-flight latency
without exposing mutable engine buffers or absolute non-stationary prices.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Final

import gymnasium as gym
import numpy as np

from .features import FEATURE_NAMES, STATE_SIZE


OBSERVATION_SCHEMA_VERSION: Final = "eigenbook.causal_depth_observation.v2"
ACTION_SCHEMA_VERSION: Final = "eigenbook.passive_quote_action.v1"

OBSERVATION_NAMES: Final = FEATURE_NAMES + (
    "inventory_fraction",
    "active_bid_present",
    "active_bid_distance",
    "active_bid_quantity",
    "active_bid_queue_ahead",
    "active_ask_present",
    "active_ask_distance",
    "active_ask_quantity",
    "active_ask_queue_ahead",
    "pending_bid_present",
    "pending_bid_distance",
    "pending_bid_quantity",
    "pending_bid_latency",
    "pending_ask_present",
    "pending_ask_distance",
    "pending_ask_quantity",
    "pending_ask_latency",
)
OBSERVATION_SIZE: Final = len(OBSERVATION_NAMES)

ACTION_NAMES: Final = (
    "side",
    "passive_distance_ticks",
    "quantity_code",
)

BUY_SIDE: Final = 0
SELL_SIDE: Final = 1


@dataclass(frozen=True, slots=True)
class QuoteState:
    """Minimal working-order state needed by the observation encoder."""

    price: int
    quantity: int
    queue_ahead: int


@dataclass(frozen=True, slots=True)
class PendingQuoteState:
    """Minimal in-flight order state needed by the observation encoder."""

    price: int
    quantity: int
    remaining_events: int


@dataclass(frozen=True, slots=True)
class ObservationInput:
    """Exact state used to construct one canonical policy observation."""

    market_features: np.ndarray
    inventory: int
    inventory_limit: int | None
    quantity_scale: int
    best_bid: int | None
    best_ask: int | None
    tick_size: int
    max_quote_distance_ticks: int
    max_order_quantity: int
    latency_events: int
    active_bid: QuoteState | None = None
    active_ask: QuoteState | None = None
    pending_bid: PendingQuoteState | None = None
    pending_ask: PendingQuoteState | None = None


def observation_space() -> gym.spaces.Box:
    """Return the exact bounded space for the versioned observation."""

    return gym.spaces.Box(
        low=-1.0,
        high=1.0,
        shape=(OBSERVATION_SIZE,),
        dtype=np.float32,
    )


class CanonicalObservationEncoder:
    """Encode exact simulator state into the shared bounded float32 layout."""

    schema_version: Final = OBSERVATION_SCHEMA_VERSION
    names: Final = OBSERVATION_NAMES

    def __init__(self) -> None:
        self._state = np.zeros(OBSERVATION_SIZE, dtype=np.float32)

    @staticmethod
    def _validate(inputs: ObservationInput) -> None:
        features = np.asarray(inputs.market_features)
        if features.shape != (STATE_SIZE,):
            raise ValueError(
                f"market_features must have shape ({STATE_SIZE},), "
                f"received {features.shape}"
            )
        if not np.isfinite(features).all():
            raise ValueError("market_features must be finite")
        for name, value in (
            ("quantity_scale", inputs.quantity_scale),
            ("tick_size", inputs.tick_size),
            ("max_order_quantity", inputs.max_order_quantity),
        ):
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")
        if (
            isinstance(inputs.max_quote_distance_ticks, bool)
            or not isinstance(inputs.max_quote_distance_ticks, int)
            or inputs.max_quote_distance_ticks < 0
        ):
            raise ValueError("max_quote_distance_ticks must be a non-negative integer")
        if (
            isinstance(inputs.latency_events, bool)
            or not isinstance(inputs.latency_events, int)
            or inputs.latency_events < 0
        ):
            raise ValueError("latency_events must be a non-negative integer")
        if inputs.inventory_limit is not None and inputs.inventory_limit <= 0:
            raise ValueError("inventory_limit must be positive or None")

    @staticmethod
    def _inventory_fraction(inputs: ObservationInput) -> float:
        if inputs.inventory_limit is not None:
            return float(inputs.inventory) / float(inputs.inventory_limit)
        return math.tanh(float(inputs.inventory) / float(inputs.quantity_scale))

    @staticmethod
    def _distance(
        *,
        side: int,
        price: int,
        best_bid: int | None,
        best_ask: int | None,
        tick_size: int,
        max_distance_ticks: int,
    ) -> float:
        reference = best_bid if side == BUY_SIDE else best_ask
        if reference is None:
            return 0.0
        native_distance = reference - price if side == BUY_SIDE else price - reference
        scale_ticks = max(1, max_distance_ticks)
        return native_distance / float(tick_size * scale_ticks)

    @staticmethod
    def _quantity(quantity: int, maximum: int) -> float:
        if quantity < 0:
            raise ValueError("quote quantity cannot be negative")
        return float(quantity) / float(maximum)

    def encode(
        self,
        inputs: ObservationInput,
        output: np.ndarray | None = None,
    ) -> np.ndarray:
        """Write one observation and return the reused or supplied buffer."""

        self._validate(inputs)
        state = self._state if output is None else output
        if state.shape != (OBSERVATION_SIZE,) or state.dtype != np.float32:
            raise ValueError(
                f"output must be a float32 vector with shape ({OBSERVATION_SIZE},)"
            )
        if not state.flags.c_contiguous:
            raise ValueError("output must be C-contiguous")

        state[:STATE_SIZE] = inputs.market_features
        state[STATE_SIZE] = self._inventory_fraction(inputs)
        cursor = STATE_SIZE + 1

        def write_active(side: int, quote: QuoteState | None) -> None:
            nonlocal cursor
            if quote is None:
                state[cursor : cursor + 4] = 0.0
            else:
                if quote.queue_ahead < 0:
                    raise ValueError("quote queue_ahead cannot be negative")
                state[cursor : cursor + 4] = (
                    1.0,
                    self._distance(
                        side=side,
                        price=quote.price,
                        best_bid=inputs.best_bid,
                        best_ask=inputs.best_ask,
                        tick_size=inputs.tick_size,
                        max_distance_ticks=inputs.max_quote_distance_ticks,
                    ),
                    self._quantity(quote.quantity, inputs.max_order_quantity),
                    math.tanh(
                        float(quote.queue_ahead) / float(inputs.max_order_quantity)
                    ),
                )
            cursor += 4

        def write_pending(side: int, quote: PendingQuoteState | None) -> None:
            nonlocal cursor
            if quote is None:
                state[cursor : cursor + 4] = 0.0
            else:
                latency_scale = max(1, inputs.latency_events)
                state[cursor : cursor + 4] = (
                    1.0,
                    self._distance(
                        side=side,
                        price=quote.price,
                        best_bid=inputs.best_bid,
                        best_ask=inputs.best_ask,
                        tick_size=inputs.tick_size,
                        max_distance_ticks=inputs.max_quote_distance_ticks,
                    ),
                    self._quantity(quote.quantity, inputs.max_order_quantity),
                    quote.remaining_events / float(latency_scale),
                )
            cursor += 4

        write_active(BUY_SIDE, inputs.active_bid)
        write_active(SELL_SIDE, inputs.active_ask)
        write_pending(BUY_SIDE, inputs.pending_bid)
        write_pending(SELL_SIDE, inputs.pending_ask)

        np.clip(state, -1.0, 1.0, out=state)
        if not np.isfinite(state).all():
            raise RuntimeError("canonical observation contains a non-finite value")
        return state


class CanonicalObservationWrapper(gym.ObservationWrapper):
    """Expose ``env.unwrapped.write_canonical_observation`` to Gymnasium."""

    def __init__(self, env: gym.Env) -> None:
        super().__init__(env)
        provider = getattr(self.env.unwrapped, "write_canonical_observation", None)
        if not callable(provider):
            raise TypeError(
                "wrapped environment must implement write_canonical_observation(output)"
            )
        self._provider = provider
        self._observation = np.zeros(OBSERVATION_SIZE, dtype=np.float32)
        self.observation_space = observation_space()

    def observation(self, observation: np.ndarray) -> np.ndarray:
        del observation
        result = self._provider(self._observation)
        if result is not self._observation:
            raise RuntimeError(
                "write_canonical_observation must return the supplied buffer"
            )
        if not self.observation_space.contains(result):
            raise RuntimeError("canonical observation is outside its declared space")
        return result


__all__ = [
    "ACTION_NAMES",
    "ACTION_SCHEMA_VERSION",
    "CanonicalObservationEncoder",
    "CanonicalObservationWrapper",
    "OBSERVATION_NAMES",
    "OBSERVATION_SCHEMA_VERSION",
    "OBSERVATION_SIZE",
    "ObservationInput",
    "PendingQuoteState",
    "QuoteState",
    "observation_space",
]
