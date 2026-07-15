#!/usr/bin/env python3
"""Run a saved policy as a deterministic, no-order-transmission paper replay.

This command consumes the same canonical sequenced depth-and-trade tape, fill
model, causal feature extractor, observation schema, action schema, and model
sidecar used during training.  It deliberately does not substitute one venue's
live trades for another venue's training book.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from stable_baselines3 import PPO

from eigenbook.model_artifact import (
    ModelCompatibilityError,
    load_model_artifact,
    read_model_metadata,
)
from eigenbook.replay import ReplayConfig

if __package__:
    from . import train_market_data as training
else:
    import train_market_data as training


@dataclass(frozen=True, slots=True)
class PaperConfig:
    market_data_path: Path
    model_path: Path
    maximum_steps: int
    seed: int


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def _seed(value: str) -> int:
    parsed = int(value)
    if parsed < 0 or parsed > (1 << 32) - 1:
        raise argparse.ArgumentTypeError("must fit an unsigned 32-bit seed")
    return parsed


def parse_args(arguments: Sequence[str] | None = None) -> PaperConfig:
    parser = argparse.ArgumentParser(
        description=(
            "Evaluate a compatible policy on a validated, bounded paper tape; "
            "no orders are transmitted."
        )
    )
    parser.add_argument("--market-data", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--maximum-steps", type=_positive_int, default=10_000)
    parser.add_argument("--seed", type=_seed, default=7)
    args = parser.parse_args(arguments)
    return PaperConfig(args.market_data, args.model, args.maximum_steps, args.seed)


def _required_feature(
    configuration: Mapping[str, Any],
    name: str,
) -> Any:
    if name not in configuration:
        raise ModelCompatibilityError(
            f"model feature configuration is missing {name!r}"
        )
    return configuration[name]


def replay_config_from_model(model_path: Path, *, maximum_steps: int) -> ReplayConfig:
    """Reconstruct the exact simulator semantics stored with a model."""

    metadata = read_model_metadata(model_path)
    feature = metadata.feature_configuration
    return ReplayConfig(
        symbol=metadata.symbol,
        venue=metadata.venue,
        data_mode=metadata.market_data_mode,
        price_scale=metadata.price_scale,
        quantity_scale=metadata.quantity_scale,
        own_order_min_price=int(_required_feature(feature, "own_order_min_price")),
        own_order_max_price=int(_required_feature(feature, "own_order_max_price")),
        tick_size=int(_required_feature(feature, "tick_size")),
        lot_size=int(_required_feature(feature, "lot_size")),
        max_quote_distance_ticks=int(
            _required_feature(feature, "max_quote_distance_ticks")
        ),
        max_order_quantity_lots=int(
            _required_feature(feature, "max_order_quantity_lots")
        ),
        max_abs_inventory_lots=(
            None
            if _required_feature(feature, "max_abs_inventory_lots") is None
            else int(feature["max_abs_inventory_lots"])
        ),
        max_episode_steps=maximum_steps,
        events_per_action=int(_required_feature(feature, "events_per_action")),
        order_entry_latency_events=int(
            _required_feature(feature, "order_entry_latency_events")
        ),
        maker_fee_rate=metadata.maker_fee_rate,
        taker_fee_rate=metadata.taker_fee_rate,
        feature_window_size=int(_required_feature(feature, "window_size")),
        feature_spread_scale_ticks=int(
            _required_feature(feature, "spread_scale_ticks")
        ),
        inventory_penalty_rate=float(
            _required_feature(feature, "inventory_penalty_rate")
        ),
        liquidate_on_termination=bool(
            _required_feature(feature, "liquidate_on_termination")
        ),
    )


def _pre_inference_checks(env: Any) -> None:
    base = env.unwrapped
    if not base.feed_synchronized:
        raise RuntimeError("feed is unsynchronized; inference is prohibited")
    limit = base.config.max_abs_inventory
    if limit is not None and abs(base.inventory) >= limit:
        raise RuntimeError("inventory risk limit reached before inference")
    for name, price in (
        ("best_bid", base.market.best_bid),
        ("best_ask", base.market.best_ask),
    ):
        if price is not None and not (
            base.config.own_order_min_price <= price <= base.config.own_order_max_price
        ):
            raise RuntimeError(
                f"{name}={price} is outside the configured own-order range"
            )


def run_paper_replay(config: PaperConfig) -> dict[str, Any]:
    """Run compatible inference until the bounded tape or risk episode ends."""

    replay_config = replay_config_from_model(
        config.model_path,
        maximum_steps=config.maximum_steps,
    )
    training.validate_market_data_contract(
        config.market_data_path,
        config=replay_config,
    )
    env = training.create_environment(
        config.market_data_path,
        config=replay_config,
    )
    try:
        expected = training.model_metadata(env, replay_config)
        model = load_model_artifact(
            PPO,
            config.model_path,
            env=env,
            expected_metadata=expected,
            device="cpu",
        )
        observation, reset_info = env.reset(seed=config.seed)
        if not reset_info["feed_synchronized"]:
            raise RuntimeError("initial snapshot did not synchronize the feed")

        total_reward = 0.0
        final_info: dict[str, Any] = dict(reset_info)
        terminated = truncated = False
        completed = 0
        for completed in range(1, config.maximum_steps + 1):
            _pre_inference_checks(env)
            action, _ = model.predict(observation, deterministic=True)
            if not env.action_space.contains(action):
                raise RuntimeError("model produced an action outside its saved space")
            observation, reward, terminated, truncated, final_info = env.step(action)
            total_reward += float(reward)
            if not final_info["feed_synchronized"]:
                raise RuntimeError("feed lost synchronization during paper replay")
            if terminated or truncated:
                break
        return {
            "steps": completed,
            "total_reward": total_reward,
            "terminated": terminated,
            "truncated": truncated,
            "final_info": final_info,
            "symbol": replay_config.symbol,
            "venue": replay_config.venue,
        }
    finally:
        env.close()


def main(arguments: Sequence[str] | None = None) -> int:
    result = run_paper_replay(parse_args(arguments))
    info = result["final_info"]
    print(
        f"[paper] venue={result['venue']} symbol={result['symbol']} "
        f"steps={result['steps']} reward={result['total_reward']:.6f} "
        f"inventory={info.get('inventory', 0)} "
        f"wealth={info.get('wealth', 0.0):.6f} "
        f"termination={info.get('termination_reason')}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
