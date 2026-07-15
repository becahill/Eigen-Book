#!/usr/bin/env python3
"""Replay Binance aggregate trades through the accounting-aware environment.

This bounded example treats each aggregate-trade row as aggressive flow with
an observed limit price and quantity. It does not reconstruct exchange depth
or queue position.
"""

from __future__ import annotations

import csv
import math
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any

import gymnasium as gym
import numpy as np

import eigenbook
from eigenbook.env import ExternalTransition, LimitOrderBookEnv


_REPLAY_DISPATCH_STATUSES = frozenset(
    {
        eigenbook.Status.FILLED,
        eigenbook.Status.NO_LIQUIDITY,
        eigenbook.Status.PARTIALLY_FILLED,
    }
)
_FINANCIAL_INFO_KEYS = (
    "inventory",
    "cash",
    "mark_price",
    "wealth",
    "fees_paid",
)
_INT64_MAX = (1 << 63) - 1
_UINT64_MAX = (1 << 64) - 1


def _parse_scaled_integer(
    text: str,
    *,
    scale: int,
    field_name: str,
    row_number: int,
    maximum: int,
) -> int:
    """Parse one positive decimal without rounding or float conversion."""

    try:
        value = Decimal(text.strip())
    except (AttributeError, InvalidOperation) as error:
        raise ValueError(
            f"row {row_number}: {field_name} is not a decimal: {text!r}"
        ) from error
    if not value.is_finite() or value <= 0:
        raise ValueError(f"row {row_number}: {field_name} must be finite and positive")
    numerator, denominator = value.as_integer_ratio()
    scaled_numerator = numerator * scale
    result, remainder = divmod(scaled_numerator, denominator)
    if remainder:
        raise ValueError(
            f"row {row_number}: {field_name}={text!r} exceeds scale {scale}"
        )
    if result > maximum:
        raise ValueError(
            f"row {row_number}: scaled {field_name} exceeds native capacity"
        )
    return result


class BinanceTradeReplayWrapper(gym.Wrapper):
    """Inject a bounded number of checked aggregate trades after each action."""

    def __init__(
        self,
        env: gym.Env,
        csv_path: str | Path,
        *,
        trades_per_action: int = 5,
        price_scale: int = 100,
        quantity_scale: int = 100_000,
    ) -> None:
        super().__init__(env)
        if isinstance(trades_per_action, bool) or not isinstance(
            trades_per_action,
            int,
        ):
            raise TypeError("trades_per_action must be an integer")
        if trades_per_action <= 0:
            raise ValueError("trades_per_action must be a positive integer")
        for name, value in (
            ("price_scale", price_scale),
            ("quantity_scale", quantity_scale),
        ):
            if isinstance(value, bool) or not isinstance(value, int):
                raise TypeError(f"{name} must be an integer")
            if value <= 0:
                raise ValueError(f"{name} must be positive")

        self.trades_per_action = trades_per_action
        self.price_scale = price_scale
        self.quantity_scale = quantity_scale
        self._csv_file = Path(csv_path).open(
            "r",
            newline="",
            encoding="utf-8",
        )
        self._reader = csv.reader(self._csv_file)

        native_env = self.env.unwrapped
        if not isinstance(native_env, LimitOrderBookEnv):
            raise TypeError("BinanceTradeReplayWrapper requires LimitOrderBookEnv")
        self._market_command = eigenbook.Command()
        self._market_command.instrument_id = native_env.instrument_id
        self._market_command.op = eigenbook.CommandOp.ADD
        self._market_command.time_in_force = eigenbook.TimeInForce.IOC
        self._next_replay_order_id = 1 << 63
        self._next_replay_timestamp = 1 << 63

    def reset(self, **kwargs: Any):
        self._csv_file.seek(0)
        self._reader = csv.reader(self._csv_file)
        self._next_replay_order_id = 1 << 63
        self._next_replay_timestamp = 1 << 63
        return super().reset(**kwargs)

    def _dispatch_row(self, row: list[str]) -> ExternalTransition:
        row_number = self._reader.line_num
        if len(row) < 7:
            raise ValueError(
                f"row {row_number}: expected at least seven aggregate-trade fields"
            )
        price = _parse_scaled_integer(
            row[1],
            scale=self.price_scale,
            field_name="price",
            row_number=row_number,
            maximum=_INT64_MAX,
        )
        quantity = _parse_scaled_integer(
            row[2],
            scale=self.quantity_scale,
            field_name="quantity",
            row_number=row_number,
            maximum=_UINT64_MAX,
        )
        raw_buyer_maker = row[6].strip().lower()
        if raw_buyer_maker not in {"true", "false"}:
            raise ValueError(f"row {row_number}: is_buyer_maker must be true or false")

        command = self._market_command
        command.order_id = self._next_replay_order_id
        command.side = (
            eigenbook.Side.SELL if raw_buyer_maker == "true" else eigenbook.Side.BUY
        )
        command.price = price
        command.quantity = quantity
        command.timestamp = self._next_replay_timestamp

        native_env: LimitOrderBookEnv = self.env.unwrapped
        transition = native_env.dispatch_external_transition(command)
        if transition.result.status not in _REPLAY_DISPATCH_STATUSES:
            raise RuntimeError(
                "aggregate-trade IOC returned unexpected status "
                f"{transition.result.status} at row {row_number}"
            )
        self._next_replay_order_id += 1
        self._next_replay_timestamp += 1
        return transition

    def step(self, action: np.ndarray):
        observation, reward, terminated, truncated, agent_info = self.env.step(action)
        info = dict(agent_info)
        replay_results: list[dict[str, Any]] = []
        external_event_count = int(info.get("external_event_count", 0))
        maker_quantity = int(info.get("maker_executed_quantity", 0))
        maker_fees = float(info.get("maker_fees", 0.0))
        step_fees = float(info.get("step_fees", 0.0))
        replay_exhausted = False

        if not terminated and not truncated:
            for _ in range(self.trades_per_action):
                try:
                    row = next(self._reader)
                except StopIteration:
                    replay_exhausted = True
                    truncated = True
                    break

                transition = self._dispatch_row(row)
                observation = transition.observation
                reward += transition.reward
                terminated = transition.terminated
                truncated = transition.truncated
                transition_info = transition.info
                external_event_count += int(transition_info["external_event_count"])
                maker_quantity += int(transition_info["maker_executed_quantity"])
                maker_fees += float(transition_info["maker_fees"])
                step_fees += float(transition_info["step_fees"])
                replay_results.append(
                    {
                        "status": transition.result.status,
                        "executed_quantity": int(transition.result.executed_quantity),
                        "reward": transition.reward,
                    }
                )
                for key in _FINANCIAL_INFO_KEYS:
                    info[key] = transition_info[key]
                if terminated or truncated:
                    break

        info.update(
            external_event_count=external_event_count,
            maker_executed_quantity=maker_quantity,
            maker_fees=maker_fees,
            step_fees=step_fees,
            replay_results=tuple(replay_results),
            replay_exhausted=replay_exhausted,
        )
        if not math.isfinite(float(reward)):
            raise RuntimeError("replay produced a non-finite reward")
        return observation, float(reward), terminated, truncated, info

    def close(self) -> None:
        self._csv_file.close()
        super().close()


def main() -> None:
    book = eigenbook.BookConfig()
    book.min_price = 4_000_000
    book.max_price = 4_500_000
    book.max_orders = 10_000
    book.order_id_map_capacity = 20_000
    book.event_log_capacity = book.max_orders + 2
    book.tick_size = 1

    instrument = eigenbook.InstrumentConfig()
    instrument.instrument_id = 101
    instrument.book_config = book

    env: gym.Env = eigenbook.LimitOrderBookEnv(instrument)
    env = BinanceTradeReplayWrapper(
        env,
        "BTCUSDT-aggTrades-2024-01-01.csv",
    )

    try:
        env.reset(seed=7)
        for step in range(1, 1_001):
            observation, _, terminated, truncated, _ = env.step(
                env.action_space.sample()
            )
            if step % 100 == 0:
                print(f"Step {step}:")
                print(observation)
            if terminated or truncated:
                break
    finally:
        env.close()


if __name__ == "__main__":
    main()
