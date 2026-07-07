"""Local historical-market-data PPO experiment.

This script is intentionally outside tests because it depends on a local
Binance aggregate-trade CSV and writes TensorBoard/model artifacts. The
maintained, deterministic PPO demonstration and its tests live in
``scripts/train_ppo.py`` and ``tests/test_train_ppo.py``.
"""

import csv
from pathlib import Path

import gymnasium as gym
import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import BaseCallback
from stable_baselines3.common.env_checker import check_env

import eigenbook
from eigenbook.env import LimitOrderBookEnv


TRAINING_TIMESTEPS = 100_000
EVALUATION_STEPS = 1_000
MODEL_PATH = "ppo_eigenbook_agent"
MARKET_DATA_PATH = Path("BTCUSDT-aggTrades-2024-01-01.csv")
TENSORBOARD_LOG_DIR = "tb_logs"


class TrainingProgressCallback(BaseCallback):
    """Print training progress at fixed timestep intervals."""

    def __init__(self, report_interval: int = 10_000) -> None:
        super().__init__()
        self.report_interval = report_interval
        self.next_report = report_interval

    def _on_step(self) -> bool:
        if self.num_timesteps >= self.next_report:
            percent_complete = 100.0 * self.num_timesteps / TRAINING_TIMESTEPS
            print(
                f"[training] {self.num_timesteps:,}/{TRAINING_TIMESTEPS:,} "
                f"timesteps complete ({percent_complete:.1f}%)",
                flush=True,
            )
            self.next_report += self.report_interval
        return True


class MarketMakerRewardWrapper(gym.Wrapper):
    """Shape PnL with inventory exposure and terminal liquidation penalties."""

    def step(self, action):
        obs, base_pnl, terminated, truncated, info = self.env.step(action)

        inventory = info.get("inventory", 0)
        step_penalty = 0.01 * (inventory ** 2)
        shaped_reward = base_pnl - step_penalty

        if (terminated or truncated) and abs(inventory) >= 100:
            shaped_reward -= 5000.0

        return obs, shaped_reward, terminated, truncated, info


class OrderBookFeaturesWrapper(gym.ObservationWrapper):
    """Extract top-of-book price, spread, and volume features."""

    def __init__(self, env):
        super().__init__(env)
        self.observation_space = gym.spaces.Box(
            low=-np.inf,
            high=np.inf,
            shape=(4,),
            dtype=np.float32,
        )

    def observation(self, obs):
        best_bid_p = obs[0, 0, 0]
        best_bid_q = obs[0, 0, 1]
        best_ask_p = obs[1, 0, 0]
        best_ask_q = obs[1, 0, 1]

        spread = best_ask_p - best_bid_p
        top_volume = best_bid_q + best_ask_q

        return np.array([best_bid_p / 100000.0, best_ask_p / 100000.0, spread / 100.0, top_volume / 100.0], dtype=np.float32)


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
    book.min_price = 4000000
    book.max_price = 4500000
    book.max_orders = 10000
    book.order_id_map_capacity = 20000
    book.tick_size = 1

    instrument = eigenbook.InstrumentConfig()
    instrument.instrument_id = 101
    instrument.book_config = book

    print("[setup] Creating LimitOrderBookEnv with native configuration...", flush=True)
    env = LimitOrderBookEnv(
        instrument,
        max_episode_steps=1000,
        max_abs_inventory=100,
    )
    env = BinanceDataWrapper(env, str(MARKET_DATA_PATH))
    env = MarketMakerRewardWrapper(env)
    env = OrderBookFeaturesWrapper(env)

    try:
        print("[validation] Running Stable Baselines3 check_env()...", flush=True)
        check_env(env, warn=True)
        print("[validation] Environment passed Gymnasium API validation.", flush=True)

        print("[training] Initializing PPO agent...", flush=True)
        model = PPO(
            "MlpPolicy",
            env,
            learning_rate=0.0003,
            batch_size=64,
            n_steps=2_000,
            verbose=1,
            tensorboard_log=TENSORBOARD_LOG_DIR,
        )

        print(
            f"[training] Starting training for exactly "
            f"{TRAINING_TIMESTEPS:,} timesteps...",
            flush=True,
        )
        model.learn(
            total_timesteps=TRAINING_TIMESTEPS,
            callback=TrainingProgressCallback(),
        )
        print("[training] Training complete.", flush=True)

        print(f"[persistence] Saving model as {MODEL_PATH!r}...", flush=True)
        model.save(MODEL_PATH)
        saved_path = Path(f"{MODEL_PATH}.zip").resolve()
        print(f"[persistence] Model saved to {saved_path}.", flush=True)

        print(
            f"[evaluation] Starting deterministic evaluation for up to "
            f"{EVALUATION_STEPS:,} steps...",
            flush=True,
        )
        obs, _ = env.reset()
        total_reward = 0.0
        survived_steps = 0
        episode_ended = False

        for step in range(1, EVALUATION_STEPS + 1):
            action, _ = model.predict(obs, deterministic=True)
            obs, reward, terminated, truncated, _ = env.step(action)
            total_reward += float(reward)
            survived_steps = step

            if step % 100 == 0:
                print(
                    f"[evaluation] Step {step:,}: "
                    f"cumulative reward={total_reward:.6f}",
                    flush=True,
                )

            if terminated or truncated:
                episode_ended = True
                reason = "terminated" if terminated else "truncated"
                print(
                    f"[evaluation] Episode {reason} after "
                    f"{survived_steps:,} steps.",
                    flush=True,
                )
                break

        if not episode_ended:
            print(
                f"[evaluation] Agent survived the full "
                f"{EVALUATION_STEPS:,}-step horizon.",
                flush=True,
            )

        print("[results] Deterministic evaluation complete.", flush=True)
        print(f"[results] Total reward: {total_reward:.6f}", flush=True)
        print(f"[results] Steps survived: {survived_steps:,}", flush=True)
    finally:
        env.close()
        print("[cleanup] Environment closed.", flush=True)


if __name__ == "__main__":
    main()
