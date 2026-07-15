#!/usr/bin/env python3
"""Train and evaluate PPO on the canonical sequenced depth-and-trade replay."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from dataclasses import dataclass, replace
import math
from pathlib import Path
import random
from typing import Any, Final

import gymnasium as gym
import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import BaseCallback
from stable_baselines3.common.env_checker import check_env
from stable_baselines3.common.utils import set_random_seed
import torch

from eigenbook.market_data import (
    DATA_MODES,
    SCHEMA_VERSION as MARKET_DATA_SCHEMA_VERSION,
    DepthBook,
    MarketDataError,
    validate_tape,
)
from eigenbook.model_artifact import (
    ModelMetadata,
    build_model_metadata,
    load_model_artifact,
    model_archive_path,
    save_model_artifact,
)
from eigenbook.observation import CanonicalObservationWrapper
from eigenbook.replay import (
    EXECUTION_QUALITY,
    FILL_MODEL_VERSION,
    MarketMakerRewardWrapper,
    ReplayConfig,
    SequencedMarketEnv,
)


DEFAULT_TRAINING_TIMESTEPS: Final = 100_000
DEFAULT_EVALUATION_STEPS: Final = 1_000
DEFAULT_SEED: Final = 7
DEFAULT_PPO_ROLLOUT_STEPS: int = 2_048
DEFAULT_PPO_BATCH_SIZE: int = 64
DEFAULT_MARKET_DATA_PATH: Final = Path(
    "market-data/BTCUSDT-binance-spot-depth-trades.csv"
)
DEFAULT_MODEL_PATH: Final = Path("training-output/ppo_eigenbook_depth")
MAX_RANDOM_SEED: Final = (1 << 32) - 1


@dataclass(frozen=True, slots=True)
class ExperimentConfig:
    market_data_path: Path
    evaluation_market_data_path: Path
    training_timesteps: int
    evaluation_steps: int
    seed: int
    model_path: Path
    simulator: ReplayConfig


@dataclass(frozen=True, slots=True)
class EvaluationResult:
    total_reward: float
    steps: int
    terminated: bool
    truncated: bool
    final_info: dict[str, Any]


@dataclass(frozen=True, slots=True)
class TrainingResult:
    model_path: Path
    metadata_path: Path
    evaluation: EvaluationResult


class TrainingProgressCallback(BaseCallback):
    """Report ten deterministic progress points."""

    def __init__(self, total_timesteps: int) -> None:
        super().__init__(verbose=0)
        self._total = total_timesteps
        self._points = sorted(
            {max(1, math.ceil(total_timesteps * index / 10)) for index in range(1, 11)}
        )
        self._next = 0

    def _on_step(self) -> bool:
        while (
            self._next < len(self._points)
            and self.num_timesteps >= self._points[self._next]
        ):
            point = self._points[self._next]
            print(
                f"[training] {point:,}/{self._total:,} requested timesteps "
                f"({100.0 * point / self._total:.1f}%)",
                flush=True,
            )
            self._next += 1
        return True


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _non_negative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return parsed


def _seed(value: str) -> int:
    parsed = int(value)
    if parsed < 0 or parsed > MAX_RANDOM_SEED:
        raise argparse.ArgumentTypeError(
            f"must be between 0 and {MAX_RANDOM_SEED}, inclusive"
        )
    return parsed


def _maker_fee(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or not -1.0 < parsed < 1.0:
        raise argparse.ArgumentTypeError("must be finite and strictly between -1 and 1")
    return parsed


def _taker_fee(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or not 0.0 <= parsed < 1.0:
        raise argparse.ArgumentTypeError("must be finite and in [0, 1)")
    return parsed


def _non_negative_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0.0:
        raise argparse.ArgumentTypeError("must be finite and non-negative")
    return parsed


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Train PPO on a validated snapshot + sequenced depth-update + "
            "aggressive-trade tape. Legacy bookTicker CSVs are rejected."
        )
    )
    parser.add_argument("--market-data", type=Path, default=DEFAULT_MARKET_DATA_PATH)
    parser.add_argument("--evaluation-market-data", type=Path)
    parser.add_argument("--symbol", default="BTCUSDT")
    parser.add_argument("--venue", default="binance_spot")
    parser.add_argument(
        "--data-mode",
        choices=sorted(DATA_MODES),
        default="depth_trades",
    )
    parser.add_argument("--price-scale", type=_positive_int, default=100)
    parser.add_argument("--quantity-scale", type=_positive_int, default=100_000)
    parser.add_argument("--own-order-min-price", type=_non_negative_int, default=0)
    parser.add_argument(
        "--own-order-max-price",
        type=_non_negative_int,
        default=(1 << 32) - 2,
    )
    parser.add_argument("--tick-size", type=_positive_int, default=1)
    parser.add_argument("--lot-size", type=_positive_int, default=1)
    parser.add_argument(
        "--max-quote-distance-ticks", type=_non_negative_int, default=10
    )
    parser.add_argument("--max-order-quantity-lots", type=_positive_int, default=100)
    parser.add_argument("--max-abs-inventory-lots", type=_positive_int, default=10_000)
    parser.add_argument("--events-per-action", type=_positive_int, default=1)
    parser.add_argument(
        "--order-entry-latency-events", type=_non_negative_int, default=1
    )
    parser.add_argument("--maker-fee-rate", type=_maker_fee, default=0.0002)
    parser.add_argument("--taker-fee-rate", type=_taker_fee, default=0.0005)
    parser.add_argument("--feature-window-size", type=_positive_int, default=32)
    parser.add_argument("--feature-spread-scale-ticks", type=_positive_int, default=8)
    parser.add_argument(
        "--inventory-penalty-rate",
        type=_non_negative_float,
        default=0.01,
    )
    parser.add_argument(
        "--training-timesteps",
        type=_positive_int,
        default=DEFAULT_TRAINING_TIMESTEPS,
    )
    parser.add_argument(
        "--evaluation-steps", type=_positive_int, default=DEFAULT_EVALUATION_STEPS
    )
    parser.add_argument("--seed", type=_seed, default=DEFAULT_SEED)
    parser.add_argument("--model-path", type=Path, default=DEFAULT_MODEL_PATH)
    return parser


def parse_args(arguments: Sequence[str] | None = None) -> ExperimentConfig:
    parser = build_argument_parser()
    args = parser.parse_args(arguments)
    if not args.model_path.name:
        parser.error("--model-path must name a file")
    try:
        simulator = ReplayConfig(
            symbol=args.symbol,
            venue=args.venue,
            data_mode=args.data_mode,
            price_scale=args.price_scale,
            quantity_scale=args.quantity_scale,
            own_order_min_price=args.own_order_min_price,
            own_order_max_price=args.own_order_max_price,
            tick_size=args.tick_size,
            lot_size=args.lot_size,
            max_quote_distance_ticks=args.max_quote_distance_ticks,
            max_order_quantity_lots=args.max_order_quantity_lots,
            max_abs_inventory_lots=args.max_abs_inventory_lots,
            events_per_action=args.events_per_action,
            order_entry_latency_events=args.order_entry_latency_events,
            maker_fee_rate=args.maker_fee_rate,
            taker_fee_rate=args.taker_fee_rate,
            feature_window_size=args.feature_window_size,
            feature_spread_scale_ticks=args.feature_spread_scale_ticks,
            inventory_penalty_rate=args.inventory_penalty_rate,
        )
    except (TypeError, ValueError) as error:
        parser.error(str(error))
    return ExperimentConfig(
        market_data_path=args.market_data,
        evaluation_market_data_path=(args.evaluation_market_data or args.market_data),
        training_timesteps=args.training_timesteps,
        evaluation_steps=args.evaluation_steps,
        seed=args.seed,
        model_path=args.model_path,
        simulator=simulator,
    )


def create_environment(
    market_data_path: str | Path,
    *,
    config: ReplayConfig,
) -> gym.Env[np.ndarray, np.ndarray]:
    """Create the one wrapper stack used for training, reload, and evaluation."""

    base: gym.Env = SequencedMarketEnv(market_data_path, config=config)
    rewarded: gym.Env = MarketMakerRewardWrapper(
        base,
        inventory_penalty_rate=config.inventory_penalty_rate,
    )
    return CanonicalObservationWrapper(rewarded)


def validate_market_data_contract(
    market_data_path: str | Path,
    *,
    config: ReplayConfig,
) -> DepthBook:
    """Validate the entire tape and its configured identity before policy use."""

    book = validate_tape(market_data_path)
    observed = (
        book.data_mode,
        book.venue,
        book.symbol,
        book.price_scale,
        book.quantity_scale,
    )
    expected = (
        config.data_mode,
        config.venue,
        config.symbol,
        config.price_scale,
        config.quantity_scale,
    )
    if observed != expected:
        raise MarketDataError(
            f"{Path(market_data_path)}: validated tape identity {observed} "
            f"does not match configured identity {expected}"
        )
    return book


def model_metadata(env: gym.Env, config: ReplayConfig) -> ModelMetadata:
    """Build the strict sidecar for this exact policy environment."""

    return build_model_metadata(
        env,
        market_data_schema_version=MARKET_DATA_SCHEMA_VERSION,
        market_data_mode=config.data_mode,
        price_scale=config.price_scale,
        quantity_scale=config.quantity_scale,
        symbol=config.symbol,
        venue=config.venue,
        maker_fee_rate=config.maker_fee_rate,
        taker_fee_rate=config.taker_fee_rate,
        fill_model=FILL_MODEL_VERSION,
        feature_configuration=config.feature_configuration(),
    )


def _seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    torch.use_deterministic_algorithms(True)
    set_random_seed(seed, using_cuda=False)


def evaluate_model(
    model: PPO,
    env: gym.Env,
    *,
    steps: int,
    seed: int,
) -> EvaluationResult:
    observation, _ = env.reset(seed=seed)
    total_reward = 0.0
    terminated = truncated = False
    final_info: dict[str, Any] = {}
    completed = 0
    for completed in range(1, steps + 1):
        if not bool(env.unwrapped.feed_synchronized):
            raise RuntimeError("feed lost synchronization before inference")
        action, _ = model.predict(observation, deterministic=True)
        observation, reward, terminated, truncated, final_info = env.step(action)
        total_reward += float(reward)
        if terminated or truncated:
            break
    return EvaluationResult(
        total_reward,
        completed,
        terminated,
        truncated,
        final_info,
    )


def run_training(config: ExperimentConfig, *, verbosity: int = 1) -> TrainingResult:
    _seed_everything(config.seed)
    validate_market_data_contract(
        config.market_data_path,
        config=config.simulator,
    )
    evaluation_is_distinct = (
        config.evaluation_market_data_path.resolve()
        != config.market_data_path.resolve()
    )
    if evaluation_is_distinct:
        validate_market_data_contract(
            config.evaluation_market_data_path,
            config=config.simulator,
        )
    training_env = create_environment(
        config.market_data_path,
        config=config.simulator,
    )
    evaluation_env: gym.Env | None = None
    try:
        check_env(training_env, warn=verbosity > 0, skip_render_check=True)
        observation, _ = training_env.reset(seed=config.seed)
        if not training_env.observation_space.contains(observation):
            raise RuntimeError("canonical observation is outside its space")

        model = PPO(
            "MlpPolicy",
            training_env,
            learning_rate=0.0003,
            n_steps=DEFAULT_PPO_ROLLOUT_STEPS,
            batch_size=DEFAULT_PPO_BATCH_SIZE,
            seed=config.seed,
            device="cpu",
            verbose=1 if verbosity > 1 else 0,
        )
        callback = (
            TrainingProgressCallback(config.training_timesteps)
            if verbosity > 0
            else None
        )
        model.learn(total_timesteps=config.training_timesteps, callback=callback)
        metadata = model_metadata(training_env, config.simulator)
        archive, sidecar = save_model_artifact(model, config.model_path, metadata)

        evaluation_config = replace(
            config.simulator,
            max_episode_steps=config.evaluation_steps,
        )
        evaluation_env = create_environment(
            config.evaluation_market_data_path,
            config=evaluation_config,
        )
        expected = model_metadata(evaluation_env, evaluation_config)
        loaded = load_model_artifact(
            PPO,
            archive,
            env=evaluation_env,
            expected_metadata=expected,
            device="cpu",
        )
        evaluation = evaluate_model(
            loaded,
            evaluation_env,
            steps=config.evaluation_steps,
            seed=config.seed,
        )
        return TrainingResult(archive, sidecar, evaluation)
    finally:
        if evaluation_env is not None:
            evaluation_env.close()
        training_env.close()


def _model_archive_path(path: Path) -> Path:
    """Compatibility alias for the canonical ZIP naming rule."""

    return model_archive_path(path)


def main(arguments: Sequence[str] | None = None) -> int:
    config = parse_args(arguments)
    if (
        config.evaluation_market_data_path.resolve()
        == config.market_data_path.resolve()
    ):
        print(
            "[warning] evaluation uses the training tape; pass "
            "--evaluation-market-data for an out-of-sample result",
            flush=True,
        )
    result = run_training(config)
    print(f"[persistence] model={result.model_path}", flush=True)
    print(f"[persistence] metadata={result.metadata_path}", flush=True)
    print(
        f"[results] reward={result.evaluation.total_reward:.6f} "
        f"steps={result.evaluation.steps} "
        f"terminated={result.evaluation.terminated} "
        f"execution_quality={EXECUTION_QUALITY}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
