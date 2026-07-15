"""Display-only, unsequenced Coinbase depth viewer.

This legacy UI neither writes canonical tapes nor drives features, fills,
training, evaluation, or paper replay. The Coinbase protocol names used below
are wire literals. Because this channel does not supply the update-ID contract
required by Eigen-Book's policy path, a disconnect requires restarting the
viewer from a new snapshot. Use ``fetch_l2_data.py`` for canonical capture.
"""

from __future__ import annotations

import asyncio
from decimal import Decimal, InvalidOperation
import json
import os
from typing import Any

import websockets


bids: dict[Decimal, Decimal] = {}
asks: dict[Decimal, Decimal] = {}


def _decimal(value: Any, field: str) -> Decimal:
    if type(value) is not str:
        raise ValueError(f"{field} must be a decimal string")
    try:
        parsed = Decimal(value)
    except InvalidOperation as error:
        raise ValueError(f"{field} is not a decimal: {value!r}") from error
    if not parsed.is_finite() or parsed < 0:
        raise ValueError(f"{field} must be finite and non-negative")
    return parsed


async def render_ui() -> None:
    """Render the five nearest displayed levels twice per second."""

    while True:
        await asyncio.sleep(0.5)
        if not bids or not asks:
            continue

        os.system("cls" if os.name == "nt" else "clear")
        top_asks = sorted(asks.items())[:5]
        top_bids = sorted(bids.items(), reverse=True)[:5]
        best_ask = top_asks[0][0]
        best_bid = top_bids[0][0]

        print("\n=== UNSEQUENCED COINBASE DEPTH DISPLAY ===")
        print(f"SPREAD: ${best_ask - best_bid:.2f}\n")
        print("------- ASKS (SELLERS) -------")
        for price, size in reversed(top_asks):
            print(f"  ${price:8.2f}  |  {size:8.5f} BTC")
        print("------------------------------")
        for price, size in top_bids:
            print(f"  ${price:8.2f}  |  {size:8.5f} BTC")
        print("------- BIDS (BUYERS) --------\n")


async def consume_feed() -> None:
    """Consume the display channel; any disconnect requires a fresh snapshot."""

    uri = "wss://ws-feed.exchange.coinbase.com"
    print(f"Connecting display viewer to {uri}...")
    async with websockets.connect(uri) as websocket:
        subscription = {
            "type": "subscribe",
            "product_ids": ["BTC-USD"],
            "channels": ["level2"],
        }
        await websocket.send(json.dumps(subscription))

        async for message in websocket:
            data = json.loads(message)
            message_type = data.get("type")
            if message_type == "snapshot":
                replacement_asks = {
                    _decimal(level[0], "snapshot ask price"): _decimal(
                        level[1], "snapshot ask size"
                    )
                    for level in data.get("asks", [])
                }
                replacement_bids = {
                    _decimal(level[0], "snapshot bid price"): _decimal(
                        level[1], "snapshot bid size"
                    )
                    for level in data.get("bids", [])
                }
                if not replacement_bids or not replacement_asks:
                    raise ValueError("display snapshot must contain both sides")
                bids.clear()
                bids.update(replacement_bids)
                asks.clear()
                asks.update(replacement_asks)
            elif message_type == "l2update":
                for change in data.get("changes", []):
                    side, price_text, size_text = change
                    price = _decimal(price_text, "update price")
                    size = _decimal(size_text, "update size")
                    levels = asks if side == "sell" else bids
                    if size == 0:
                        levels.pop(price, None)
                    else:
                        levels[price] = size


async def main() -> None:
    await asyncio.gather(consume_feed(), render_ui())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nDisconnected from depth display.")
