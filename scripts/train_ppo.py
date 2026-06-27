#!/usr/bin/env python3
"""Train, persist, reload, and evaluate PPO on the Eigen-Book environment."""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
import random

import gymnasium as gym
from gymnasium.wrappers import FlattenObservation
import numpy as np
from stable_baselines3 import PPO
from stable_baselines3.common.callbacks import BaseCallback
from stable_baselines3.common.env_checker import check_env
from stable_baselines3.common.utils import set_random_seed
import torch

import eigenbook
from eigenbook.env import LimitOrderBookEnv


DEFAULT_TRAINING_TIMESTEPS = 100_000
DEFAULT_EVALUATION_STEPS = 1_000
DEFAULT_SEED = 7
DEFAULT_PPO_ROLLOUT_STEPS = 2_048
DEFAULT_PPO_BATCH_SIZE = 64
DEFAULT_MODEL_OUTPUT = Path("training-output/ppo_eigenbook_agent")
MAX_SEED = (2**32) - 1


@dataclass(frozen=True)
class TrainingConfig:
    """Validated command-line configuration."""

    training_timesteps: int
    evaluation_steps: int
    seed: int
    ppo_rollout_steps: int
    ppo_batch_size: int
    model_output: Path
    verbosity: int


@dataclass(frozen=True)
class EvaluationResult:
    """Summary of one deterministic evaluation episode."""

    total_reward: float
    steps: int
    terminated: bool
    truncated: bool


@dataclass(frozen=True)
class TrainingResult:
    """Artifacts and metrics produced by a training run."""

    model_path: Path
    evaluation: EvaluationResult


class TrainingProgressCallback(BaseCallback):
    """Report progress at ten evenly spaced points."""

    def __init__(self, total_timesteps: int) -> None:
        super().__init__(verbose=0)
        self._total_timesteps = total_timesteps
        self._report_points = sorted(
            {
                max(1, (total_timesteps * point + 9) // 10)
                for point in range(1, 11)
            }
        )
        self._next_report_index = 0

    def _on_step(self) -> bool:
        while (
            self._next_report_index < len(self._report_points)
            and self.num_timesteps
            >= self._report_points[self._next_report_index]
        ):
            report_timestep = self._report_points[self._next_report_index]
            percent_complete = (
                100.0 * report_timestep / self._total_timesteps
            )
            print(
                f"[training] {report_timestep:,}/"
                f"{self._total_timesteps:,} requested timesteps "
                f"({percent_complete:.1f}%)",
                flush=True,
            )
            self._next_report_index += 1
        return True


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _ppo_size(value: str) -> int:
    parsed = int(value)
    if parsed < 2:
        raise argparse.ArgumentTypeError("must be an integer of at least 2")
    return parsed


def _seed(value: str) -> int:
    parsed = int(value)
    if parsed < 0 or parsed > MAX_SEED:
        raise argparse.ArgumentTypeError(
            f"must be between 0 and {MAX_SEED}, inclusive"
        )
    return parsed


def build_argument_parser() -> argparse.ArgumentParser:
    """Build the training CLI parser."""
    parser = argparse.ArgumentParser(
        description=(
            "Train, save, reload, and deterministically evaluate a "
            "Stable-Baselines3 PPO demonstration agent."
        )
    )
    parser.add_argument(
        "--training-timesteps",
        type=_positive_int,
        default=DEFAULT_TRAINING_TIMESTEPS,
        help=(
            "requested PPO training timesteps "
            f"(default: {DEFAULT_TRAINING_TIMESTEPS})"
        ),
    )
    parser.add_argument(
        "--evaluation-steps",
        type=_positive_int,
        default=DEFAULT_EVALUATION_STEPS,
        help=(
            "maximum deterministic evaluation steps "
            f"(default: {DEFAULT_EVALUATION_STEPS})"
        ),
    )
    parser.add_argument(
        "--seed",
        type=_seed,
        default=DEFAULT_SEED,
        help=f"random seed in [0, {MAX_SEED}] (default: {DEFAULT_SEED})",
    )
    parser.add_argument(
        "--ppo-rollout-steps",
        type=_ppo_size,
        default=DEFAULT_PPO_ROLLOUT_STEPS,
        help=(
            "steps collected per PPO rollout "
            f"(default: {DEFAULT_PPO_ROLLOUT_STEPS})"
        ),
    )
    parser.add_argument(
        "--ppo-batch-size",
        type=_ppo_size,
        default=DEFAULT_PPO_BATCH_SIZE,
        help=f"PPO minibatch size (default: {DEFAULT_PPO_BATCH_SIZE})",
    )
    parser.add_argument(
        "--model-output",
        type=Path,
        default=DEFAULT_MODEL_OUTPUT,
        help=(
            "model output path; a .zip suffix is added when absent "
            f"(default: {DEFAULT_MODEL_OUTPUT})"
        ),
    )
    output_group = parser.add_mutually_exclusive_group()
    output_group.add_argument(
        "-q",
        "--quiet",
        action="store_const",
        dest="verbosity",
        const=0,
        help="suppress routine progress and result output",
    )
    output_group.add_argument(
        "-v",
        "--verbose",
        action="store_const",
        dest="verbosity",
        const=2,
        help="show routine output and Stable-Baselines3 training metrics",
    )
    parser.set_defaults(verbosity=1)
    return parser


def parse_args(arguments: Sequence[str] | None = None) -> TrainingConfig:
    """Parse and cross-validate command-line arguments."""
    parser = build_argument_parser()
    args = parser.parse_args(arguments)

    if args.ppo_batch_size > args.ppo_rollout_steps:
        parser.error("--ppo-batch-size cannot exceed --ppo-rollout-steps")
    if args.ppo_rollout_steps % args.ppo_batch_size != 0:
        parser.error("--ppo-rollout-steps must be divisible by --ppo-batch-size")
    if not args.model_output.name:
        parser.error("--model-output must name a file")

    return TrainingConfig(
        training_timesteps=args.training_timesteps,
        evaluation_steps=args.evaluation_steps,
        seed=args.seed,
        ppo_rollout_steps=args.ppo_rollout_steps,
        ppo_batch_size=args.ppo_batch_size,
        model_output=args.model_output,
        verbosity=args.verbosity,
    )


def seed_everything(seed: int) -> None:
    """Seed every random-number source used by this CPU-only pipeline."""
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)
    torch.use_deterministic_algorithms(True)
    set_random_seed(seed, using_cuda=False)


def create_environment(
    *,
    seed: int | None = None,
) -> gym.Env[np.ndarray, np.ndarray]:
    """Create the fixed demonstration environment with flattened observations."""
    book = eigenbook.BookConfig()
    book.min_price = 90
    book.max_price = 110
    book.max_orders = 64
    book.order_id_map_capacity = 128
    book.tick_size = 1

    instrument = eigenbook.InstrumentConfig()
    instrument.instrument_id = 101
    instrument.book_config = book

    environment = FlattenObservation(
        LimitOrderBookEnv(
            instrument,
            max_episode_steps=1_000,
            max_abs_inventory=100,
        )
    )
    if seed is not None:
        environment.reset(seed=seed)
        environment.action_space.seed(seed)
        environment.observation_space.seed(seed)
    return environment


def evaluate_model(
    model: PPO,
    environment: gym.Env[np.ndarray, np.ndarray],
    *,
    evaluation_steps: int,
    seed: int,
) -> EvaluationResult:
    """Evaluate a model deterministically for at most one episode."""
    observation, _ = environment.reset(seed=seed)
    total_reward = 0.0
    terminated = False
    truncated = False
    completed_steps = 0

    for completed_steps in range(1, evaluation_steps + 1):
        action, _ = model.predict(observation, deterministic=True)
        observation, reward, terminated, truncated, _ = environment.step(action)
        total_reward += float(reward)
        if terminated or truncated:
            break

    return EvaluationResult(
        total_reward=total_reward,
        steps=completed_steps,
        terminated=terminated,
        truncated=truncated,
    )


def _archive_path(model_output: Path) -> Path:
    if model_output.suffix == ".zip":
        return model_output
    return Path(f"{model_output}.zip")


def _log(config: TrainingConfig, message: str) -> None:
    if config.verbosity > 0:
        print(message, flush=True)


def run_training(config: TrainingConfig) -> TrainingResult:
    """Run environment validation, PPO training, persistence, and evaluation."""
    seed_everything(config.seed)

    _log(config, f"[setup] Using deterministic seed {config.seed} on CPU.")
    validation_environment = create_environment(seed=config.seed)
    try:
        _log(config, "[validation] Checking the Gymnasium environment.")
        check_env(
            validation_environment,
            warn=config.verbosity > 0,
            skip_render_check=True,
        )
    finally:
        validation_environment.close()

    model_path = _archive_path(config.model_output).expanduser().resolve()
    if model_path.exists() and model_path.is_dir():
        raise IsADirectoryError(f"model output is a directory: {model_path}")
    model_path.parent.mkdir(parents=True, exist_ok=True)

    training_environment = create_environment(seed=config.seed)
    try:
        _log(
            config,
            f"[training] Requesting {config.training_timesteps:,} PPO timesteps.",
        )
        model = PPO(
            "MlpPolicy",
            training_environment,
            n_steps=config.ppo_rollout_steps,
            batch_size=config.ppo_batch_size,
            seed=config.seed,
            device="cpu",
            verbose=1 if config.verbosity > 1 else 0,
        )
        callback = (
            None
            if config.verbosity == 0
            else TrainingProgressCallback(config.training_timesteps)
        )
        model.learn(
            total_timesteps=config.training_timesteps,
            callback=callback,
        )
        model.save(model_path)
    finally:
        training_environment.close()

    if not model_path.is_file():
        raise RuntimeError(f"Stable-Baselines3 did not create {model_path}")
    _log(config, f"[persistence] Saved model to {model_path}.")

    evaluation_environment = create_environment(seed=config.seed)
    try:
        loaded_model = PPO.load(
            model_path,
            env=evaluation_environment,
            device="cpu",
        )
        _log(
            config,
            f"[persistence] Reloaded model; evaluating up to "
            f"{config.evaluation_steps:,} steps.",
        )
        evaluation = evaluate_model(
            loaded_model,
            evaluation_environment,
            evaluation_steps=config.evaluation_steps,
            seed=config.seed,
        )
    finally:
        evaluation_environment.close()

    _log(
        config,
        f"[results] reward={evaluation.total_reward:.6f}, "
        f"steps={evaluation.steps:,}, terminated={evaluation.terminated}, "
        f"truncated={evaluation.truncated}",
    )
    return TrainingResult(model_path=model_path, evaluation=evaluation)


def main(arguments: Sequence[str] | None = None) -> int:
    """CLI entry point."""
    run_training(parse_args(arguments))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
