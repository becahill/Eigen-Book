import asyncio
import json
import websockets
import os

# Global state for the order book
bids = {}
asks = {}

async def render_ui():
    """Throttled UI renderer (2 FPS)"""
    while True:
        await asyncio.sleep(0.5)
        
        if not bids or not asks:
            continue
            
        os.system('cls' if os.name == 'nt' else 'clear')
        
        # Sort asks ascending, bids descending
        top_asks = sorted(asks.items())[:5]
        top_bids = sorted(bids.items(), reverse=True)[:5]
        
        best_ask = top_asks[0][0] if top_asks else 0
        best_bid = top_bids[0][0] if top_bids else 0
        spread = best_ask - best_bid
        
        print("\n=== COINBASE BTC-USD L2 BOOK ===")
        print(f"SPREAD: ${spread:.2f}\n")
        
        print("------- ASKS (SELLERS) -------")
        # Print asks in reverse so the best ask is at the bottom (closest to spread)
        for price, size in reversed(top_asks):
            print(f"  ${price:8.2f}  |  {size:8.5f} BTC")
            
        print("------------------------------")
        
        for price, size in top_bids:
            print(f"  ${price:8.2f}  |  {size:8.5f} BTC")
        print("------- BIDS (BUYERS) --------\n")

async def consume_feed():
    """High-speed network ingestor"""
    uri = "wss://ws-feed.exchange.coinbase.com"
    print(f"Connecting to {uri}...")
    
    async with websockets.connect(uri) as websocket:
        # Explicit subscription payload
        sub = {
            "type": "subscribe", 
            "product_ids": ["BTC-USD"], 
            "channels": ["level2"]
        }
        await websocket.send(json.dumps(sub))
        
        async for message in websocket:
            data = json.loads(message)
            msg_type = data.get("type")
            
            if msg_type == "snapshot":
                for ask in data.get("asks", []):
                    asks[float(ask[0])] = float(ask[1])
                for bid in data.get("bids", []):
                    bids[float(bid[0])] = float(bid[1])
                    
            elif msg_type == "l2update":
                for change in data.get("changes", []):
                    side, price_str, size_str = change
                    price, size = float(price_str), float(size_str)
                    
                    target_dict = asks if side == "sell" else bids
                    
                    if size == 0.0:
                        target_dict.pop(price, None)
                    else:
                        target_dict[price] = size

async def main():
    # Run the network ingestor and the UI renderer simultaneously
    await asyncio.gather(consume_feed(), render_ui())

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nDisconnected from L2 stream.")
