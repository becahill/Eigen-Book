"""Replay Binance aggregate trades through the Eigen-Book Gymnasium environment."""

from __future__ import annotations

import csv

import gymnasium as gym
import numpy as np

import eigenbook


class BinanceDataWrapper(gym.Wrapper):
    """Inject five historical Binance trades after every agent action."""

    def __init__(self, env: gym.Env, csv_path: str) -> None:
        super().__init__(env)
        self._csv_file = open(csv_path, newline="", encoding="utf-8")
        self._reader = csv.reader(self._csv_file)

        native_env = self.env.unwrapped
        self._market_command = eigenbook.Command()
        self._market_command.instrument_id = native_env.instrument_id
        self._market_command.op = eigenbook.CommandOp.MARKET
        self._market_command.time_in_force = eigenbook.TimeInForce.IOC
        self._next_replay_order_id = 1 << 63
        self._next_replay_timestamp = 1 << 63

    def reset(self, **kwargs):
        self._csv_file.seek(0)
        self._reader = csv.reader(self._csv_file)
        self._next_replay_order_id = 1 << 63
        self._next_replay_timestamp = 1 << 63
        return super().reset(**kwargs)

    def step(self, action: np.ndarray):
        obs, reward, terminated, truncated, info = self.env.step(action)
        native_env = self.env.unwrapped

        for _ in range(5):
            row = next(self._reader)
            price = int(float(row[1]) * 100)
            quantity = max(1, int(float(row[2]) * 100000))
            is_buyer_maker = row[6]
            side = (
                eigenbook.Side.SELL
                if is_buyer_maker == "True"
                else eigenbook.Side.BUY
            )

            self._market_command.order_id = self._next_replay_order_id
            self._market_command.side = side
            self._market_command.price = price
            self._market_command.quantity = quantity
            self._market_command.timestamp = self._next_replay_timestamp

            native_env.engine.dispatch_with_buffer(
                self._market_command,
                native_env.event_buffer,
            )
            self._next_replay_order_id += 1
            self._next_replay_timestamp += 1

        obs = native_env._get_obs()
        return obs, reward, terminated, truncated, info

    def close(self) -> None:
        self._csv_file.close()
        super().close()


def main() -> None:
    book = eigenbook.BookConfig()
    book.min_price = 4_000_000
    book.max_price = 4_500_000
    book.max_orders = 10_000
    book.order_id_map_capacity = 20_000

    instrument = eigenbook.InstrumentConfig()
    instrument.instrument_id = 101
    instrument.book_config = book

    env = eigenbook.LimitOrderBookEnv(instrument)
    env = BinanceDataWrapper(env, "BTCUSDT-aggTrades-2024-01-01.csv")

    try:
        env.reset()
        for step in range(1, 1_001):
            obs, _, _, _, _ = env.step(env.action_space.sample())
            if step % 100 == 0:
                print(f"Step {step}:")
                print(obs)
    finally:
        env.close()


if __name__ == "__main__":
    main()
