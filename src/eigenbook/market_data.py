"""Versioned, sequenced external depth-market-data contract.

The canonical CSV stores one physical row per changed depth level, but rows are
grouped by a strictly increasing ``event_id``.  An event is yielded only after
all of its rows have been parsed and validated.  Timestamps never determine
event boundaries.

Prices and sizes are decimal strings on disk and exact fixed-point integers in
memory.  A tape begins with a depth snapshot.  Incremental depth ranges must
bridge the preceding final update identifier, and an explicit later snapshot
starts a new recovery epoch.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Final, Iterator, Mapping, TextIO


SCHEMA_VERSION: Final = "eigenbook.market_data.v2"
DATA_MODES: Final = frozenset({"depth", "depth_trades"})
EVENT_KINDS: Final = frozenset({"snapshot", "depth_update", "trade"})
DEPTH_SIDES: Final = frozenset({"bid", "ask"})
AGGRESSOR_SIDES: Final = frozenset({"buy", "sell"})

CSV_COLUMNS: Final = (
    "schema_version",
    "data_mode",
    "venue",
    "symbol",
    "price_scale",
    "quantity_scale",
    "event_id",
    "event_time",
    "event_kind",
    "first_update_id",
    "last_update_id",
    "previous_update_id",
    "trade_id",
    "first_trade_id",
    "last_trade_id",
    "aggressor_side",
    "side",
    "price",
    "size",
)

_EVENT_METADATA_COLUMNS: Final = CSV_COLUMNS[:-3]
_INT64_MAX: Final = (1 << 63) - 1
_UINT64_MAX: Final = (1 << 64) - 1


class MarketDataError(ValueError):
    """The external tape violates the canonical deterministic contract."""


@dataclass(frozen=True, slots=True)
class DepthLevel:
    """One absolute-size depth change in exact fixed-point units."""

    side: str
    price: int
    size: int
    row_number: int

    @property
    def side_index(self) -> int:
        """Compatibility adapter: bid is 0 and ask is 1."""

        if self.side == "bid":
            return 0
        if self.side == "ask":
            return 1
        raise MarketDataError(
            f"invalid depth side={self.side!r} at row {self.row_number}"
        )


@dataclass(frozen=True, slots=True)
class MarketEvent:
    """One complete source event, never an intermediate physical CSV row."""

    schema_version: str
    data_mode: str
    venue: str
    symbol: str
    price_scale: int
    quantity_scale: int
    event_id: int
    event_time: int
    kind: str
    first_update_id: int | None
    last_update_id: int | None
    previous_update_id: int | None
    trade_id: int | None
    first_trade_id: int | None
    last_trade_id: int | None
    aggressor_side: str | None
    levels: tuple[DepthLevel, ...]
    trade_price: int | None
    trade_size: int | None
    first_row: int
    last_row: int

    @property
    def source_context(self) -> str:
        """Stable diagnostic context containing source IDs and physical rows."""

        rows = (
            str(self.first_row)
            if self.first_row == self.last_row
            else f"{self.first_row}-{self.last_row}"
        )
        if self.kind == "trade":
            source = (
                f"aggregate_trade_id={self.trade_id} "
                f"raw_trade_ids={self.first_trade_id}-{self.last_trade_id}"
            )
        else:
            previous = (
                "none"
                if self.previous_update_id is None
                else str(self.previous_update_id)
            )
            source = (
                f"update_ids={self.first_update_id}-{self.last_update_id} "
                f"previous_update_id={previous}"
            )
        return f"event_id={self.event_id} {source} rows={rows}"


@dataclass(frozen=True, slots=True)
class _RawRow:
    row_number: int
    values: Mapping[str, str | None]


def _raw_context(rows: list[_RawRow]) -> str:
    first = rows[0]
    last = rows[-1]
    event_id = (first.values.get("event_id") or "<missing>").strip()
    first_update = (first.values.get("first_update_id") or "<missing>").strip()
    last_update = (first.values.get("last_update_id") or "<missing>").strip()
    trade_id = (first.values.get("trade_id") or "").strip()
    first_trade_id = (first.values.get("first_trade_id") or "<missing>").strip()
    last_trade_id = (first.values.get("last_trade_id") or "<missing>").strip()
    source = (
        f"aggregate_trade_id={trade_id} raw_trade_ids={first_trade_id}-{last_trade_id}"
        if trade_id
        else f"update_ids={first_update}-{last_update}"
    )
    row_text = (
        str(first.row_number)
        if first.row_number == last.row_number
        else f"{first.row_number}-{last.row_number}"
    )
    return f"event_id={event_id} {source} rows={row_text}"


def _required_value(row: _RawRow, name: str, context: str) -> str:
    value = row.values.get(name)
    if value is None:
        raise MarketDataError(f"{context}: {name} is missing")
    result = value.strip()
    if not result:
        raise MarketDataError(f"{context}: {name} is empty")
    return result


def _optional_value(row: _RawRow, name: str) -> str:
    value = row.values.get(name)
    return "" if value is None else value.strip()


def _parse_uint(
    text: str,
    *,
    name: str,
    context: str,
    positive: bool = False,
) -> int:
    try:
        value = int(text, 10)
    except ValueError as error:
        raise MarketDataError(
            f"{context}: {name} is not an integer: {text!r}"
        ) from error
    minimum = 1 if positive else 0
    if value < minimum or value > _UINT64_MAX:
        qualifier = "positive" if positive else "non-negative"
        raise MarketDataError(
            f"{context}: {name} must be {qualifier} and <= {_UINT64_MAX}"
        )
    return value


def _parse_scale(text: str, *, name: str, context: str) -> int:
    return _parse_uint(text, name=name, context=context, positive=True)


def _parse_fixed_point(
    text: str,
    *,
    scale: int,
    name: str,
    context: str,
    allow_zero: bool,
    maximum: int,
) -> int:
    try:
        value = Decimal(text)
    except InvalidOperation as error:
        raise MarketDataError(
            f"{context}: {name} is not an exact decimal: {text!r}"
        ) from error
    if not value.is_finite():
        raise MarketDataError(f"{context}: {name} must be finite")
    if value < 0 or (value == 0 and not allow_zero):
        qualifier = "non-negative" if allow_zero else "positive"
        raise MarketDataError(f"{context}: {name} must be {qualifier}")
    numerator, denominator = value.as_integer_ratio()
    scaled_numerator = numerator * scale
    result, remainder = divmod(scaled_numerator, denominator)
    if remainder:
        raise MarketDataError(
            f"{context}: {name}={text!r} exceeds scale={scale} precision"
        )
    if result > maximum:
        raise MarketDataError(f"{context}: scaled {name} exceeds native capacity")
    return result


class SequencedDepthCsv:
    """Rewindable streaming reader for canonical atomic depth tapes."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)

    def __iter__(self) -> Iterator[MarketEvent]:
        return self.events()

    def events(self) -> Iterator[MarketEvent]:
        """Yield validated events; every iteration reopens the tape."""

        try:
            file = self.path.open("r", newline="", encoding="utf-8-sig")
        except OSError as error:
            raise FileNotFoundError(
                f"unable to open market-data tape: {self.path}"
            ) from error
        with file:
            yield from self._events_from_file(file)

    def _events_from_file(self, file: TextIO) -> Iterator[MarketEvent]:
        def checked_physical_lines() -> Iterator[str]:
            for row_number, line in enumerate(file, start=1):
                if not line.strip():
                    raise MarketDataError(
                        "event_id=<missing> update_ids=<missing>-<missing> "
                        f"rows={row_number}: blank physical CSV row"
                    )
                yield line

        reader: csv.DictReader[str] = csv.DictReader(checked_physical_lines())
        if reader.fieldnames is None:
            raise MarketDataError(f"{self.path}: CSV header is missing")
        normalized = tuple(name.strip() for name in reader.fieldnames)
        if normalized != CSV_COLUMNS:
            raise MarketDataError(
                f"{self.path}: CSV header must be exactly {list(CSV_COLUMNS)}; "
                f"found={list(normalized)}"
            )

        lookahead: _RawRow | None = None
        expected_event_id = 1
        tape_identity: tuple[str, str, str, str, int, int] | None = None
        last_depth_update_id: int | None = None
        last_aggregate_trade_id: int | None = None
        last_raw_trade_id: int | None = None
        awaiting_depth_bridge = False
        yielded = 0

        while True:
            first = lookahead
            lookahead = None
            if first is None:
                first = self._next_raw(reader)
            if first is None:
                break

            raw_event_id = _optional_value(first, "event_id")
            rows = [first]
            while True:
                candidate = self._next_raw(reader)
                if candidate is None:
                    break
                if _optional_value(candidate, "event_id") != raw_event_id:
                    lookahead = candidate
                    break
                rows.append(candidate)

            event = self._parse_event(rows)
            if event.event_id != expected_event_id:
                problem = (
                    "event rows are not contiguous or are out of order"
                    if event.event_id < expected_event_id
                    else "event_id order gap"
                )
                raise MarketDataError(
                    f"{event.source_context}: {problem}; expected={expected_event_id}"
                )
            expected_event_id += 1

            identity = (
                event.schema_version,
                event.data_mode,
                event.venue,
                event.symbol,
                event.price_scale,
                event.quantity_scale,
            )
            if tape_identity is None:
                tape_identity = identity
            elif identity != tape_identity:
                raise MarketDataError(
                    f"{event.source_context}: tape metadata changed; "
                    f"expected={tape_identity}, found={identity}"
                )

            if yielded == 0 and event.kind != "snapshot":
                raise MarketDataError(
                    f"{event.source_context}: tape must begin with a snapshot"
                )

            if event.kind == "snapshot":
                if awaiting_depth_bridge:
                    raise MarketDataError(
                        f"{event.source_context}: snapshot arrived before the "
                        "preceding snapshot was bridged by a depth update"
                    )
                if (
                    last_depth_update_id is not None
                    and event.last_update_id is not None
                    and event.last_update_id <= last_depth_update_id
                ):
                    raise MarketDataError(
                        f"{event.source_context}: stale resnapshot; "
                        f"previous_last_update_id={last_depth_update_id}"
                    )
                last_depth_update_id = event.last_update_id
                awaiting_depth_bridge = True
            elif event.kind == "depth_update":
                if last_depth_update_id is None:
                    raise MarketDataError(
                        f"{event.source_context}: depth update has no active snapshot"
                    )
                expected_update_id = last_depth_update_id + 1
                if event.previous_update_id is not None and (
                    event.previous_update_id != last_depth_update_id
                ):
                    raise MarketDataError(
                        f"{event.source_context}: depth continuity gap; "
                        f"expected_previous_update_id={last_depth_update_id}"
                    )
                assert event.first_update_id is not None
                assert event.last_update_id is not None
                if not (
                    event.first_update_id <= expected_update_id <= event.last_update_id
                ):
                    raise MarketDataError(
                        f"{event.source_context}: depth continuity gap; "
                        f"expected_update_id={expected_update_id}"
                    )
                last_depth_update_id = event.last_update_id
                awaiting_depth_bridge = False
            else:
                if awaiting_depth_bridge:
                    raise MarketDataError(
                        f"{event.source_context}: trade arrived before the active "
                        "snapshot was bridged by a depth update"
                    )
                assert event.trade_id is not None
                assert event.first_trade_id is not None
                assert event.last_trade_id is not None
                if (
                    last_aggregate_trade_id is not None
                    and event.trade_id != last_aggregate_trade_id + 1
                ):
                    raise MarketDataError(
                        f"{event.source_context}: aggregate trade continuity gap; "
                        "expected_aggregate_trade_id="
                        f"{last_aggregate_trade_id + 1}"
                    )
                if (
                    last_raw_trade_id is not None
                    and event.first_trade_id != last_raw_trade_id + 1
                ):
                    raise MarketDataError(
                        f"{event.source_context}: raw trade continuity gap; "
                        f"expected_first_trade_id={last_raw_trade_id + 1}"
                    )
                last_aggregate_trade_id = event.trade_id
                last_raw_trade_id = event.last_trade_id

            yielded += 1
            yield event

        if yielded == 0:
            raise MarketDataError(
                f"{self.path}: tape contains no events; initial snapshot required"
            )
        if awaiting_depth_bridge:
            raise MarketDataError(
                f"{self.path}: tape ended before the active snapshot was bridged "
                "by a depth update"
            )

    @staticmethod
    def _next_raw(reader: csv.DictReader[str]) -> _RawRow | None:
        try:
            values = next(reader)
        except StopIteration:
            return None
        row = _RawRow(reader.line_num, values)
        if None in values:
            raise MarketDataError(
                f"event_id={_optional_value(row, 'event_id') or '<missing>'} "
                f"update_ids={_optional_value(row, 'first_update_id') or '<missing>'}-"
                f"{_optional_value(row, 'last_update_id') or '<missing>'} "
                f"rows={row.row_number}: row has more fields than the header"
            )
        return row

    @staticmethod
    def _parse_event(rows: list[_RawRow]) -> MarketEvent:
        context = _raw_context(rows)
        first = rows[0]
        baseline = tuple(
            _optional_value(first, name) for name in _EVENT_METADATA_COLUMNS
        )
        for row in rows[1:]:
            metadata = tuple(
                _optional_value(row, name) for name in _EVENT_METADATA_COLUMNS
            )
            if metadata != baseline:
                raise MarketDataError(
                    f"{context}: metadata differs within one atomic event "
                    f"at row {row.row_number}"
                )

        schema_version = _required_value(first, "schema_version", context)
        if schema_version != SCHEMA_VERSION:
            raise MarketDataError(
                f"{context}: unsupported schema_version={schema_version!r}"
            )
        data_mode = _required_value(first, "data_mode", context)
        if data_mode not in DATA_MODES:
            raise MarketDataError(f"{context}: unsupported data_mode={data_mode!r}")
        venue = _required_value(first, "venue", context)
        symbol = _required_value(first, "symbol", context)
        price_scale = _parse_scale(
            _required_value(first, "price_scale", context),
            name="price_scale",
            context=context,
        )
        quantity_scale = _parse_scale(
            _required_value(first, "quantity_scale", context),
            name="quantity_scale",
            context=context,
        )
        event_id = _parse_uint(
            _required_value(first, "event_id", context),
            name="event_id",
            context=context,
            positive=True,
        )
        event_time = _parse_uint(
            _required_value(first, "event_time", context),
            name="event_time",
            context=context,
        )
        kind = _required_value(first, "event_kind", context)
        if kind not in EVENT_KINDS:
            raise MarketDataError(f"{context}: unsupported event_kind={kind!r}")

        first_update_text = _optional_value(first, "first_update_id")
        last_update_text = _optional_value(first, "last_update_id")
        previous_update_text = _optional_value(first, "previous_update_id")
        trade_id_text = _optional_value(first, "trade_id")
        first_trade_id_text = _optional_value(first, "first_trade_id")
        last_trade_id_text = _optional_value(first, "last_trade_id")
        aggressor_side_text = _optional_value(first, "aggressor_side")

        levels: list[DepthLevel] = []
        trade_price: int | None = None
        trade_size: int | None = None
        first_update_id: int | None = None
        last_update_id: int | None = None
        previous_update_id: int | None = None
        trade_id: int | None = None
        first_trade_id: int | None = None
        last_trade_id: int | None = None
        aggressor_side: str | None = None

        if kind in {"snapshot", "depth_update"}:
            if not first_update_text or not last_update_text:
                raise MarketDataError(
                    f"{context}: {kind} requires first_update_id and last_update_id"
                )
            if (
                trade_id_text
                or first_trade_id_text
                or last_trade_id_text
                or aggressor_side_text
            ):
                raise MarketDataError(
                    f"{context}: depth events cannot contain trade metadata"
                )
            first_update_id = _parse_uint(
                first_update_text,
                name="first_update_id",
                context=context,
            )
            last_update_id = _parse_uint(
                last_update_text,
                name="last_update_id",
                context=context,
            )
            if first_update_id > last_update_id:
                raise MarketDataError(
                    f"{context}: first_update_id exceeds last_update_id"
                )
            if previous_update_text:
                previous_update_id = _parse_uint(
                    previous_update_text,
                    name="previous_update_id",
                    context=context,
                )
            if kind == "snapshot":
                if first_update_id != last_update_id:
                    raise MarketDataError(
                        f"{context}: snapshot update IDs must be equal"
                    )
                if previous_update_id is not None:
                    raise MarketDataError(
                        f"{context}: snapshot cannot contain previous_update_id"
                    )

            seen_levels: set[tuple[str, int]] = set()
            for row in rows:
                side = _required_value(row, "side", context)
                if side not in DEPTH_SIDES:
                    raise MarketDataError(
                        f"{context}: unsupported depth side={side!r} "
                        f"at row {row.row_number}"
                    )
                price = _parse_fixed_point(
                    _required_value(row, "price", context),
                    scale=price_scale,
                    name="price",
                    context=f"{context} row={row.row_number}",
                    allow_zero=False,
                    maximum=_INT64_MAX,
                )
                size = _parse_fixed_point(
                    _required_value(row, "size", context),
                    scale=quantity_scale,
                    name="size",
                    context=f"{context} row={row.row_number}",
                    allow_zero=kind == "depth_update",
                    maximum=_UINT64_MAX,
                )
                key = (side, price)
                if key in seen_levels:
                    raise MarketDataError(
                        f"{context}: duplicate {side} level {price} "
                        f"at row {row.row_number}"
                    )
                seen_levels.add(key)
                levels.append(DepthLevel(side, price, size, row.row_number))
        else:
            if data_mode != "depth_trades":
                raise MarketDataError(
                    f"{context}: trade event is prohibited in data_mode='depth'"
                )
            if first_update_text or last_update_text or previous_update_text:
                raise MarketDataError(
                    f"{context}: trade cannot contain depth update IDs"
                )
            if len(rows) != 1:
                raise MarketDataError(f"{context}: trade must occupy exactly one row")
            if _optional_value(first, "side"):
                raise MarketDataError(f"{context}: trade depth side must be empty")
            trade_id = _parse_uint(
                _required_value(first, "trade_id", context),
                name="trade_id",
                context=context,
            )
            first_trade_id = _parse_uint(
                _required_value(first, "first_trade_id", context),
                name="first_trade_id",
                context=context,
            )
            last_trade_id = _parse_uint(
                _required_value(first, "last_trade_id", context),
                name="last_trade_id",
                context=context,
            )
            if first_trade_id > last_trade_id:
                raise MarketDataError(
                    f"{context}: first_trade_id exceeds last_trade_id"
                )
            aggressor_side = _required_value(first, "aggressor_side", context)
            if aggressor_side not in AGGRESSOR_SIDES:
                raise MarketDataError(
                    f"{context}: unsupported aggressor_side={aggressor_side!r}"
                )
            trade_price = _parse_fixed_point(
                _required_value(first, "price", context),
                scale=price_scale,
                name="price",
                context=context,
                allow_zero=False,
                maximum=_INT64_MAX,
            )
            trade_size = _parse_fixed_point(
                _required_value(first, "size", context),
                scale=quantity_scale,
                name="size",
                context=context,
                allow_zero=False,
                maximum=_UINT64_MAX,
            )

        return MarketEvent(
            schema_version=schema_version,
            data_mode=data_mode,
            venue=venue,
            symbol=symbol,
            price_scale=price_scale,
            quantity_scale=quantity_scale,
            event_id=event_id,
            event_time=event_time,
            kind=kind,
            first_update_id=first_update_id,
            last_update_id=last_update_id,
            previous_update_id=previous_update_id,
            trade_id=trade_id,
            first_trade_id=first_trade_id,
            last_trade_id=last_trade_id,
            aggressor_side=aggressor_side,
            levels=tuple(levels),
            trade_price=trade_price,
            trade_size=trade_size,
            first_row=rows[0].row_number,
            last_row=rows[-1].row_number,
        )


class DepthBook:
    """Transactional exact depth state rebuilt from :class:`MarketEvent`."""

    def __init__(self) -> None:
        self.bids: dict[int, int] = {}
        self.asks: dict[int, int] = {}
        self.initialized = False
        self.schema_version: str | None = None
        self.data_mode: str | None = None
        self.venue: str | None = None
        self.symbol: str | None = None
        self.price_scale: int | None = None
        self.quantity_scale: int | None = None
        self.last_event_id = 0
        self.last_update_id: int | None = None
        self.last_aggregate_trade_id: int | None = None
        self.last_raw_trade_id: int | None = None
        self.event_count = 0
        self.trade_count = 0
        self.resnapshot_count = 0
        self.synchronized = False

    @property
    def best_bid(self) -> int | None:
        return max(self.bids, default=None)

    @property
    def best_ask(self) -> int | None:
        return min(self.asks, default=None)

    def apply(self, event: MarketEvent) -> None:
        """Validate and commit one complete event, or leave all state unchanged."""

        context = event.source_context
        if event.schema_version != SCHEMA_VERSION:
            raise MarketDataError(
                f"{context}: unsupported schema_version={event.schema_version!r}"
            )
        if event.data_mode not in DATA_MODES:
            raise MarketDataError(
                f"{context}: unsupported data_mode={event.data_mode!r}"
            )
        if event.kind not in EVENT_KINDS:
            raise MarketDataError(f"{context}: unsupported event kind={event.kind!r}")
        if event.kind == "trade" and event.data_mode != "depth_trades":
            raise MarketDataError(
                f"{context}: trade event is prohibited in data_mode='depth'"
            )
        if not event.venue or not event.symbol:
            raise MarketDataError(f"{context}: venue and symbol must be non-empty")
        if event.price_scale <= 0 or event.quantity_scale <= 0:
            raise MarketDataError(f"{context}: scales must be positive")
        expected_event_id = self.last_event_id + 1
        if event.event_id != expected_event_id:
            raise MarketDataError(
                f"{context}: event_id order gap; expected={expected_event_id}"
            )

        identity = (
            event.schema_version,
            event.data_mode,
            event.venue,
            event.symbol,
            event.price_scale,
            event.quantity_scale,
        )
        current_identity = (
            self.schema_version,
            self.data_mode,
            self.venue,
            self.symbol,
            self.price_scale,
            self.quantity_scale,
        )
        if self.initialized and identity != current_identity:
            raise MarketDataError(f"{context}: tape metadata changed")
        if not self.initialized and event.kind != "snapshot":
            raise MarketDataError(f"{context}: initial event must be a snapshot")

        if event.kind == "snapshot":
            candidate_bids: dict[int, int] = {}
            candidate_asks: dict[int, int] = {}
        elif event.kind == "depth_update":
            candidate_bids = self.bids.copy()
            candidate_asks = self.asks.copy()
        else:
            candidate_bids = self.bids
            candidate_asks = self.asks
        candidate_last_update_id = self.last_update_id
        candidate_last_aggregate_trade_id = self.last_aggregate_trade_id
        candidate_last_raw_trade_id = self.last_raw_trade_id
        candidate_trade_count = self.trade_count
        candidate_resnapshot_count = self.resnapshot_count
        candidate_synchronized = self.synchronized

        if event.kind in {"snapshot", "depth_update"}:
            if event.first_update_id is None or event.last_update_id is None:
                raise MarketDataError(
                    f"{context}: depth event requires first/last update IDs"
                )
            if event.first_update_id > event.last_update_id:
                raise MarketDataError(
                    f"{context}: first_update_id exceeds last_update_id"
                )
            if not event.levels:
                raise MarketDataError(f"{context}: depth event has no levels")
            if event.kind == "snapshot":
                if self.initialized and not self.synchronized:
                    raise MarketDataError(
                        f"{context}: snapshot arrived before the preceding "
                        "snapshot was bridged by a depth update"
                    )
                if event.first_update_id != event.last_update_id:
                    raise MarketDataError(
                        f"{context}: snapshot update IDs must be equal"
                    )
                if event.previous_update_id is not None:
                    raise MarketDataError(
                        f"{context}: snapshot cannot contain previous_update_id"
                    )
                if (
                    self.last_update_id is not None
                    and event.last_update_id <= self.last_update_id
                ):
                    raise MarketDataError(
                        f"{context}: stale resnapshot; "
                        f"previous_last_update_id={self.last_update_id}"
                    )
                candidate_last_update_id = event.last_update_id
                candidate_synchronized = False
                if self.initialized:
                    candidate_resnapshot_count += 1
            else:
                if self.last_update_id is None:
                    raise MarketDataError(
                        f"{context}: depth update has no active snapshot"
                    )
                expected_update_id = self.last_update_id + 1
                if event.previous_update_id is not None and (
                    event.previous_update_id != self.last_update_id
                ):
                    raise MarketDataError(
                        f"{context}: depth continuity gap; "
                        f"expected_previous_update_id={self.last_update_id}"
                    )
                if not (
                    event.first_update_id <= expected_update_id <= event.last_update_id
                ):
                    raise MarketDataError(
                        f"{context}: depth continuity gap; "
                        f"expected_update_id={expected_update_id}"
                    )
                candidate_last_update_id = event.last_update_id
                candidate_synchronized = True

            seen: set[tuple[str, int]] = set()
            for level in event.levels:
                if level.side not in DEPTH_SIDES:
                    raise MarketDataError(
                        f"{context}: invalid side={level.side!r} "
                        f"at row {level.row_number}"
                    )
                if level.price <= 0 or level.price > _INT64_MAX:
                    raise MarketDataError(
                        f"{context}: invalid price={level.price} "
                        f"at row {level.row_number}"
                    )
                if level.size < 0 or level.size > _UINT64_MAX:
                    raise MarketDataError(
                        f"{context}: invalid size={level.size} "
                        f"at row {level.row_number}"
                    )
                key = (level.side, level.price)
                if key in seen:
                    raise MarketDataError(
                        f"{context}: duplicate level={key} at row {level.row_number}"
                    )
                seen.add(key)
                levels = candidate_bids if level.side == "bid" else candidate_asks
                if level.size == 0:
                    levels.pop(level.price, None)
                else:
                    levels[level.price] = level.size

            if event.kind == "snapshot" and (not candidate_bids or not candidate_asks):
                raise MarketDataError(
                    f"{context}: snapshot must contain bid and ask liquidity"
                )
            best_bid = max(candidate_bids, default=None)
            best_ask = min(candidate_asks, default=None)
            if best_bid is not None and best_ask is not None and best_bid >= best_ask:
                raise MarketDataError(
                    f"{context}: crossed/locked depth after atomic event "
                    f"(best_bid={best_bid}, best_ask={best_ask})"
                )
        else:
            if not self.initialized:
                raise MarketDataError(f"{context}: trade precedes initial snapshot")
            if not self.synchronized:
                raise MarketDataError(
                    f"{context}: trade precedes the depth update that bridges "
                    "the active snapshot"
                )
            if (
                event.trade_id is None
                or event.first_trade_id is None
                or event.last_trade_id is None
                or event.first_trade_id > event.last_trade_id
                or event.aggressor_side not in AGGRESSOR_SIDES
                or event.trade_price is None
                or event.trade_price <= 0
                or event.trade_size is None
                or event.trade_size <= 0
            ):
                raise MarketDataError(f"{context}: invalid trade metadata")
            if (
                self.last_aggregate_trade_id is not None
                and event.trade_id != self.last_aggregate_trade_id + 1
            ):
                raise MarketDataError(
                    f"{context}: aggregate trade continuity gap; "
                    "expected_aggregate_trade_id="
                    f"{self.last_aggregate_trade_id + 1}"
                )
            if (
                self.last_raw_trade_id is not None
                and event.first_trade_id != self.last_raw_trade_id + 1
            ):
                raise MarketDataError(
                    f"{context}: raw trade continuity gap; "
                    f"expected_first_trade_id={self.last_raw_trade_id + 1}"
                )
            candidate_last_aggregate_trade_id = event.trade_id
            candidate_last_raw_trade_id = event.last_trade_id
            candidate_trade_count += 1

        # Commit only after the complete candidate state has passed validation.
        self.bids = candidate_bids
        self.asks = candidate_asks
        self.schema_version = event.schema_version
        self.data_mode = event.data_mode
        self.venue = event.venue
        self.symbol = event.symbol
        self.price_scale = event.price_scale
        self.quantity_scale = event.quantity_scale
        self.last_event_id = event.event_id
        self.last_update_id = candidate_last_update_id
        self.last_aggregate_trade_id = candidate_last_aggregate_trade_id
        self.last_raw_trade_id = candidate_last_raw_trade_id
        self.event_count += 1
        self.trade_count = candidate_trade_count
        self.resnapshot_count = candidate_resnapshot_count
        self.synchronized = candidate_synchronized
        self.initialized = True


def validate_tape(path: str | Path) -> DepthBook:
    """Replay a canonical tape and return its validated final depth state."""

    book = DepthBook()
    for event in SequencedDepthCsv(path):
        book.apply(event)
    return book


__all__ = [
    "CSV_COLUMNS",
    "DATA_MODES",
    "SCHEMA_VERSION",
    "DepthBook",
    "DepthLevel",
    "MarketDataError",
    "MarketEvent",
    "SequencedDepthCsv",
    "validate_tape",
]
