from __future__ import annotations

import eigenbook as eb


def make_book_config(
    *,
    min_price: int = 90,
    max_price: int = 110,
    max_orders: int = 16,
    event_log_capacity: int = 0,
    lot_size: int = 0,
    self_trade_policy: eb.SelfTradePolicy = eb.SelfTradePolicy.DISABLED,
    market_data_capacity: int = 0,
) -> eb.BookConfig:
    config = eb.BookConfig()
    config.min_price = min_price
    config.max_price = max_price
    config.max_orders = max_orders
    config.order_id_map_capacity = max_orders * 2
    config.tick_size = 1
    config.event_log_capacity = event_log_capacity
    config.price_level_mode = eb.PriceLevelMode.DENSE
    config.lot_size = lot_size
    config.self_trade_policy = self_trade_policy
    config.market_data_capacity = market_data_capacity
    return config


def make_instrument(
    *,
    instrument_id: int = 101,
    book_config: eb.BookConfig | None = None,
    lot_size: int = 1,
    self_trade_policy: eb.SelfTradePolicy = eb.SelfTradePolicy.DISABLED,
    market_data_capacity: int = 0,
) -> eb.InstrumentConfig:
    config = eb.InstrumentConfig()
    config.instrument_id = instrument_id
    config.book_config = (
        book_config if book_config is not None else make_book_config()
    )
    config.tick_size = 1
    config.lot_size = lot_size
    config.self_trade_policy = self_trade_policy
    config.market_data_capacity = market_data_capacity
    return config


def make_add_command(
    *,
    instrument_id: int = 101,
    order_id: int,
    side: eb.Side,
    price: int,
    quantity: int,
    timestamp: int = 1,
) -> eb.Command:
    command = eb.Command()
    command.instrument_id = instrument_id
    command.op = eb.CommandOp.ADD
    command.order_id = order_id
    command.side = side
    command.price = price
    command.quantity = quantity
    command.time_in_force = eb.TimeInForce.GTC
    command.timestamp = timestamp
    return command
