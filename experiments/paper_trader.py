import asyncio
import json
import websockets
import numpy as np
import gymnasium as gym
import eigenbook
from eigenbook.env import LimitOrderBookEnv
from stable_baselines3 import PPO

class OrderBookFeaturesWrapper(gym.Wrapper):
    def __init__(self, env):
        super().__init__(env)
        self.observation_space = gym.spaces.Box(low=-np.inf, high=np.inf, shape=(4,), dtype=np.float32)
        
    def observation(self, obs):
        best_bid_p = obs[0][0][0]
        best_ask_p = obs[1][0][0]
        spread = 0.0 if best_bid_p == 0 else (best_ask_p - best_bid_p)
        top_volume = obs[0][0][1] + obs[1][0][1]
        return np.array([best_bid_p / 100000.0, best_ask_p / 100000.0, spread / 100.0, top_volume / 100.0], dtype=np.float32)
        
    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        return self.observation(obs), reward, terminated, truncated, info
        
    def reset(self, **kwargs):
        obs, info = self.env.reset(**kwargs)
        return self.observation(obs), info

async def run_paper_trader():
    book = eigenbook.BookConfig()
    book.min_price = 4000000
    book.max_price = 4500000
    book.max_orders = 10000
    book.order_id_map_capacity = 20000
    
    instrument = eigenbook.InstrumentConfig()
    instrument.instrument_id = 1
    instrument.book_config = book
    
    env = LimitOrderBookEnv(instrument, max_episode_steps=100000, max_abs_inventory=100)
    env = OrderBookFeaturesWrapper(env)
    
    model = PPO.load("ppo_eigenbook_agent.zip")
    obs, info = env.reset()
    
    TARGET_PRICE = 42500.00 
    price_offset = None
    
    _eb = getattr(eigenbook, "_eigenbook", eigenbook)
    SIDE_SELL = getattr(_eb.Side, "SELL", getattr(_eb.Side, "Sell", None))
    SIDE_BUY = getattr(_eb.Side, "BUY", getattr(_eb.Side, "Buy", None))
    TIF_IOC = getattr(_eb.TimeInForce, "IOC", getattr(_eb.TimeInForce, "Ioc", None))
    OP_ADD = getattr(_eb.CommandOp, "ADD_LIMIT", getattr(_eb.CommandOp, "AddLimit", None))
    
    uri = "wss://ws-feed.exchange.coinbase.com"
    async with websockets.connect(uri) as websocket:
        await websocket.send(json.dumps({"type": "subscribe", "product_ids": ["BTC-USD"], "channels": ["matches"]}))
        
        async for message in websocket:
            data = json.loads(message)
            if data.get("type") == "match":
                live_price = float(data["price"])
                size = float(data["size"])
                taker_side = "SELL" if data["side"] == "buy" else "BUY"
                
                if price_offset is None:
                    price_offset = live_price - TARGET_PRICE
                    print(f"\n[MLOps] Neural Network trained at ~${TARGET_PRICE:,.2f}. Live market is ~${live_price:,.2f}.")
                    print(f"[MLOps] Calibrating live flow: Shifting inputs by -${price_offset:,.2f}\n")
                
                shifted_price = live_price - price_offset
                price_int = int(shifted_price * 100)
                qty_int = max(1, int(size * 100000))
                side_enum = SIDE_SELL if taker_side == "SELL" else SIDE_BUY
                
                try:
                    env.unwrapped.engine.dispatch(
                        _eb.Command(
                            instrument_id=1, 
                            op=OP_ADD, 
                            order_id=999999, 
                            side=side_enum, 
                            price=price_int, 
                            quantity=qty_int, 
                            time_in_force=TIF_IOC
                        )
                    )
                except Exception as e:
                    pass 
                
                action, _ = model.predict(obs, deterministic=True)
                obs, reward, terminated, truncated, info = env.step(action)
                
                inventory = info.get("inventory", 0)
                print(f"LIVE TAPE | {taker_side:4} {size:8.5f} BTC @ ${live_price:8.2f} (Agent Sees: ${shifted_price:8.2f}) | PnL: {reward:8.2f} | Inv: {inventory:+}")
                
                if terminated or truncated:
                    print("Limit reached. Resetting...")
                    obs, info = env.reset()

if __name__ == "__main__":
    try:
        asyncio.run(run_paper_trader())
    except KeyboardInterrupt:
        print("\nDisconnected from live stream.")
