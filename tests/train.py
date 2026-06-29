"""Train and evaluate a PPO agent on the Eigen-Book Gymnasium environment."""

import random
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


class MarketMakerRewardWrapper(gym.RewardWrapper):
    """Invert the native reward to favor passive market-making behavior."""

    def reward(self, reward):
        return -1.0 * reward


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

        return np.array(
            [best_bid_p, best_ask_p, spread, top_volume],
            dtype=np.float32,
        )


class NoiseTraderWrapper(gym.Wrapper):
    """Inject random aggressive IOC orders after agent actions."""

    def __init__(self, env, noise_prob=0.15):
        super().__init__(env)
        self.noise_prob = noise_prob
        self._next_noise_order_id = 1_000_000_000
        self._next_noise_timestamp = 1_000_000_000
        self._noise_command = eigenbook.Command()
        self._noise_command.instrument_id = self.env.unwrapped.instrument_id
        self._noise_command.op = eigenbook.CommandOp.ADD
        self._noise_command.time_in_force = eigenbook.TimeInForce.IOC

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)

        if random.random() < self.noise_prob and not (terminated or truncated):
            native_env = self.env.unwrapped
            side = random.choice((eigenbook.Side.BUY, eigenbook.Side.SELL))
            quantity = random.randint(1, 5)

            self._noise_command.order_id = self._next_noise_order_id
            self._noise_command.side = side
            self._noise_command.price = (
                native_env._max_price
                if side == eigenbook.Side.BUY
                else native_env._min_price
            )
            self._noise_command.quantity = quantity
            self._noise_command.timestamp = self._next_noise_timestamp

            native_env.engine.dispatch_with_buffer(
                self._noise_command,
                native_env.event_buffer,
            )
            self._next_noise_order_id += 1
            self._next_noise_timestamp += 1
            obs = native_env._get_obs()

        return obs, reward, terminated, truncated, info


def main() -> None:
    book = eigenbook.BookConfig()
    book.min_price = 90
    book.max_price = 110
    book.max_orders = 64
    book.order_id_map_capacity = 128
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
    env = MarketMakerRewardWrapper(env)
    env = NoiseTraderWrapper(env)
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
