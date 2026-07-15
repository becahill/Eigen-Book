#!/usr/bin/env python3
"""Capture a canonical Binance Spot depth tape from live source events.

This command opens the diff-depth stream before requesting the initial REST
snapshot, then records only sequenced depth messages that bridge that snapshot.
Aggregate trades may be recorded in the same arrival-ordered tape.  Binance
``bookTicker`` is intentionally unsupported: it is BBO data, not depth.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import importlib.util
import json
import os
import re
import select
import socket
import ssl
import struct
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping, Sequence
from contextlib import AbstractContextManager, contextmanager
from dataclasses import dataclass
from datetime import date, datetime, time as datetime_time, timedelta, timezone
from pathlib import Path
from types import ModuleType
from typing import Any, Final, Protocol, TextIO, cast


def _load_market_data_module() -> ModuleType:
    """Import the installed package, with a source-tree fallback for the CLI."""

    try:
        from eigenbook import market_data

        return market_data
    except ImportError:
        path = Path(__file__).resolve().parent / "src/eigenbook/market_data.py"
        spec = importlib.util.spec_from_file_location(
            "_eigenbook_standalone_market_data",
            path,
        )
        if spec is None or spec.loader is None:
            raise
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module


_market_data = _load_market_data_module()
CSV_COLUMNS = _market_data.CSV_COLUMNS
SCHEMA_VERSION = _market_data.SCHEMA_VERSION
MarketDataError = _market_data.MarketDataError
validate_tape = _market_data.validate_tape


VENUE: Final = "binance_spot"
DEFAULT_SYMBOL: Final = "BTCUSDT"
DEFAULT_PRICE_SCALE: Final = 100_000_000
DEFAULT_QUANTITY_SCALE: Final = 100_000_000
DEFAULT_SNAPSHOT_LIMIT: Final = 5_000
SNAPSHOT_LIMITS: Final = frozenset({5, 10, 20, 50, 100, 500, 1_000, 5_000})
DATA_MODES: Final = frozenset({"depth", "depth_trades"})
SYMBOL_PATTERN: Final = re.compile(r"[A-Z0-9]{2,20}\Z")


class SourceStream(Protocol):
    """One injectable live JSON stream."""

    def receive(self, timeout_seconds: float) -> Mapping[str, Any] | None:
        """Return one message, or ``None`` after a timeout."""


class BinanceSpotNetwork(Protocol):
    """Injectable boundary for all network operations."""

    def open_stream(
        self,
        symbol: str,
        *,
        include_trades: bool,
    ) -> AbstractContextManager[SourceStream]:
        """Open diff depth, optionally multiplexed with aggregate trades."""

    def fetch_depth_snapshot(
        self,
        symbol: str,
        *,
        limit: int,
    ) -> Mapping[str, Any]:
        """Fetch the initial REST depth snapshot."""


@dataclass(frozen=True, slots=True)
class CaptureConfig:
    """Validated capture and output parameters."""

    symbol: str
    start: datetime
    end: datetime
    destination: Path
    data_mode: str
    price_scale: int = DEFAULT_PRICE_SCALE
    quantity_scale: int = DEFAULT_QUANTITY_SCALE
    snapshot_limit: int = DEFAULT_SNAPSHOT_LIMIT

    def __post_init__(self) -> None:
        symbol = self.symbol.upper()
        if not SYMBOL_PATTERN.fullmatch(symbol):
            raise ValueError("symbol must contain 2-20 uppercase letters/digits")
        object.__setattr__(self, "symbol", symbol)
        if self.start.tzinfo is None or self.end.tzinfo is None:
            raise ValueError("start and end must be timezone-aware UTC times")
        start = self.start.astimezone(timezone.utc)
        end = self.end.astimezone(timezone.utc)
        if end <= start:
            raise ValueError("end must be later than start")
        object.__setattr__(self, "start", start)
        object.__setattr__(self, "end", end)
        object.__setattr__(self, "destination", Path(self.destination))
        if self.data_mode not in DATA_MODES:
            raise ValueError(f"data_mode must be one of {sorted(DATA_MODES)}")
        for name, value in (
            ("price_scale", self.price_scale),
            ("quantity_scale", self.quantity_scale),
        ):
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise ValueError(f"{name} must be a positive integer")
        if self.snapshot_limit not in SNAPSHOT_LIMITS:
            raise ValueError(f"snapshot_limit must be one of {sorted(SNAPSHOT_LIMITS)}")


class _WebSocketJsonStream:
    """Small RFC 6455 text client sufficient for Binance public streams."""

    _GUID: Final = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, url: str, *, connect_timeout: float = 10.0) -> None:
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme != "wss" or not parsed.hostname:
            raise ValueError("websocket URL must use wss://")
        port = parsed.port or 443
        raw = socket.create_connection((parsed.hostname, port), timeout=connect_timeout)
        context = ssl.create_default_context()
        self._socket = context.wrap_socket(raw, server_hostname=parsed.hostname)
        self._buffer = bytearray()
        self._fragments = bytearray()
        self._fragment_opcode: int | None = None
        self._closed = False
        self._frame_timeout_seconds = max(1.0, connect_timeout)

        key = base64.b64encode(os.urandom(16)).decode("ascii")
        resource = parsed.path or "/"
        if parsed.query:
            resource += f"?{parsed.query}"
        host = parsed.hostname if port == 443 else f"{parsed.hostname}:{port}"
        request = (
            f"GET {resource} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode("ascii")
        self._socket.sendall(request)
        response = self._read_http_headers()
        status = response.split(b"\r\n", 1)[0]
        if b" 101 " not in status:
            self.close()
            decoded_status = status.decode(errors="replace")
            raise OSError(f"websocket upgrade failed: {decoded_status}")
        expected = base64.b64encode(
            hashlib.sha1((key + self._GUID).encode("ascii")).digest()
        ).decode("ascii")
        headers: dict[str, str] = {}
        for line in response.split(b"\r\n")[1:]:
            if b":" not in line:
                continue
            name, value = line.split(b":", 1)
            headers[name.decode("ascii").strip().lower()] = value.decode(
                "ascii"
            ).strip()
        if headers.get("sec-websocket-accept") != expected:
            self.close()
            raise OSError("websocket server returned an invalid accept key")

    def _read_http_headers(self) -> bytes:
        while b"\r\n\r\n" not in self._buffer:
            chunk = self._socket.recv(4096)
            if not chunk:
                raise OSError("websocket closed during HTTP upgrade")
            self._buffer.extend(chunk)
            if len(self._buffer) > 64 * 1024:
                raise OSError("websocket upgrade headers are too large")
        boundary = self._buffer.index(b"\r\n\r\n") + 4
        headers = bytes(self._buffer[:boundary])
        del self._buffer[:boundary]
        return headers

    def _recv_exact(self, size: int) -> bytes:
        while len(self._buffer) < size:
            chunk = self._socket.recv(max(4096, size - len(self._buffer)))
            if not chunk:
                raise EOFError("websocket stream closed")
            self._buffer.extend(chunk)
        result = bytes(self._buffer[:size])
        del self._buffer[:size]
        return result

    def _send_control(self, opcode: int, payload: bytes) -> None:
        mask = os.urandom(4)
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        header = bytearray((0x80 | opcode,))
        length = len(payload)
        if length <= 125:
            header.append(0x80 | length)
        else:
            raise ValueError("websocket control frame exceeds 125 bytes")
        self._socket.sendall(bytes(header) + mask + masked)

    def _receive_text(self) -> str:
        while True:
            first, second = self._recv_exact(2)
            final = bool(first & 0x80)
            opcode = first & 0x0F
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8))[0]
            mask = self._recv_exact(4) if masked else b""
            payload = self._recv_exact(length)
            if masked:
                payload = bytes(
                    value ^ mask[index % 4] for index, value in enumerate(payload)
                )

            if opcode == 0x8:
                self._closed = True
                raise EOFError("websocket server closed the stream")
            if opcode == 0x9:
                self._send_control(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode not in {0x0, 0x1}:
                raise OSError(f"unsupported websocket opcode {opcode}")

            if opcode == 0x1:
                if self._fragment_opcode is not None:
                    raise OSError("nested websocket text fragments")
                self._fragment_opcode = opcode
                self._fragments.clear()
            elif self._fragment_opcode is None:
                raise OSError("unexpected websocket continuation frame")
            self._fragments.extend(payload)
            if final:
                result = self._fragments.decode("utf-8")
                self._fragments.clear()
                self._fragment_opcode = None
                return result

    def receive(self, timeout_seconds: float) -> Mapping[str, Any] | None:
        if self._closed:
            raise EOFError("websocket stream is closed")
        idle_timeout = max(0.001, timeout_seconds)
        if (
            not self._buffer
            and self._fragment_opcode is None
            and self._socket.pending() == 0
        ):
            readable, _, _ = select.select(
                (self._socket,),
                (),
                (),
                idle_timeout,
            )
            if not readable:
                return None
        self._socket.settimeout(self._frame_timeout_seconds)
        try:
            text = self._receive_text()
        except socket.timeout as error:
            raise OSError(
                "websocket timed out after frame parsing began; reconnect and "
                "resnapshot are required"
            ) from error
        try:
            value = json.loads(text)
        except json.JSONDecodeError as error:
            raise MarketDataError("Binance websocket returned invalid JSON") from error
        if not isinstance(value, Mapping):
            raise MarketDataError("Binance websocket message is not a JSON object")
        return cast(Mapping[str, Any], value)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._send_control(0x8, b"")
        except OSError:
            pass
        self._socket.close()


class LiveBinanceSpotNetwork:
    """Standard-library Binance Spot REST and WebSocket implementation."""

    def __init__(
        self,
        *,
        rest_base_url: str = "https://api.binance.com",
        stream_base_url: str = "wss://stream.binance.com:9443",
        request_timeout: float = 15.0,
    ) -> None:
        self.rest_base_url = rest_base_url.rstrip("/")
        self.stream_base_url = stream_base_url.rstrip("/")
        self.request_timeout = request_timeout

    def fetch_depth_snapshot(
        self,
        symbol: str,
        *,
        limit: int,
    ) -> Mapping[str, Any]:
        query = urllib.parse.urlencode({"symbol": symbol, "limit": limit})
        url = f"{self.rest_base_url}/api/v3/depth?{query}"
        with urllib.request.urlopen(url, timeout=self.request_timeout) as response:
            payload = response.read()
        try:
            value = json.loads(payload)
        except json.JSONDecodeError as error:
            raise MarketDataError("Binance snapshot returned invalid JSON") from error
        if not isinstance(value, Mapping):
            raise MarketDataError("Binance snapshot is not a JSON object")
        return cast(Mapping[str, Any], value)

    @contextmanager
    def open_stream(
        self,
        symbol: str,
        *,
        include_trades: bool,
    ):
        stream_names = [f"{symbol.lower()}@depth@100ms"]
        if include_trades:
            stream_names.append(f"{symbol.lower()}@aggTrade")
        streams = "/".join(stream_names)
        connection = _WebSocketJsonStream(
            f"{self.stream_base_url}/stream?streams={streams}"
        )
        try:
            yield connection
        finally:
            connection.close()


def _source_uint(value: Any, name: str, context: str) -> int:
    if type(value) is int:
        parsed = value
    elif type(value) is str and value and value.isascii() and value.isdecimal():
        parsed = int(value, 10)
    else:
        raise MarketDataError(f"{context}: {name} is not an integer")
    if parsed < 0 or parsed > (1 << 64) - 1:
        raise MarketDataError(f"{context}: {name} is outside uint64 range")
    return parsed


def _source_decimal_text(value: Any, name: str, context: str) -> str:
    if type(value) is not str or not value.strip():
        raise MarketDataError(f"{context}: {name} must be a non-empty decimal string")
    return value


def _source_levels(value: Any, name: str, context: str) -> list[tuple[str, str]]:
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes)):
        raise MarketDataError(f"{context}: {name} must be an array")
    result: list[tuple[str, str]] = []
    for index, raw_level in enumerate(value):
        if (
            not isinstance(raw_level, Sequence)
            or isinstance(raw_level, (str, bytes))
            or len(raw_level) != 2
        ):
            raise MarketDataError(
                f"{context}: {name}[{index}] must be exactly [price, size]"
            )
        result.append(
            (
                _source_decimal_text(
                    raw_level[0],
                    f"{name}[{index}].price",
                    context,
                ),
                _source_decimal_text(
                    raw_level[1],
                    f"{name}[{index}].size",
                    context,
                ),
            )
        )
    return result


def _unwrap_message(message: Mapping[str, Any]) -> Mapping[str, Any]:
    data = message.get("data", message)
    if not isinstance(data, Mapping):
        raise MarketDataError("Binance combined stream data is not an object")
    return cast(Mapping[str, Any], data)


def _metadata_row(
    config: CaptureConfig,
    *,
    event_id: int,
    event_time: int,
    event_kind: str,
    first_update_id: int | str = "",
    last_update_id: int | str = "",
    previous_update_id: int | str = "",
    trade_id: int | str = "",
    first_trade_id: int | str = "",
    last_trade_id: int | str = "",
    aggressor_side: str = "",
) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "data_mode": config.data_mode,
        "venue": VENUE,
        "symbol": config.symbol,
        "price_scale": config.price_scale,
        "quantity_scale": config.quantity_scale,
        "event_id": event_id,
        "event_time": event_time,
        "event_kind": event_kind,
        "first_update_id": first_update_id,
        "last_update_id": last_update_id,
        "previous_update_id": previous_update_id,
        "trade_id": trade_id,
        "first_trade_id": first_trade_id,
        "last_trade_id": last_trade_id,
        "aggressor_side": aggressor_side,
        "side": "",
        "price": "",
        "size": "",
    }


def _write_depth_event(
    writer: csv.DictWriter,
    metadata: dict[str, Any],
    bids: list[tuple[str, str]],
    asks: list[tuple[str, str]],
) -> None:
    if not bids and not asks:
        raise MarketDataError(
            f"event_id={metadata['event_id']} "
            f"update_ids={metadata['first_update_id']}-{metadata['last_update_id']} "
            "source_message: depth event has no changes"
        )
    for side, levels in (("bid", bids), ("ask", asks)):
        for price, size in levels:
            row = dict(metadata)
            row["side"] = side
            row["price"] = price
            row["size"] = size
            writer.writerow(row)


def _wait_until(
    target: datetime,
    *,
    now: Callable[[], datetime],
    sleep: Callable[[float], None],
) -> None:
    while True:
        remaining = (target - now().astimezone(timezone.utc)).total_seconds()
        if remaining <= 0:
            return
        sleep(min(remaining, 1.0))


def _write_live_tape(
    file: TextIO,
    config: CaptureConfig,
    *,
    network: BinanceSpotNetwork,
    now: Callable[[], datetime],
    sleep: Callable[[float], None],
) -> None:
    _wait_until(config.start, now=now, sleep=sleep)
    current = now().astimezone(timezone.utc)
    if current >= config.end:
        raise MarketDataError(
            "requested UTC capture range has already ended; live capture "
            "does not backfill historical data"
        )

    include_trades = config.data_mode == "depth_trades"
    writer = csv.DictWriter(file, fieldnames=CSV_COLUMNS, lineterminator="\n")
    writer.writeheader()

    with network.open_stream(
        config.symbol,
        include_trades=include_trades,
    ) as stream:
        snapshot = network.fetch_depth_snapshot(
            config.symbol,
            limit=config.snapshot_limit,
        )
        snapshot_id = _source_uint(
            snapshot.get("lastUpdateId"),
            "lastUpdateId",
            "snapshot",
        )
        bids = _source_levels(snapshot.get("bids"), "bids", "snapshot")
        asks = _source_levels(snapshot.get("asks"), "asks", "snapshot")
        snapshot_time = int(now().astimezone(timezone.utc).timestamp() * 1_000)
        event_id = 1
        _write_depth_event(
            writer,
            _metadata_row(
                config,
                event_id=event_id,
                event_time=snapshot_time,
                event_kind="snapshot",
                first_update_id=snapshot_id,
                last_update_id=snapshot_id,
            ),
            bids,
            asks,
        )

        last_update_id = snapshot_id
        synchronized = False
        synchronization_event_time: int | None = None
        last_aggregate_trade_id: int | None = None
        last_raw_trade_id: int | None = None
        start_ms = int(config.start.timestamp() * 1_000)
        end_ms = int(config.end.timestamp() * 1_000)
        while now().astimezone(timezone.utc) < config.end:
            remaining = (config.end - now().astimezone(timezone.utc)).total_seconds()
            message = stream.receive(min(max(remaining, 0.001), 1.0))
            if message is None:
                continue
            payload = _unwrap_message(message)
            source_kind = payload.get("e")
            source_symbol = str(payload.get("s", "")).upper()
            if source_symbol != config.symbol:
                raise MarketDataError(
                    f"source event symbol mismatch: expected={config.symbol}, "
                    f"found={source_symbol or '<missing>'}"
                )

            if source_kind == "depthUpdate":
                first_update_id = _source_uint(payload.get("U"), "U", "depth")
                final_update_id = _source_uint(payload.get("u"), "u", "depth")
                event_time = _source_uint(payload.get("E"), "E", "depth")
                previous_raw = payload.get("pu")
                previous_update_id = (
                    None
                    if previous_raw is None
                    else _source_uint(previous_raw, "pu", "depth")
                )
                if final_update_id <= last_update_id:
                    continue
                expected = last_update_id + 1
                context = (
                    f"source update_ids={first_update_id}-{final_update_id} "
                    f"previous_update_id={previous_update_id}"
                )
                if previous_update_id is not None and (
                    previous_update_id != last_update_id
                ):
                    raise MarketDataError(
                        f"{context}: depth continuity gap; "
                        f"expected_previous_update_id={last_update_id}"
                    )
                if not first_update_id <= expected <= final_update_id:
                    raise MarketDataError(
                        f"{context}: depth continuity gap; "
                        f"expected_update_id={expected}"
                    )
                depth_bids = _source_levels(payload.get("b"), "b", context)
                depth_asks = _source_levels(payload.get("a"), "a", context)
                if event_time >= end_ms:
                    break
                event_id += 1
                _write_depth_event(
                    writer,
                    _metadata_row(
                        config,
                        event_id=event_id,
                        event_time=event_time,
                        event_kind="depth_update",
                        first_update_id=first_update_id,
                        last_update_id=final_update_id,
                        previous_update_id=(
                            "" if previous_update_id is None else previous_update_id
                        ),
                    ),
                    depth_bids,
                    depth_asks,
                )
                last_update_id = final_update_id
                if not synchronized:
                    synchronization_event_time = event_time
                synchronized = True
            elif source_kind == "aggTrade":
                if not include_trades:
                    raise MarketDataError("aggregate trade arrived in depth-only mode")
                # Trades buffered before the first update bridges the REST
                # snapshot cannot be placed causally against a synchronized
                # reconstructed book, so fail closed by excluding them.
                if not synchronized:
                    continue
                event_time = _source_uint(
                    payload.get("T", payload.get("E")),
                    "T",
                    "aggregate trade",
                )
                assert synchronization_event_time is not None
                if event_time < synchronization_event_time:
                    continue
                if event_time < start_ms:
                    continue
                if event_time >= end_ms:
                    break
                trade_id = _source_uint(payload.get("a"), "a", "aggregate trade")
                first_trade_id = _source_uint(
                    payload.get("f"),
                    "f",
                    f"aggregate_trade_id={trade_id}",
                )
                last_source_trade_id = _source_uint(
                    payload.get("l"),
                    "l",
                    f"aggregate_trade_id={trade_id}",
                )
                trade_context = (
                    f"aggregate_trade_id={trade_id} "
                    f"raw_trade_ids={first_trade_id}-{last_source_trade_id}"
                )
                if first_trade_id > last_source_trade_id:
                    raise MarketDataError(
                        f"{trade_context}: first raw trade id exceeds last"
                    )
                if (
                    last_aggregate_trade_id is not None
                    and trade_id != last_aggregate_trade_id + 1
                ):
                    raise MarketDataError(
                        f"{trade_context}: aggregate trade continuity gap; "
                        "expected_aggregate_trade_id="
                        f"{last_aggregate_trade_id + 1}"
                    )
                if (
                    last_raw_trade_id is not None
                    and first_trade_id != last_raw_trade_id + 1
                ):
                    raise MarketDataError(
                        f"{trade_context}: raw trade continuity gap; "
                        f"expected_first_trade_id={last_raw_trade_id + 1}"
                    )
                price = _source_decimal_text(
                    payload.get("p"),
                    "p",
                    trade_context,
                )
                size = _source_decimal_text(
                    payload.get("q"),
                    "q",
                    trade_context,
                )
                buyer_is_maker = payload.get("m")
                if not isinstance(buyer_is_maker, bool):
                    raise MarketDataError(f"trade_id={trade_id}: m must be a boolean")
                aggressor_side = "sell" if buyer_is_maker else "buy"
                event_id += 1
                row = _metadata_row(
                    config,
                    event_id=event_id,
                    event_time=event_time,
                    event_kind="trade",
                    trade_id=trade_id,
                    first_trade_id=first_trade_id,
                    last_trade_id=last_source_trade_id,
                    aggressor_side=aggressor_side,
                )
                row["price"] = price
                row["size"] = size
                writer.writerow(row)
                last_aggregate_trade_id = trade_id
                last_raw_trade_id = last_source_trade_id
            else:
                raise MarketDataError(
                    f"unsupported Binance stream event kind {source_kind!r}"
                )

        if not synchronized:
            raise MarketDataError(
                "capture ended before a diff-depth update bridged the initial "
                f"snapshot lastUpdateId={snapshot_id}; no tape was published"
            )

    file.flush()
    os.fsync(file.fileno())


def download_tape(
    config: CaptureConfig,
    *,
    network: BinanceSpotNetwork | None = None,
    now: Callable[[], datetime] | None = None,
    sleep: Callable[[float], None] = time.sleep,
) -> Path:
    """Capture, fully validate, and atomically publish one tape."""

    source = LiveBinanceSpotNetwork() if network is None else network
    clock = (lambda: datetime.now(timezone.utc)) if now is None else now
    destination = config.destination.expanduser().resolve()
    if destination.exists() and destination.is_dir():
        raise IsADirectoryError(f"destination is a directory: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        suffix=".tmp",
        dir=destination.parent,
        text=True,
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(
            descriptor,
            "w",
            newline="",
            encoding="utf-8",
        ) as file:
            _write_live_tape(
                file,
                config,
                network=source,
                now=clock,
                sleep=sleep,
            )
        validate_tape(temporary_path)
        os.replace(temporary_path, destination)
        return destination
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        temporary_path.unlink(missing_ok=True)
        raise


def _utc_datetime(value: str) -> datetime:
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "must be ISO-8601 with an explicit UTC offset"
        ) from error
    if parsed.tzinfo is None:
        raise argparse.ArgumentTypeError("must include Z or an explicit UTC offset")
    parsed = parsed.astimezone(timezone.utc)
    return parsed


def _utc_date(value: str) -> date:
    try:
        return date.fromisoformat(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be YYYY-MM-DD") from error


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a positive integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def parse_args(arguments: Sequence[str] | None = None) -> CaptureConfig:
    parser = argparse.ArgumentParser(
        description=(
            "Capture Binance Spot initial depth plus sequenced diff-depth "
            "updates, optionally with aggregate trades. bookTicker/BBO is "
            "not accepted as depth."
        )
    )
    parser.add_argument("--symbol", default=DEFAULT_SYMBOL)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument(
        "--data-mode",
        choices=sorted(DATA_MODES),
        required=True,
        help="depth, or depth plus aggregate trades",
    )
    window = parser.add_mutually_exclusive_group(required=True)
    window.add_argument(
        "--date",
        type=_utc_date,
        help="UTC date; live capture starts no earlier than invocation",
    )
    window.add_argument(
        "--start",
        type=_utc_datetime,
        help="inclusive UTC ISO-8601 start",
    )
    parser.add_argument(
        "--end",
        type=_utc_datetime,
        help="exclusive UTC ISO-8601 end; required with --start",
    )
    parser.add_argument(
        "--price-scale",
        type=_positive_int,
        default=DEFAULT_PRICE_SCALE,
    )
    parser.add_argument(
        "--quantity-scale",
        type=_positive_int,
        default=DEFAULT_QUANTITY_SCALE,
    )
    parser.add_argument(
        "--snapshot-limit",
        type=int,
        choices=sorted(SNAPSHOT_LIMITS),
        default=DEFAULT_SNAPSHOT_LIMIT,
    )
    args = parser.parse_args(arguments)

    if args.date is not None:
        if args.end is not None:
            parser.error("--end cannot be combined with --date")
        start = datetime.combine(args.date, datetime_time.min, tzinfo=timezone.utc)
        end = start + timedelta(days=1)
    else:
        if args.end is None:
            parser.error("--end is required with --start")
        start = args.start
        end = args.end

    try:
        return CaptureConfig(
            symbol=args.symbol,
            start=start,
            end=end,
            destination=args.destination,
            data_mode=args.data_mode,
            price_scale=args.price_scale,
            quantity_scale=args.quantity_scale,
            snapshot_limit=args.snapshot_limit,
        )
    except ValueError as error:
        parser.error(str(error))
        raise AssertionError("argparse parser.error must not return")


def main(
    arguments: Sequence[str] | None = None,
    *,
    network: BinanceSpotNetwork | None = None,
    now: Callable[[], datetime] | None = None,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    config = parse_args(arguments)
    try:
        output = download_tape(
            config,
            network=network,
            now=now,
            sleep=sleep,
        )
    except (MarketDataError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"validated canonical market-data tape: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
