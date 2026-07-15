from __future__ import annotations

from collections.abc import Mapping
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
import socket
from typing import Any

import pytest

import fetch_l2_data as downloader
from eigenbook.market_data import (
    DepthBook,
    MarketDataError,
    SequencedDepthCsv,
    validate_tape,
)


FIXTURES = Path(__file__).parent / "fixtures" / "market_data"
START = datetime(2024, 1, 1, 0, 0, 0, tzinfo=timezone.utc)


class FakeClock:
    def __init__(self, current: datetime) -> None:
        self.current = current

    def __call__(self) -> datetime:
        return self.current

    def advance(self, seconds: float) -> None:
        self.current += timedelta(seconds=seconds)

    def sleep(self, seconds: float) -> None:
        self.advance(seconds)


class FakeStream:
    def __init__(
        self,
        messages: list[Mapping[str, Any]],
        clock: FakeClock,
        end: datetime,
    ) -> None:
        self.messages = list(messages)
        self.clock = clock
        self.end = end

    def receive(self, timeout_seconds: float) -> Mapping[str, Any] | None:
        if self.messages:
            self.clock.advance(min(timeout_seconds, 0.05))
            return self.messages.pop(0)
        self.clock.current = self.end
        return None


class FakeNetwork:
    def __init__(
        self,
        *,
        snapshot: Mapping[str, Any],
        messages: list[Mapping[str, Any]],
        clock: FakeClock,
        end: datetime,
        snapshot_error: Exception | None = None,
    ) -> None:
        self.snapshot = snapshot
        self.messages = messages
        self.clock = clock
        self.end = end
        self.snapshot_error = snapshot_error
        self.opened_before_snapshot = False
        self.include_trades: bool | None = None

    @contextmanager
    def open_stream(self, symbol: str, *, include_trades: bool):
        assert symbol == "BTCUSDT"
        self.opened_before_snapshot = True
        self.include_trades = include_trades
        yield FakeStream(self.messages, self.clock, self.end)

    def fetch_depth_snapshot(self, symbol: str, *, limit: int):
        assert self.opened_before_snapshot
        assert symbol == "BTCUSDT"
        assert limit == 5_000
        if self.snapshot_error is not None:
            raise self.snapshot_error
        return self.snapshot


def canonical_snapshot() -> dict[str, Any]:
    return {
        "lastUpdateId": 100,
        "bids": [["100.00", "5.000"]],
        "asks": [["101.00", "4.000"]],
    }


def canonical_messages() -> list[Mapping[str, Any]]:
    return [
        {
            "data": {
                "e": "depthUpdate",
                "E": 1_704_067_200_100,
                "s": "BTCUSDT",
                "U": 101,
                "u": 101,
                "b": [["102.00", "6.000"], ["100.00", "0"]],
                "a": [["103.00", "7.000"], ["101.00", "0"]],
            }
        },
        {
            "data": {
                "e": "aggTrade",
                "E": 1_704_067_200_150,
                "T": 1_704_067_200_150,
                "s": "BTCUSDT",
                "a": 900,
                "f": 1_000,
                "l": 1_003,
                "p": "103.00",
                "q": "0.400",
                "m": False,
            }
        },
        {
            "data": {
                "e": "depthUpdate",
                "E": 1_704_067_200_200,
                "s": "BTCUSDT",
                "U": 102,
                "u": 103,
                "pu": 101,
                "b": [["102.00", "5.000"]],
                "a": [["103.00", "6.000"]],
            }
        },
    ]


def capture_config(destination: Path, *, data_mode: str = "depth_trades"):
    return downloader.CaptureConfig(
        symbol="btcusdt",
        start=START,
        end=START + timedelta(seconds=1),
        destination=destination,
        data_mode=data_mode,
        price_scale=100,
        quantity_scale=1_000,
    )


def test_reader_groups_atomic_events_by_event_id_not_timestamp() -> None:
    events = list(SequencedDepthCsv(FIXTURES / "valid_atomic.csv"))

    assert [event.event_id for event in events] == [1, 2, 3, 4]
    assert [event.kind for event in events] == [
        "snapshot",
        "depth_update",
        "trade",
        "depth_update",
    ]
    assert events[0].event_time == events[1].event_time == events[2].event_time
    assert len(events[1].levels) == 4
    assert {(level.side, level.price, level.size) for level in events[1].levels} == {
        ("bid", 10_200, 6_000),
        ("ask", 10_300, 7_000),
        ("bid", 10_000, 0),
        ("ask", 10_100, 0),
    }
    assert events[2].trade_id == 900
    assert events[2].first_trade_id == 1_000
    assert events[2].last_trade_id == 1_003
    assert events[2].aggressor_side == "buy"
    assert events[2].trade_price == 10_300
    assert events[2].trade_size == 400


def test_atomic_multi_level_update_commits_only_final_uncrossed_state() -> None:
    events = list(SequencedDepthCsv(FIXTURES / "valid_atomic.csv"))
    book = DepthBook()
    book.apply(events[0])
    assert (book.best_bid, book.best_ask) == (10_000, 10_100)
    assert book.synchronized is False

    # Adding 10_200 bid before removing the old 10_100 ask would cross if the
    # physical rows were exposed individually. One apply observes all changes.
    book.apply(events[1])
    assert book.synchronized is True
    assert (book.best_bid, book.best_ask) == (10_200, 10_300)
    assert 10_000 not in book.bids
    assert 10_100 not in book.asks
    book.apply(events[2])
    book.apply(events[3])
    assert book.event_count == 4
    assert book.trade_count == 1
    assert book.last_update_id == 103


def test_initial_snapshot_without_bridge_is_rejected(tmp_path: Path) -> None:
    lines = (FIXTURES / "valid_atomic.csv").read_text(encoding="utf-8").splitlines()
    path = tmp_path / "snapshot-only.csv"
    path.write_text("\n".join(lines[:3]) + "\n", encoding="utf-8")

    with pytest.raises(MarketDataError, match="ended before.*bridged"):
        validate_tape(path)


def test_resnapshot_without_bridge_is_rejected(tmp_path: Path) -> None:
    lines = (FIXTURES / "resnapshot.csv").read_text(encoding="utf-8").splitlines()
    path = tmp_path / "unbridged-resnapshot.csv"
    path.write_text("\n".join(lines[:6]) + "\n", encoding="utf-8")

    with pytest.raises(MarketDataError, match="ended before.*bridged"):
        validate_tape(path)


def test_sequence_gap_fails_closed_with_source_ids_and_rows() -> None:
    with pytest.raises(MarketDataError) as raised:
        list(SequencedDepthCsv(FIXTURES / "sequence_gap.csv"))

    message = str(raised.value)
    assert "event_id=2" in message
    assert "update_ids=102-102" in message
    assert "rows=4" in message
    assert "expected_update_id=101" in message


def test_previous_update_id_mismatch_is_rejected(tmp_path: Path) -> None:
    text = (FIXTURES / "valid_atomic.csv").read_text(encoding="utf-8")
    path = tmp_path / "previous-gap.csv"
    path.write_text(text.replace(",101,101,100,,,", ",101,101,99,,,"), encoding="utf-8")

    with pytest.raises(MarketDataError) as raised:
        list(SequencedDepthCsv(path))
    message = str(raised.value)
    assert "event_id=2" in message
    assert "previous_update_id=99" in message
    assert "expected_previous_update_id=100" in message
    assert "rows=4-7" in message


@pytest.mark.parametrize(
    ("trade_id", "first_trade_id", "expected"),
    [
        (902, 1_004, "expected_aggregate_trade_id=901"),
        (901, 1_005, "expected_first_trade_id=1004"),
    ],
)
def test_trade_sequence_gaps_are_rejected(
    tmp_path: Path,
    trade_id: int,
    first_trade_id: int,
    expected: str,
) -> None:
    source = (FIXTURES / "valid_atomic.csv").read_text(encoding="utf-8")
    extra = (
        "eigenbook.market_data.v2,depth_trades,binance_spot,BTCUSDT,"
        f"100,1000,5,1700000000002,trade,,,,{trade_id},"
        f"{first_trade_id},{first_trade_id},sell,,102.00,0.100\n"
    )
    path = tmp_path / "trade-gap.csv"
    path.write_text(source + extra, encoding="utf-8")

    with pytest.raises(MarketDataError) as raised:
        validate_tape(path)
    message = str(raised.value)
    assert "event_id=5" in message
    assert expected in message


def test_explicit_resnapshot_starts_new_sequence_epoch() -> None:
    book = validate_tape(FIXTURES / "resnapshot.csv")

    assert book.resnapshot_count == 1
    assert book.last_update_id == 501
    assert book.best_bid == 20_000
    assert book.best_ask == 20_100
    assert 10_000 not in book.bids


def test_stale_resnapshot_is_rejected(tmp_path: Path) -> None:
    source = (FIXTURES / "resnapshot.csv").read_text(encoding="utf-8")
    path = tmp_path / "stale-resnapshot.csv"
    path.write_text(
        source.replace(",500,500,,,,", ",101,101,,,,"),
        encoding="utf-8",
    )

    with pytest.raises(MarketDataError) as raised:
        validate_tape(path)
    message = str(raised.value)
    assert "event_id=3" in message
    assert "stale resnapshot" in message
    assert "previous_last_update_id=101" in message


def test_crossed_update_is_transactional_and_diagnostic() -> None:
    events = list(SequencedDepthCsv(FIXTURES / "crossed.csv"))
    book = DepthBook()
    book.apply(events[0])
    before_bids = dict(book.bids)
    before_asks = dict(book.asks)

    with pytest.raises(MarketDataError) as raised:
        book.apply(events[1])
    message = str(raised.value)
    assert "event_id=2" in message
    assert "update_ids=101-101" in message
    assert "rows=4" in message
    assert "crossed/locked" in message
    assert book.bids == before_bids
    assert book.asks == before_asks
    assert book.last_event_id == 1
    assert book.last_update_id == 100


def test_invalid_fixed_point_row_reports_event_source_and_row() -> None:
    with pytest.raises(MarketDataError) as raised:
        list(SequencedDepthCsv(FIXTURES / "invalid_precision.csv"))

    message = str(raised.value)
    assert "event_id=1" in message
    assert "update_ids=100-100" in message
    assert "rows=2-3" in message
    assert "row=2" in message
    assert "exceeds scale=100 precision" in message


def test_blank_physical_row_is_not_silently_skipped(tmp_path: Path) -> None:
    source = (FIXTURES / "valid_atomic.csv").read_text(encoding="utf-8")
    lines = source.splitlines()
    path = tmp_path / "blank-row.csv"
    path.write_text("\n".join((*lines[:3], "", *lines[3:])) + "\n", encoding="utf-8")

    with pytest.raises(MarketDataError) as raised:
        list(SequencedDepthCsv(path))
    message = str(raised.value)
    assert "event_id=<missing>" in message
    assert "rows=4" in message
    assert "blank physical CSV row" in message


def test_source_numbers_cannot_arrive_as_floats() -> None:
    with pytest.raises(MarketDataError, match="U is not an integer"):
        downloader._source_uint(101.5, "U", "depth")
    with pytest.raises(MarketDataError, match="decimal string"):
        downloader._source_levels([[100.0, "1.0"]], "b", "depth")
    with pytest.raises(MarketDataError, match="exactly"):
        downloader._source_levels(
            [["100.0", "1.0", "unexpected"]],
            "b",
            "depth",
        )


class TimeoutFrameSocket:
    def __init__(self) -> None:
        self.timeout: float | None = None

    def pending(self) -> int:
        return 0

    def settimeout(self, value: float) -> None:
        self.timeout = value

    def recv(self, size: int) -> bytes:
        del size
        raise socket.timeout("fixture partial frame")


def test_websocket_timeout_mid_frame_is_fatal() -> None:
    stream = object.__new__(downloader._WebSocketJsonStream)
    stream._socket = TimeoutFrameSocket()
    # Complete unmasked text header, but only two of five payload bytes. The
    # next call must never reinterpret those payload bytes as a fresh header.
    stream._buffer = bytearray(b"\x81\x05ab")
    stream._fragments = bytearray()
    stream._fragment_opcode = None
    stream._closed = False
    stream._frame_timeout_seconds = 1.0

    with pytest.raises(OSError, match="frame parsing began"):
        stream.receive(0.1)


def test_downloader_writes_validated_atomic_tape_then_replays(tmp_path: Path) -> None:
    destination = tmp_path / "capture.csv"
    clock = FakeClock(START)
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=canonical_messages(),
        clock=clock,
        end=START + timedelta(seconds=1),
    )

    output = downloader.download_tape(
        capture_config(destination),
        network=network,
        now=clock,
        sleep=clock.sleep,
    )

    assert output == destination.resolve()
    assert network.opened_before_snapshot
    assert network.include_trades is True
    events = list(SequencedDepthCsv(output))
    assert [event.kind for event in events] == [
        "snapshot",
        "depth_update",
        "trade",
        "depth_update",
    ]
    assert len(events[1].levels) == 4
    final_book = validate_tape(output)
    assert (final_book.best_bid, final_book.best_ask) == (10_200, 10_300)
    assert final_book.last_update_id == 103
    assert final_book.last_aggregate_trade_id == 900
    assert list(tmp_path.glob(".capture.csv.*.tmp")) == []


def test_trade_older_than_synchronizing_update_is_excluded(
    tmp_path: Path,
) -> None:
    destination = tmp_path / "capture.csv"
    clock = FakeClock(START)
    messages = canonical_messages()
    delayed_trade = messages[1]["data"]
    assert isinstance(delayed_trade, dict)
    delayed_trade["E"] = 1_704_067_200_050
    delayed_trade["T"] = 1_704_067_200_050
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=messages,
        clock=clock,
        end=START + timedelta(seconds=1),
    )

    output = downloader.download_tape(
        capture_config(destination),
        network=network,
        now=clock,
        sleep=clock.sleep,
    )

    events = list(SequencedDepthCsv(output))
    assert [event.kind for event in events] == [
        "snapshot",
        "depth_update",
        "depth_update",
    ]
    assert validate_tape(output).last_aggregate_trade_id is None


def test_downloader_gap_cleans_temp_and_preserves_destination(tmp_path: Path) -> None:
    destination = tmp_path / "capture.csv"
    destination.write_text("keep-me\n", encoding="utf-8")
    clock = FakeClock(START)
    gap_message: Mapping[str, Any] = {
        "data": {
            "e": "depthUpdate",
            "E": 1_704_067_200_100,
            "s": "BTCUSDT",
            "U": 102,
            "u": 102,
            "b": [["100.00", "6.000"]],
            "a": [],
        }
    }
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=[gap_message],
        clock=clock,
        end=START + timedelta(seconds=1),
    )

    with pytest.raises(MarketDataError, match="expected_update_id=101"):
        downloader.download_tape(
            capture_config(destination),
            network=network,
            now=clock,
            sleep=clock.sleep,
        )

    assert destination.read_text(encoding="utf-8") == "keep-me\n"
    assert list(tmp_path.glob(".capture.csv.*.tmp")) == []


def test_downloader_trade_gap_cleans_temp_and_preserves_destination(
    tmp_path: Path,
) -> None:
    destination = tmp_path / "capture.csv"
    destination.write_text("keep-me\n", encoding="utf-8")
    clock = FakeClock(START)
    messages = canonical_messages()
    messages.insert(
        2,
        {
            "data": {
                "e": "aggTrade",
                "E": 1_704_067_200_160,
                "T": 1_704_067_200_160,
                "s": "BTCUSDT",
                "a": 901,
                "f": 1_005,
                "l": 1_005,
                "p": "102.00",
                "q": "0.100",
                "m": True,
            }
        },
    )
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=messages,
        clock=clock,
        end=START + timedelta(seconds=1),
    )

    with pytest.raises(MarketDataError, match="raw trade continuity gap"):
        downloader.download_tape(
            capture_config(destination),
            network=network,
            now=clock,
            sleep=clock.sleep,
        )

    assert destination.read_text(encoding="utf-8") == "keep-me\n"
    assert list(tmp_path.glob(".capture.csv.*.tmp")) == []


def test_post_write_validation_failure_never_replaces_destination(
    tmp_path: Path,
) -> None:
    destination = tmp_path / "capture.csv"
    destination.write_text("validated-old-tape\n", encoding="utf-8")
    clock = FakeClock(START)
    crossed_message: Mapping[str, Any] = {
        "data": {
            "e": "depthUpdate",
            "E": 1_704_067_200_100,
            "s": "BTCUSDT",
            "U": 101,
            "u": 101,
            "b": [["102.00", "1.000"]],
            "a": [],
        }
    }
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=[crossed_message],
        clock=clock,
        end=START + timedelta(seconds=1),
    )

    with pytest.raises(MarketDataError, match="crossed/locked"):
        downloader.download_tape(
            capture_config(destination),
            network=network,
            now=clock,
            sleep=clock.sleep,
        )

    assert destination.read_text(encoding="utf-8") == "validated-old-tape\n"
    assert list(tmp_path.glob(".capture.csv.*.tmp")) == []


def test_snapshot_without_bridging_update_is_not_published(tmp_path: Path) -> None:
    destination = tmp_path / "capture.csv"
    destination.write_text("existing\n", encoding="utf-8")
    clock = FakeClock(START)
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=[],
        clock=clock,
        end=START + timedelta(seconds=1),
    )

    with pytest.raises(MarketDataError, match="before a diff-depth update bridged"):
        downloader.download_tape(
            capture_config(destination),
            network=network,
            now=clock,
            sleep=clock.sleep,
        )

    assert destination.read_text(encoding="utf-8") == "existing\n"
    assert list(tmp_path.glob(".capture.csv.*.tmp")) == []


def test_main_returns_nonzero_and_preserves_destination_on_failure(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    destination = tmp_path / "capture.csv"
    destination.write_text("existing\n", encoding="utf-8")
    clock = FakeClock(START)
    network = FakeNetwork(
        snapshot=canonical_snapshot(),
        messages=[],
        clock=clock,
        end=START + timedelta(seconds=1),
        snapshot_error=OSError("fixture network failure"),
    )

    result = downloader.main(
        [
            "--symbol",
            "BTCUSDT",
            "--start",
            "2024-01-01T00:00:00Z",
            "--end",
            "2024-01-01T00:00:01Z",
            "--destination",
            str(destination),
            "--data-mode",
            "depth_trades",
            "--price-scale",
            "100",
            "--quantity-scale",
            "1000",
        ],
        network=network,
        now=clock,
        sleep=clock.sleep,
    )

    assert result == 1
    assert "fixture network failure" in capsys.readouterr().err
    assert destination.read_text(encoding="utf-8") == "existing\n"
    assert list(tmp_path.glob(".capture.csv.*.tmp")) == []


def test_cli_supports_utc_date_and_explicit_range(tmp_path: Path) -> None:
    by_date = downloader.parse_args(
        [
            "--date",
            "2024-01-01",
            "--destination",
            str(tmp_path / "date.csv"),
            "--data-mode",
            "depth",
        ]
    )
    assert by_date.start == START
    assert by_date.end == START + timedelta(days=1)
    assert by_date.data_mode == "depth"

    by_range = downloader.parse_args(
        [
            "--symbol",
            "ethusdt",
            "--start",
            "2024-01-01T00:00:00+00:00",
            "--end",
            "2024-01-01T00:00:01Z",
            "--destination",
            str(tmp_path / "range.csv"),
            "--data-mode",
            "depth_trades",
        ]
    )
    assert by_range.symbol == "ETHUSDT"
    assert by_range.end - by_range.start == timedelta(seconds=1)


def test_bookticker_is_not_an_available_data_mode(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as raised:
        downloader.parse_args(
            [
                "--date",
                "2024-01-01",
                "--destination",
                str(tmp_path / "bbo.csv"),
                "--data-mode",
                "bookTicker",
            ]
        )
    assert raised.value.code == 2
