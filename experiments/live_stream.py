import asyncio
import json
import websockets

async def stream_coinbase_trades() -> None:
    uri = "wss://ws-feed.exchange.coinbase.com"
    print(f"Connecting to Coinbase Public WebSocket: {uri}...")
    
    async with websockets.connect(uri) as websocket:
        print("Connected! Subscribing to BTC-USD matches...\n")
        
        # Coinbase requires an explicit subscription payload
        subscribe_message = {
            "type": "subscribe",
            "product_ids": ["BTC-USD"],
            "channels": ["matches"]
        }
        await websocket.send(json.dumps(subscribe_message))
        
        async for message in websocket:
            data = json.loads(message)
            
            # Filter out the initial subscription confirmation messages
            if data.get("type") == "match":
                price = float(data["price"])
                size = float(data["size"])
                # Coinbase explicitly tells us the maker side. 
                # If the maker was a "buy", the aggressive taker was a "sell".
                taker_side = "SELL" if data["side"] == "buy" else "BUY"
                
                print(f"LIVE TRADE | Side: {taker_side:4} | Price: ${price:8.2f} | Qty: {size:8.5f} BTC")

if __name__ == "__main__":
    try:
        asyncio.run(stream_coinbase_trades())
    except KeyboardInterrupt:
        print("\nDisconnected from live stream.")
