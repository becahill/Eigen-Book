from __future__ import annotations

import gc

import numpy as np
import pytest

import eigenbook as eb

from python_helpers import make_add_command, make_book_config, make_instrument


TRADE_EVENT_DTYPE = np.dtype(
    [
        ("instrument_id", np.uint32),
        ("aggressor_id", np.uint64),
        ("resting_id", np.uint64),
        ("aggressor_side", np.uint8),
        ("price", np.int64),
        ("quantity", np.uint64),
        ("timestamp", np.uint64),
        ("sequence", np.uint64),
    ],
    align=True,
)

BOOK_EVENT_DTYPE = np.dtype(
    [
        ("kind", np.uint8),
        ("instrument_id", np.uint32),
        ("status", np.uint8),
        ("order_id", np.uint64),
        ("side", np.uint8),
        ("price", np.int64),
        ("quantity", np.uint64),
        ("old_quantity", np.uint64),
        ("new_quantity", np.uint64),
        ("timestamp", np.uint64),
        ("sequence", np.uint64),
        ("time_in_force", np.uint8),
        ("trade", TRADE_EVENT_DTYPE),
    ],
    align=True,
)


def make_engine_and_buffer(
    *,
    max_orders: int = 8,
    event_log_capacity: int = 8,
) -> tuple[eb.MatchingEngine, np.ndarray]:
    book = make_book_config(
        max_orders=max_orders,
        event_log_capacity=event_log_capacity,
    )
    engine = eb.MatchingEngine([make_instrument(book_config=book)])
    buffer = np.empty(
        engine.event_buffer_capacity(101),
        dtype=eb.BOOK_EVENT_DTYPE,
    )
    return engine, buffer


def test_registered_dtypes_match_native_layout() -> None:
    assert TRADE_EVENT_DTYPE == eb.TRADE_EVENT_DTYPE
    assert BOOK_EVENT_DTYPE == eb.BOOK_EVENT_DTYPE
    assert eb.BOOK_EVENT_DTYPE.isnative
    events = np.empty(1, dtype=eb.BOOK_EVENT_DTYPE)
    trades = np.empty(1, dtype=eb.TRADE_EVENT_DTYPE)
    assert events.ctypes.data % eb.BOOK_EVENT_ALIGNMENT == 0
    assert trades.ctypes.data % eb.TRADE_EVENT_ALIGNMENT == 0


def test_partially_filled_event_buffer_is_a_caller_owned_copy() -> None:
    engine, event_buffer = make_engine_and_buffer()
    buffer_address = event_buffer.ctypes.data

    written = engine.dispatch_with_buffer(
        make_add_command(
            order_id=1,
            side=eb.Side.SELL,
            price=100,
            quantity=5,
            timestamp=10,
        ),
        event_buffer,
    )
    assert written == 2
    assert event_buffer.ctypes.data == buffer_address
    assert event_buffer[0]["kind"] == 1
    assert event_buffer[1]["kind"] == 2
    assert event_buffer[1]["order_id"] == 1
    assert event_buffer[1]["quantity"] == 5

    result = engine.dispatch_result_with_buffer(
        make_add_command(
            order_id=2,
            side=eb.Side.BUY,
            price=100,
            quantity=5,
            timestamp=11,
        ),
        event_buffer,
    )
    assert result.status == eb.Status.FILLED
    assert result.events_emitted == 2
    assert result.executed_quantity == 5
    assert event_buffer[1]["kind"] == 0
    assert event_buffer[1]["trade"]["instrument_id"] == 101
    assert event_buffer[1]["trade"]["aggressor_id"] == 2
    assert event_buffer[1]["trade"]["resting_id"] == 1
    assert event_buffer[1]["trade"]["price"] == 100
    assert event_buffer[1]["trade"]["quantity"] == 5
    assert engine.reward_from_events(event_buffer, 2, 5) == 5.0

    del engine
    gc.collect()

    assert event_buffer.ctypes.data == buffer_address
    assert event_buffer[1]["trade"]["quantity"] == 5


def test_venue_command_can_copy_events_to_buffer() -> None:
    engine, event_buffer = make_engine_and_buffer()
    command = eb.VenueCommand(
        make_add_command(
            order_id=1,
            side=eb.Side.BUY,
            price=100,
            quantity=5,
            timestamp=10,
        ),
        participant_id=7,
        post_only=True,
    )

    result = engine.dispatch_result_with_buffer(command, event_buffer)

    assert result.status == eb.Status.ACCEPTED
    assert result.events_emitted == 2
    assert event_buffer[0]["kind"] == 1
    assert event_buffer[1]["kind"] == 2
    assert event_buffer[1]["order_id"] == 1
    assert event_buffer[1]["quantity"] == 5


def test_empty_event_result_does_not_modify_buffer() -> None:
    engine, event_buffer = make_engine_and_buffer()
    event_buffer.view(np.uint8).fill(0xA5)
    original = event_buffer.view(np.uint8).copy()

    result = engine.dispatch_result_with_buffer(
        make_add_command(
            instrument_id=999,
            order_id=1,
            side=eb.Side.BUY,
            price=100,
            quantity=1,
        ),
        event_buffer,
    )
    assert result.status == eb.Status.UNKNOWN_INSTRUMENT
    assert result.events_emitted == 0
    np.testing.assert_array_equal(event_buffer.view(np.uint8), original)


def test_full_event_buffer() -> None:
    engine, event_buffer = make_engine_and_buffer(
        max_orders=3,
        event_log_capacity=4,
    )
    assert (
        engine.dispatch_with_buffer(
            make_add_command(
                order_id=1,
                side=eb.Side.SELL,
                price=99,
                quantity=1,
            ),
            event_buffer,
        )
        == 2
    )
    assert (
        engine.dispatch_with_buffer(
            make_add_command(
                order_id=2,
                side=eb.Side.SELL,
                price=100,
                quantity=1,
            ),
            event_buffer,
        )
        == 2
    )

    result = engine.dispatch_result_with_buffer(
        make_add_command(
            order_id=3,
            side=eb.Side.BUY,
            price=101,
            quantity=3,
        ),
        event_buffer,
    )
    assert result.status == eb.Status.PARTIALLY_FILLED
    assert result.events_emitted == event_buffer.shape[0] == 4
    np.testing.assert_array_equal(
        event_buffer["kind"],
        np.array([1, 0, 0, 2], dtype=np.uint8),
    )
    np.testing.assert_array_equal(
        event_buffer[1:3]["trade"]["quantity"],
        np.array([1, 1], dtype=np.uint64),
    )


def test_depth_writes_float32_c_contiguous_buffer() -> None:
    engine, event_buffer = make_engine_and_buffer()
    for order_id, price, quantity in ((1, 98, 2), (2, 99, 3), (3, 99, 4)):
        assert (
            engine.dispatch_with_buffer(
                make_add_command(
                    order_id=order_id,
                    side=eb.Side.BUY,
                    price=price,
                    quantity=quantity,
                ),
                event_buffer,
            )
            == 2
        )

    depth_buffer = np.full((5, 2), -1.0, dtype=np.float32)
    depth_address = depth_buffer.ctypes.data
    engine.depth(101, eb.Side.BUY, depth_buffer)
    assert depth_buffer.ctypes.data == depth_address
    np.testing.assert_array_equal(
        depth_buffer,
        np.array(
            [
                [99.0, 7.0],
                [98.0, 2.0],
                [0.0, 0.0],
                [0.0, 0.0],
                [0.0, 0.0],
            ],
            dtype=np.float32,
        ),
    )

    ask_depth = np.full((5, 2), -1.0, dtype=np.float32)
    engine.depth(101, eb.Side.SELL, ask_depth)
    np.testing.assert_array_equal(ask_depth, np.zeros((5, 2), dtype=np.float32))


def test_invalid_event_buffers_reject_before_dispatch() -> None:
    engine, event_buffer = make_engine_and_buffer()
    command = make_add_command(
        order_id=1,
        side=eb.Side.BUY,
        price=99,
        quantity=1,
    )

    with pytest.raises(ValueError, match="smaller"):
        engine.dispatch_with_buffer(command, event_buffer[:-1].copy())
    with pytest.raises(TypeError):
        engine.dispatch_with_buffer(
            command,
            np.empty(event_buffer.shape[0], dtype=np.uint8),
        )
    with pytest.raises(ValueError, match="one-dimensional"):
        engine.dispatch_with_buffer(
            command,
            np.empty((2, event_buffer.shape[0] // 2), dtype=BOOK_EVENT_DTYPE),
        )

    read_only = event_buffer.copy()
    read_only.flags.writeable = False
    with pytest.raises(ValueError, match="writable"):
        engine.dispatch_with_buffer(command, read_only)

    strided = np.empty(event_buffer.shape[0] * 2, dtype=BOOK_EVENT_DTYPE)[::2]
    with pytest.raises(TypeError):
        engine.dispatch_with_buffer(command, strided)

    raw = np.empty(
        event_buffer.shape[0] * BOOK_EVENT_DTYPE.itemsize + 1,
        dtype=np.uint8,
    )
    unaligned = np.ndarray(
        (event_buffer.shape[0],),
        dtype=BOOK_EVENT_DTYPE,
        buffer=raw,
        offset=1,
    )
    with pytest.raises(ValueError, match="aligned"):
        engine.dispatch_with_buffer(command, unaligned)

    assert not engine.top_of_book(101).bid.valid


def test_invalid_depth_and_reward_buffers() -> None:
    engine, event_buffer = make_engine_and_buffer()

    with pytest.raises(TypeError):
        engine.depth(101, eb.Side.BUY, np.empty((5, 2), dtype=np.float64))
    with pytest.raises(ValueError, match=r"shape \(levels, 2\)"):
        engine.depth(101, eb.Side.BUY, np.empty(10, dtype=np.float32))
    with pytest.raises(ValueError, match="at most 64"):
        engine.depth(101, eb.Side.BUY, np.empty((65, 2), dtype=np.float32))

    read_only_depth = np.empty((5, 2), dtype=np.float32)
    read_only_depth.flags.writeable = False
    with pytest.raises(ValueError, match="writable"):
        engine.depth(101, eb.Side.BUY, read_only_depth)

    with pytest.raises(ValueError, match="event_count"):
        engine.reward_from_events(event_buffer, event_buffer.shape[0] + 1, 1)
