import numpy as np

import eigenbook_py as eb


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

assert TRADE_EVENT_DTYPE == eb.TRADE_EVENT_DTYPE
assert BOOK_EVENT_DTYPE == eb.BOOK_EVENT_DTYPE

book_config = eb.BookConfig()
book_config.min_price = 90
book_config.max_price = 110
book_config.max_orders = 8
book_config.order_id_map_capacity = 16
book_config.tick_size = 1
book_config.event_log_capacity = 8
book_config.price_level_mode = eb.PriceLevelMode.DENSE

instrument = eb.InstrumentConfig()
instrument.instrument_id = 101
instrument.book_config = book_config
instrument.tick_size = 1
instrument.lot_size = 1

engine = eb.MatchingEngine([instrument])
event_buffer = np.empty(book_config.event_log_capacity, dtype=BOOK_EVENT_DTYPE)
buffer_address = event_buffer.ctypes.data

resting_sell = eb.Command()
resting_sell.instrument_id = 101
resting_sell.op = eb.CommandOp.ADD
resting_sell.order_id = 1
resting_sell.side = eb.Side.SELL
resting_sell.price = 100
resting_sell.quantity = 5
resting_sell.time_in_force = eb.TimeInForce.GTC
resting_sell.timestamp = 10

written = engine.dispatch_with_buffer(resting_sell, event_buffer)
assert written == 2
assert event_buffer.ctypes.data == buffer_address
assert event_buffer[0]["kind"] == 1  # BookEventKind.ORDER_ACCEPTED
assert event_buffer[1]["kind"] == 2  # BookEventKind.ORDER_RESTING
assert event_buffer[1]["order_id"] == 1
assert event_buffer[1]["quantity"] == 5

crossing_buy = eb.Command()
crossing_buy.instrument_id = 101
crossing_buy.op = eb.CommandOp.ADD
crossing_buy.order_id = 2
crossing_buy.side = eb.Side.BUY
crossing_buy.price = 100
crossing_buy.quantity = 5
crossing_buy.time_in_force = eb.TimeInForce.GTC
crossing_buy.timestamp = 11

written = engine.dispatch_with_buffer(crossing_buy, event_buffer)
assert written == 2
assert event_buffer.ctypes.data == buffer_address
assert event_buffer[0]["kind"] == 1  # BookEventKind.ORDER_ACCEPTED
assert event_buffer[1]["kind"] == 0  # BookEventKind.TRADE
assert event_buffer[1]["trade"]["instrument_id"] == 101
assert event_buffer[1]["trade"]["aggressor_id"] == 2
assert event_buffer[1]["trade"]["resting_id"] == 1
assert event_buffer[1]["trade"]["price"] == 100
assert event_buffer[1]["trade"]["quantity"] == 5

undersized = np.empty(book_config.event_log_capacity - 1, dtype=BOOK_EVENT_DTYPE)
unsubmitted = eb.Command()
unsubmitted.instrument_id = 101
unsubmitted.op = eb.CommandOp.ADD
unsubmitted.order_id = 3
unsubmitted.side = eb.Side.BUY
unsubmitted.price = 99
unsubmitted.quantity = 1
unsubmitted.time_in_force = eb.TimeInForce.GTC

try:
    engine.dispatch_with_buffer(unsubmitted, undersized)
except ValueError:
    pass
else:
    raise AssertionError("undersized event buffer must be rejected before dispatch")

assert not engine.top_of_book(101).bid.valid

try:
    engine.dispatch_with_buffer(unsubmitted, np.empty(8, dtype=np.uint8))
except TypeError:
    pass
else:
    raise AssertionError("incorrect event dtype must not be converted")

try:
    engine.dispatch_with_buffer(
        unsubmitted,
        np.empty((2, book_config.event_log_capacity // 2), dtype=BOOK_EVENT_DTYPE),
    )
except ValueError:
    pass
else:
    raise AssertionError("multidimensional event buffers must be rejected")

read_only = np.empty(book_config.event_log_capacity, dtype=BOOK_EVENT_DTYPE)
read_only.flags.writeable = False
try:
    engine.dispatch_with_buffer(unsubmitted, read_only)
except ValueError:
    pass
else:
    raise AssertionError("read-only event buffers must be rejected")

assert not engine.top_of_book(101).bid.valid
