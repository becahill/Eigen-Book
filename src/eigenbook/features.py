"""Causal microstructure features for Eigen-Book's sequenced depth replay.

``FeatureExtractor`` consumes one atomic absolute-size depth message at a
time.  It keeps an exact fixed-point book mirror because Cont--Kukanov--Stoikov
order-flow imbalance (OFI) must compare every consecutive best quote; it cannot
be recovered from the final observation after several feed ticks.

The dictionary boundary accepts these schemas::

    {
        "type": "snapshot",
        "bids": {10_000: 12_000, 9_999: 8_000},
        "asks": {10_001: 10_000, 10_002: 7_000},
    }

    {
        "type": "update",  # "l2update" is also accepted
        "changes": (
            {"side": "bid", "price": 10_000, "size": 15_000},
            {"side": "ask", "price": 10_001, "size": 0},
        ),
    }

An incremental message may instead contain one root-level
``side``/``price``/``size`` change or grouped ``bids``/``asks`` maps.  Sizes
are absolute; zero deletes.  Prices must be exact positive integers, matching
the replay book's fixed-point representation.

``update_overlay`` is a compatibility adapter for objects exposing the same
atomic batch kind and level records.  Trade records may be mappings or
``SyntheticFill``-like objects.  Explicit ``aggressor_side`` is preferred.  A
generic ``side``/``side_index`` is treated as the aggressor side by default;
legacy resting-order fill adapters must opt into the inverse interpretation.

``get_state()`` returns a reused C-contiguous ``float32`` vector:

``[OFI, microprice pressure, microprice drift, spread, known-depth imbalance, trade flow]``

Every value is bounded and dimensionless.  The state contains no absolute
price or volume level and uses no adaptive statistic that could leak future
information or change between training and evaluation.  These transformations
remove the usual unit roots and make the representation stationarity-oriented;
strict statistical stationarity still depends on the data-generating process.
"""

from __future__ import annotations

import heapq
import math
from collections.abc import Iterable, Mapping
from typing import Any, Final, TypeAlias

import numpy as np


BUY_SIDE: Final = 0
SELL_SIDE: Final = 1
TOP_DEPTH_LEVELS: Final = 5

FEATURE_NAMES: Final = (
    "order_flow_imbalance",
    "microprice_pressure",
    "microprice_drift",
    "spread",
    "known_depth_imbalance_up_to_5",
    "trade_flow_imbalance",
)
STATE_SIZE: Final = len(FEATURE_NAMES)

_OFI: Final = 0
_DEPTH: Final = 1
_VALID_OFI: Final = 2
_SIGNED_TRADE: Final = 3
_TRADE_VOLUME: Final = 4
_RING_WIDTH: Final = 5

_INT64_MAX: Final = (1 << 63) - 1
_UINT64_MAX: Final = (1 << 64) - 1
_MISSING: Final = object()

Number: TypeAlias = int | float
Book: TypeAlias = dict[int, Number]
Change: TypeAlias = tuple[int, int, Number]
Bbo: TypeAlias = tuple[int, Number, int, Number]


class FeatureExtractor:
    """Extract bounded causal features from sequential depth and trade events.

    Parameters
    ----------
    window_size:
        Number of atomic depth messages in the causal OFI and trade-flow window.
        The window is event-count based; elapsed-time duration is deliberately
        not inferred from exchange timestamps.
    tick_size:
        Price units per venue tick.
    spread_scale_ticks:
        Fixed tick scale used by ``tanh(spread / scale)``.  Unlike online
        z-scoring, this transform is identical in training and evaluation.
    trade_side_is_aggressor:
        Meaning of generic trade ``side`` fields.  The default is ``True`` for
        normal market-data trade records.  Set this to ``False`` only for an
        adapter whose side belongs to the resting order rather than the
        aggressor.

    Notes
    -----
    The extractor is single-writer state and deliberately does not lock.  The
    returned state buffer is borrowed and overwritten by the next update; do
    not mutate it, and copy it before retaining it in custom replay storage.

    Synthetic fill quantities are caused by the agent's own quotes.
    Omit them when trade flow is intended to contain only exogenous predictive
    prints; pass them only when execution feedback is intentionally observable.
    """

    FEATURE_NAMES: Final = FEATURE_NAMES
    STATE_SIZE: Final = STATE_SIZE

    def __init__(
        self,
        *,
        window_size: int = 32,
        tick_size: Number = 1,
        spread_scale_ticks: Number = 8,
        trade_side_is_aggressor: bool = True,
    ) -> None:
        if (
            isinstance(window_size, (bool, np.bool_))
            or not isinstance(window_size, (int, np.integer))
            or int(window_size) <= 0
        ):
            raise ValueError("window_size must be a positive integer")
        if not isinstance(trade_side_is_aggressor, (bool, np.bool_)):
            raise TypeError("trade_side_is_aggressor must be a bool")

        self.window_size = int(window_size)
        self.tick_size = self._positive_float(tick_size, "tick_size")
        self.spread_scale_ticks = self._positive_float(
            spread_scale_ticks,
            "spread_scale_ticks",
        )
        self.trade_side_is_aggressor = bool(trade_side_is_aggressor)

        # Exact books own absolute sizes.  Top-5 caches make observation reads
        # O(5); lazy candidate heaps refill depleted levels in O(log N)
        # amortized time rather than rescanning deep dictionaries at the touch.
        self._books: tuple[Book, Book] = ({}, {})
        self._top_prices: tuple[list[int], list[int]] = ([], [])
        self._candidate_heaps: tuple[list[int], list[int]] = ([], [])

        # One preallocated row per atomic depth event. Vectorized subtract/replace/
        # add updates all rolling totals without a deque or temporary array.
        self._ring = np.zeros(
            (self.window_size, _RING_WIDTH),
            dtype=np.float64,
        )
        self._totals = np.zeros(_RING_WIDTH, dtype=np.float64)
        self._cursor = 0

        # Stable-Baselines3 accepts reused writable NumPy observations.  Keeping
        # this buffer writable also avoids PyTorch's non-writable-array warning.
        self._state = np.zeros(STATE_SIZE, dtype=np.float32)

        self._initialized = False
        self._last_ofi = 0.0
        self._window_ofi = 0.0
        self._microprice: float | None = None
        self._spread: float | None = None
        self._depth_imbalance = 0.0

    # ------------------------------------------------------------------
    # Input validation and wire-format adapters
    # ------------------------------------------------------------------

    @staticmethod
    def _positive_float(value: Any, name: str) -> float:
        if isinstance(value, (bool, np.bool_)) or not isinstance(
            value,
            (int, float, np.integer, np.floating),
        ):
            raise TypeError(f"{name} must be a real number")
        result = float(value)
        if not math.isfinite(result) or result <= 0.0:
            raise ValueError(f"{name} must be finite and positive")
        return result

    @staticmethod
    def _price(value: Any) -> int:
        # Float keys are prohibited: above 2**53, a float can silently change
        # level identity and make a later exact deletion miss its target.
        if isinstance(value, (bool, np.bool_)) or not isinstance(
            value,
            (int, np.integer),
        ):
            raise TypeError("price must be a fixed-point integer")
        result = int(value)
        if result <= 0 or result > _INT64_MAX:
            raise ValueError(f"price must be between 1 and {_INT64_MAX}")
        return result

    @staticmethod
    def _size(value: Any, *, positive: bool) -> Number:
        if isinstance(value, (bool, np.bool_)) or not isinstance(
            value,
            (int, float, np.integer, np.floating),
        ):
            raise TypeError("size must be a real number")
        result: Number = (
            int(value) if isinstance(value, (int, np.integer)) else float(value)
        )
        invalid_sign = result <= 0 if positive else result < 0
        if invalid_sign or result > _UINT64_MAX or not math.isfinite(float(result)):
            qualifier = "positive" if positive else "non-negative"
            raise ValueError(f"size must be finite, {qualifier}, and <= {_UINT64_MAX}")
        return result

    @staticmethod
    def _side(value: Any) -> int:
        if isinstance(value, str):
            normalized = value.strip().lower()
            if normalized in {"bid", "buy"}:
                return BUY_SIDE
            if normalized in {"ask", "sell"}:
                return SELL_SIDE
            raise ValueError(f"unsupported side {value!r}")
        if isinstance(value, (bool, np.bool_)) or not isinstance(
            value,
            (int, np.integer),
        ):
            raise TypeError("side must be bid/buy/0 or ask/sell/1")
        side = int(value)
        if side not in (BUY_SIDE, SELL_SIDE):
            raise ValueError(f"unsupported side index {side}")
        return side

    @staticmethod
    def _has_field(record: Any, name: str) -> bool:
        if isinstance(record, Mapping):
            return name in record
        if hasattr(record, name):
            return True
        names = getattr(getattr(record, "dtype", None), "names", None)
        return names is not None and name in names

    @classmethod
    def _field(cls, record: Any, *names: str) -> Any:
        """Read a mapping, slotted dataclass, or NumPy structured field."""

        for name in names:
            if isinstance(record, Mapping) and name in record:
                return record[name]
            if hasattr(record, name):
                return getattr(record, name)
            dtype_names = getattr(getattr(record, "dtype", None), "names", None)
            if dtype_names is not None and name in dtype_names:
                return record[name]
        raise KeyError(f"missing required field {names[0]!r}")

    def _change(self, record: Any) -> Change:
        if isinstance(record, Mapping) or self._has_field(record, "side_index"):
            side = self._field(record, "side", "side_index")
            price = self._field(record, "price")
            size = self._field(record, "size", "quantity")
        else:
            try:
                side, price, size = record
            except (TypeError, ValueError) as error:
                raise ValueError(
                    "each change must contain side, price, and size"
                ) from error
        return self._side(side), self._price(price), self._size(size, positive=False)

    def _snapshot_side(self, levels: Any) -> Book:
        if not isinstance(levels, Mapping):
            raise TypeError("snapshot bids and asks must be price-to-size mappings")
        book: Book = {}
        for raw_price, raw_size in levels.items():
            price = self._price(raw_price)
            size = self._size(raw_size, positive=False)
            if size > 0:
                book[price] = size
        return book

    def _changes(self, update: Mapping[str, Any]) -> list[Change]:
        if "changes" in update:
            raw_changes = update["changes"]
            if not isinstance(raw_changes, Iterable) or isinstance(
                raw_changes,
                (str, bytes, Mapping),
            ):
                raise TypeError("changes must be an iterable of records")
            return [self._change(change) for change in raw_changes]

        changes: list[Change] = []
        for key, side in (("bids", BUY_SIDE), ("asks", SELL_SIDE)):
            if key not in update:
                continue
            raw_levels = update[key]
            if not isinstance(raw_levels, Mapping):
                raise TypeError("grouped bid/ask changes must be mappings")
            changes.extend(
                (
                    side,
                    self._price(price),
                    self._size(size, positive=False),
                )
                for price, size in raw_levels.items()
            )
        if changes or "bids" in update or "asks" in update:
            return changes
        if any(key in update for key in ("side", "side_index", "price", "size")):
            return [self._change(update)]
        raise KeyError("incremental message contains no depth changes")

    def _aggregate_trades(self, trade_events: Any) -> tuple[float, float]:
        if trade_events is None:
            return 0.0, 0.0
        if isinstance(trade_events, Mapping) or not isinstance(
            trade_events,
            Iterable,
        ):
            events: Iterable[Any] = (trade_events,)
        else:
            events = trade_events

        signed_volume = 0.0
        total_volume = 0.0
        for event in events:
            payload = (
                self._field(event, "trade")
                if self._has_field(event, "trade")
                else event
            )
            quantity = float(
                self._size(self._field(payload, "quantity", "size"), positive=True)
            )
            if self._has_field(payload, "aggressor_side"):
                side = self._side(self._field(payload, "aggressor_side"))
                side_is_aggressor = True
            else:
                side = self._side(self._field(payload, "side", "side_index"))
                side_is_aggressor = (
                    self._field(payload, "side_is_aggressor")
                    if self._has_field(payload, "side_is_aggressor")
                    else self.trade_side_is_aggressor
                )
                if not isinstance(side_is_aggressor, (bool, np.bool_)):
                    raise TypeError("side_is_aggressor must be a bool")
            aggressor_side = side if side_is_aggressor else 1 - side
            signed_volume += quantity if aggressor_side == BUY_SIDE else -quantity
            total_volume += quantity
            if not math.isfinite(signed_volume) or not math.isfinite(total_volume):
                raise ValueError("aggregate trade volume exceeds float64 capacity")
        return signed_volume, total_volume

    # ------------------------------------------------------------------
    # Exact known-book state and bounded nearest-five maintenance
    # ------------------------------------------------------------------

    @staticmethod
    def _heap_key(side: int, price: int) -> int:
        return -price if side == BUY_SIDE else price

    @staticmethod
    def _heap_price(side: int, key: int) -> int:
        return -key if side == BUY_SIDE else key

    def _push_candidate(self, side: int, price: int) -> None:
        heapq.heappush(self._candidate_heaps[side], self._heap_key(side, price))

    def _rebuild_side(self, side: int) -> None:
        book = self._books[side]
        top = (
            heapq.nlargest(TOP_DEPTH_LEVELS, book)
            if side == BUY_SIDE
            else heapq.nsmallest(TOP_DEPTH_LEVELS, book)
        )
        self._top_prices[side][:] = top
        candidates = [self._heap_key(side, price) for price in book if price not in top]
        heapq.heapify(candidates)
        self._candidate_heaps[side][:] = candidates

    def _compact_candidates(self, side: int) -> None:
        # Deleting an off-touch level leaves a lazy heap tombstone.  Rebuild
        # only after stale entries exceed live candidates by a constant factor,
        # keeping memory bounded and rebuild cost amortized over prior updates.
        live = max(0, len(self._books[side]) - len(self._top_prices[side]))
        if len(self._candidate_heaps[side]) > max(
            64,
            (2 * live) + TOP_DEPTH_LEVELS,
        ):
            self._rebuild_side(side)

    def _insert_top(self, side: int, price: int) -> None:
        top = self._top_prices[side]
        if price in top:
            return
        index = 0
        if side == BUY_SIDE:
            while index < len(top) and top[index] > price:
                index += 1
        else:
            while index < len(top) and top[index] < price:
                index += 1
        top.insert(index, price)
        if len(top) > TOP_DEPTH_LEVELS:
            self._push_candidate(side, top.pop())

    def _refill_top(self, side: int) -> None:
        top = self._top_prices[side]
        candidates = self._candidate_heaps[side]
        while len(top) < TOP_DEPTH_LEVELS and candidates:
            price = self._heap_price(side, heapq.heappop(candidates))
            # Skip lazy deletions and duplicate entries for current top levels.
            if price in self._books[side] and price not in top:
                self._insert_top(side, price)

    def _apply_change(self, side: int, price: int, size: Number) -> None:
        book = self._books[side]
        top = self._top_prices[side]
        if size == 0:
            if book.pop(price, None) is not None and price in top:
                top.remove(price)
                self._refill_top(side)
            self._compact_candidates(side)
            return

        is_new = price not in book
        book[price] = size
        if not is_new or price in top:
            return
        if (
            len(top) < TOP_DEPTH_LEVELS
            or (side == BUY_SIDE and price > top[-1])
            or (side == SELL_SIDE and price < top[-1])
        ):
            self._insert_top(side, price)
        else:
            self._push_candidate(side, price)
        self._compact_candidates(side)

    def _bbo(self) -> Bbo | None:
        if not self._top_prices[BUY_SIDE] or not self._top_prices[SELL_SIDE]:
            return None
        bid = self._top_prices[BUY_SIDE][0]
        ask = self._top_prices[SELL_SIDE][0]
        return bid, self._books[BUY_SIDE][bid], ask, self._books[SELL_SIDE][ask]

    @staticmethod
    def _validate_uncrossed(bbo: Bbo | None) -> None:
        if bbo is not None and bbo[0] >= bbo[2]:
            raise ValueError(
                f"locked/crossed depth book: best_bid={bbo[0]}, best_ask={bbo[2]}"
            )

    # ------------------------------------------------------------------
    # Microstructure calculations and causal normalization
    # ------------------------------------------------------------------

    @staticmethod
    def _microprice_components(bbo: Bbo) -> tuple[float, float, float]:
        """Return microprice, spread, and normalized L1 pressure.

        ``microprice = (ask * bid_size + bid * ask_size) / total_size``.
        The midpoint form avoids multiplying two large fixed-point integers.
        """

        bid, bid_size, ask, ask_size = bbo
        spread = float(ask - bid)
        total_size = bid_size + ask_size
        pressure = float(bid_size - ask_size) / float(total_size)
        midpoint = float(bid) + (0.5 * spread)
        return midpoint + (0.5 * spread * pressure), spread, pressure

    @staticmethod
    def _cks_ofi(previous: Bbo, current: Bbo) -> float:
        """Return the standard Cont--Kukanov--Stoikov event contribution.

        Equality intentionally activates both indicators, producing the queue
        size delta when a best price is unchanged.
        """

        old_bid, old_bid_size, old_ask, old_ask_size = previous
        bid, bid_size, ask, ask_size = current
        bid_flow = (bid_size if bid >= old_bid else 0) - (
            old_bid_size if bid <= old_bid else 0
        )
        ask_flow = -(ask_size if ask <= old_ask else 0) + (
            old_ask_size if ask >= old_ask else 0
        )
        return float(bid_flow + ask_flow)

    def _reset_all_signals(self) -> None:
        self._ring.fill(0.0)
        self._totals.fill(0.0)
        self._cursor = 0
        self._state.fill(0.0)
        self._last_ofi = 0.0
        self._window_ofi = 0.0
        self._microprice = None
        self._spread = None
        self._depth_imbalance = 0.0

    def _reset_quote_signals(self) -> None:
        # A one-sided transition invalidates OFI and microprice history, but it
        # must not erase otherwise valid exogenous trade flow.
        self._ring[:, :_SIGNED_TRADE].fill(0.0)
        self._totals[:_SIGNED_TRADE].fill(0.0)
        self._last_ofi = 0.0
        self._window_ofi = 0.0

    def reset(self) -> None:
        """Clear book and signal history for a full market-data rewind."""

        for side in (BUY_SIDE, SELL_SIDE):
            self._books[side].clear()
            self._top_prices[side].clear()
            self._candidate_heaps[side].clear()
        self._reset_all_signals()
        self._initialized = False

    def _push_window(
        self,
        *,
        ofi: float,
        depth: float,
        valid_ofi: bool,
        signed_trade: float,
        trade_volume: float,
    ) -> None:
        row = self._ring[self._cursor]
        self._totals -= row
        row[:] = ofi, depth, float(valid_ofi), signed_trade, trade_volume
        self._totals += row
        self._cursor = (self._cursor + 1) % self.window_size

    def _write_state(self, bbo: Bbo | None, microprice_drift: float) -> None:
        valid_count = self._totals[_VALID_OFI]
        average_depth = self._totals[_DEPTH] / valid_count if valid_count > 0.0 else 0.0
        normalized_ofi = (
            math.tanh(self._totals[_OFI] / average_depth)
            if average_depth > 0.0
            else 0.0
        )

        trade_volume = self._totals[_TRADE_VOLUME]
        trade_imbalance = (
            self._totals[_SIGNED_TRADE] / trade_volume if trade_volume > 0.0 else 0.0
        )

        bid_depth = sum(
            self._books[BUY_SIDE][price] for price in self._top_prices[BUY_SIDE]
        )
        ask_depth = sum(
            self._books[SELL_SIDE][price] for price in self._top_prices[SELL_SIDE]
        )
        total_depth = bid_depth + ask_depth
        depth_imbalance = (
            float(bid_depth - ask_depth) / float(total_depth)
            if total_depth > 0
            else 0.0
        )

        pressure = 0.0
        normalized_spread = 0.0
        if bbo is None:
            self._microprice = None
            self._spread = None
        else:
            self._microprice, self._spread, pressure = self._microprice_components(bbo)
            normalized_spread = math.tanh(
                self._spread / (self.tick_size * self.spread_scale_ticks)
            )

        self._window_ofi = float(self._totals[_OFI])
        self._depth_imbalance = depth_imbalance
        self._state[:] = (
            normalized_ofi,
            pressure,
            microprice_drift,
            normalized_spread,
            depth_imbalance,
            trade_imbalance,
        )
        np.clip(self._state, -1.0, 1.0, out=self._state)
        if not np.isfinite(self._state).all():
            raise RuntimeError("feature normalization produced a non-finite value")

    def _advance(
        self,
        previous: Bbo | None,
        current: Bbo | None,
        signed_trade: float,
        trade_volume: float,
        *,
        rebase: bool,
    ) -> None:
        valid_transition = previous is not None and current is not None
        if rebase:
            self._reset_all_signals()
        elif not valid_transition:
            # Never manufacture OFI across an empty-side interval or feed gap.
            self._reset_quote_signals()

        ofi = 0.0
        depth_scale = 0.0
        microprice_drift = 0.0
        if valid_transition and not rebase:
            assert previous is not None and current is not None
            ofi = self._cks_ofi(previous, current)
            depth_scale = 0.25 * float(
                previous[1] + previous[3] + current[1] + current[3]
            )
            old_micro, old_spread, _ = self._microprice_components(previous)
            micro, spread, _ = self._microprice_components(current)
            drift_scale = max(self.tick_size, 0.5 * (old_spread + spread))
            microprice_drift = math.tanh((micro - old_micro) / drift_scale)

        self._last_ofi = ofi
        self._push_window(
            ofi=ofi,
            depth=depth_scale,
            valid_ofi=valid_transition and not rebase,
            signed_trade=signed_trade,
            trade_volume=trade_volume,
        )
        self._write_state(current, microprice_drift)

    # ------------------------------------------------------------------
    # Public ingestion boundary
    # ------------------------------------------------------------------

    def _install_snapshot(
        self,
        bids: Book,
        asks: Book,
        signed_trade: float,
        trade_volume: float,
    ) -> None:
        best_bid = max(bids, default=None)
        best_ask = min(asks, default=None)
        if not self._initialized and (best_bid is None or best_ask is None):
            raise ValueError("initial depth snapshot must contain bids and asks")
        if best_bid is not None and best_ask is not None and best_bid >= best_ask:
            raise ValueError(
                "locked/crossed depth snapshot: "
                f"best_bid={best_bid}, best_ask={best_ask}"
            )

        self._books = bids, asks
        self._rebuild_side(BUY_SIDE)
        self._rebuild_side(SELL_SIDE)
        self._initialized = True
        self._advance(None, self._bbo(), signed_trade, trade_volume, rebase=True)

    def _apply_batch(
        self,
        changes: list[Change],
        signed_trade: float,
        trade_volume: float,
    ) -> None:
        if not self._initialized:
            raise RuntimeError("an initial depth snapshot is required before updates")
        previous = self._bbo()

        # Save only first-touch values for duplicate levels.  This provides an
        # atomic crossed-book failure path without copying full dictionaries.
        undo: dict[tuple[int, int], object | Number] = {}
        for side, price, size in changes:
            key = side, price
            if key not in undo:
                undo[key] = self._books[side].get(price, _MISSING)
            self._apply_change(side, price, size)

        current = self._bbo()
        try:
            self._validate_uncrossed(current)
        except ValueError:
            for (side, price), old_size in undo.items():
                if old_size is _MISSING:
                    self._books[side].pop(price, None)
                else:
                    self._books[side][price] = old_size  # type: ignore[assignment]
            self._rebuild_side(BUY_SIDE)
            self._rebuild_side(SELL_SIDE)
            raise
        self._advance(previous, current, signed_trade, trade_volume, rebase=False)

    def update(
        self,
        depth_update: Mapping[str, Any],
        trade_events: Any = None,
    ) -> None:
        """Ingest one atomic dictionary depth message and associated trades."""

        if not isinstance(depth_update, Mapping):
            raise TypeError("depth_update must be a mapping")
        raw_kind = depth_update.get("type", depth_update.get("kind", "update"))
        if not isinstance(raw_kind, str):
            raise TypeError("depth message type must be a string")
        kind = raw_kind.strip().lower()
        if kind not in {"snapshot", "update", "l2update"}:
            raise ValueError(f"unsupported depth message type {raw_kind!r}")

        signed_trade, trade_volume = self._aggregate_trades(trade_events)
        if kind == "snapshot":
            if "bids" in depth_update and "asks" in depth_update:
                bids = self._snapshot_side(depth_update["bids"])
                asks = self._snapshot_side(depth_update["asks"])
            elif "changes" in depth_update:
                bids, asks = {}, {}
                for side, price, size in self._changes(depth_update):
                    if size > 0:
                        (bids if side == BUY_SIDE else asks)[price] = size
            else:
                raise KeyError("snapshot requires bids/asks or side-tagged changes")
            self._install_snapshot(bids, asks, signed_trade, trade_volume)
            return

        self._apply_batch(
            self._changes(depth_update),
            signed_trade,
            trade_volume,
        )

    def update_overlay(self, batch: Any, trade_events: Any = None) -> None:
        """Ingest one compatible atomic depth batch and optional trade records.

        Call this once inside the environment's per-feed-tick loop, after the
        batch is applied and that tick's fills are known.  Calling only after a
        multi-event action would collapse best-quote transitions and change OFI.
        """

        kind = getattr(batch, "kind", None)
        levels = getattr(batch, "levels", None)
        if not isinstance(kind, str) or not isinstance(levels, Iterable):
            raise TypeError("batch must expose string kind and iterable levels")
        normalized_kind = kind.strip().lower()
        if normalized_kind not in {"snapshot", "update", "l2update"}:
            raise ValueError(f"unsupported depth batch kind {kind!r}")

        changes = [self._change(level) for level in levels]
        signed_trade, trade_volume = self._aggregate_trades(trade_events)
        if normalized_kind == "snapshot":
            bids, asks = {}, {}
            for side, price, size in changes:
                if size > 0:
                    (bids if side == BUY_SIDE else asks)[price] = size
            self._install_snapshot(bids, asks, signed_trade, trade_volume)
            return
        self._apply_batch(changes, signed_trade, trade_volume)

    def update_trades(self, trade_events: Any) -> None:
        """Add trade-only events without advancing or evicting the OFI window."""

        if not self._initialized:
            raise RuntimeError("an initial depth snapshot is required before trades")
        signed_trade, trade_volume = self._aggregate_trades(trade_events)
        if trade_volume == 0.0:
            return

        row = self._ring[(self._cursor - 1) % self.window_size]
        new_signed = row[_SIGNED_TRADE] + signed_trade
        new_volume = row[_TRADE_VOLUME] + trade_volume
        if not math.isfinite(new_signed) or not math.isfinite(new_volume):
            raise ValueError("aggregate trade volume exceeds float64 capacity")
        self._totals -= row
        row[_SIGNED_TRADE] = new_signed
        row[_TRADE_VOLUME] = new_volume
        self._totals += row
        self._write_state(self._bbo(), float(self._state[2]))

    def ingest(
        self,
        depth_update: Mapping[str, Any],
        trade_events: Any = None,
    ) -> None:
        """Feed-handler-style alias for :meth:`update`."""

        self.update(depth_update, trade_events)

    def get_state(self) -> np.ndarray:
        """Return the reused normalized C-contiguous ``float32`` state buffer."""

        return self._state

    @property
    def last_ofi(self) -> float:
        """Raw CKS contribution from the latest atomic depth message."""

        return self._last_ofi

    @property
    def order_flow_imbalance(self) -> float:
        """Raw CKS OFI sum over the configured causal event window."""

        return self._window_ofi

    @property
    def microprice(self) -> float | None:
        """Current raw cross-weighted microprice, or ``None`` if one-sided."""

        return self._microprice

    @property
    def spread(self) -> float | None:
        """Current raw spread, or ``None`` if the book is one-sided."""

        return self._spread

    @property
    def depth_imbalance(self) -> float:
        """Imbalance over up to five nearest known levels in ``[-1, 1]``."""

        return self._depth_imbalance


__all__ = ["FEATURE_NAMES", "STATE_SIZE", "FeatureExtractor"]
