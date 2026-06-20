import eigenbook_py as eb


book_config = eb.BookConfig()
book_config.min_price = 90
book_config.max_price = 110
book_config.max_orders = 16
book_config.order_id_map_capacity = 32
book_config.tick_size = 1
book_config.event_log_capacity = 32
book_config.price_level_mode = eb.PriceLevelMode.DENSE

instrument = eb.InstrumentConfig()
instrument.instrument_id = 101
instrument.book_config = book_config
instrument.tick_size = 1
instrument.lot_size = 1

engine = eb.MatchingEngine([instrument])

command = eb.Command()
command.instrument_id = 101
command.op = eb.CommandOp.ADD
command.order_id = 1
command.side = eb.Side.BUY
command.price = 100
command.quantity = 10
command.time_in_force = eb.TimeInForce.GTC
command.timestamp = 1

result = engine.dispatch(command)
assert result.status == eb.Status.ACCEPTED
assert result.accepted_quantity == 10
assert result.resting_quantity == 10

top = engine.top_of_book(101)
assert top.status == eb.Status.ACCEPTED
assert top.bid.valid
assert top.bid.price == 100
assert top.bid.quantity == 10
assert top.bid.order_count == 1
assert not top.ask.valid

unknown = engine.top_of_book(202)
assert unknown.status == eb.Status.UNKNOWN_INSTRUMENT
