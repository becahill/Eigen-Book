from __future__ import annotations

import gc

import pytest

import eigenbook as eb

from python_helpers import make_add_command, make_book_config, make_instrument


def test_matching_engine_binding_behavior() -> None:
    engine = eb.MatchingEngine([make_instrument()])
    command = make_add_command(
        order_id=1,
        side=eb.Side.BUY,
        price=100,
        quantity=10,
    )

    result = engine.dispatch(command)
    assert result.status == eb.Status.ACCEPTED
    assert result.accepted_quantity == 10
    assert result.executed_quantity == 0
    assert result.resting_quantity == 10
    assert result.events_emitted == 2

    top = engine.top_of_book(101)
    assert top.status == eb.Status.ACCEPTED
    assert top.bid.valid
    assert top.bid.price == 100
    assert top.bid.quantity == 10
    assert top.bid.order_count == 1
    assert not top.ask.valid

    unknown = engine.top_of_book(202)
    assert unknown.status == eb.Status.UNKNOWN_INSTRUMENT


def test_configuration_errors_are_pythonic() -> None:
    instrument = make_instrument()
    duplicate = make_instrument()
    with pytest.raises(ValueError, match=r"index 1.*duplicate instrument id"):
        eb.MatchingEngine([instrument, duplicate])

    invalid_book = make_book_config(min_price=110, max_price=90)
    invalid_instrument = make_instrument(
        instrument_id=202,
        book_config=invalid_book,
    )
    with pytest.raises(
        ValueError,
        match=r"index 1.*invalid order-book configuration",
    ):
        eb.MatchingEngine([instrument, invalid_instrument])

    with pytest.raises(TypeError):
        eb.MatchingEngine([object()])


def test_engine_owns_copied_configuration() -> None:
    book = make_book_config()
    instrument = make_instrument(book_config=book)
    engine = eb.MatchingEngine([instrument])

    book.min_price = 1_000
    instrument.instrument_id = 999

    result = engine.dispatch(
        make_add_command(
            order_id=1,
            side=eb.Side.BUY,
            price=100,
            quantity=1,
        )
    )
    assert result.status == eb.Status.ACCEPTED
    assert engine.top_of_book(999).status == eb.Status.UNKNOWN_INSTRUMENT


def test_dispatch_result_is_detached_from_engine_storage() -> None:
    engine = eb.MatchingEngine([make_instrument()])
    result = engine.dispatch(
        make_add_command(
            order_id=1,
            side=eb.Side.BUY,
            price=100,
            quantity=7,
        )
    )

    del engine
    gc.collect()

    assert result.status == eb.Status.ACCEPTED
    assert result.accepted_quantity == 7
    assert result.resting_quantity == 7
    assert result.events_emitted == 2


def test_native_rejections_are_exposed_as_status_values() -> None:
    engine = eb.MatchingEngine([make_instrument()])
    invalid_price = engine.dispatch(
        make_add_command(
            order_id=1,
            side=eb.Side.BUY,
            price=1_000,
            quantity=1,
        )
    )
    assert invalid_price.status == eb.Status.INVALID_PRICE
    assert invalid_price.events_emitted == 1

    unknown_instrument = engine.dispatch(
        make_add_command(
            instrument_id=999,
            order_id=2,
            side=eb.Side.SELL,
            price=100,
            quantity=1,
        )
    )
    assert unknown_instrument.status == eb.Status.UNKNOWN_INSTRUMENT
    assert unknown_instrument.events_emitted == 0

    with pytest.raises(KeyError, match="999"):
        engine.event_buffer_capacity(999)


def test_default_event_capacity_is_reported_by_native_engine() -> None:
    book = make_book_config(max_orders=16, event_log_capacity=0)
    engine = eb.MatchingEngine([make_instrument(book_config=book)])
    assert engine.event_buffer_capacity(101) == 18
