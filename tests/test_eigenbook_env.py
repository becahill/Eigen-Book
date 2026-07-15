from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

import eigenbook as eb
from python_helpers import make_add_command, make_book_config, make_instrument

try:
    from gymnasium.utils.env_checker import check_env

    from eigenbook.env import ExternalDispatchError, LimitOrderBookEnv
except ModuleNotFoundError as error:
    if error.name != "gymnasium":
        raise
    pytest.skip("requires the eigenbook[rl] extra", allow_module_level=True)

EXPERIMENTS_DIRECTORY = Path(__file__).resolve().parents[1] / "experiments"
sys.path.insert(0, str(EXPERIMENTS_DIRECTORY))
from historical_replay import BinanceTradeReplayWrapper  # noqa: E402


pytestmark = pytest.mark.rl


def make_environment(
    *,
    max_episode_steps: int | None = 10,
    max_abs_inventory: int | None = None,
) -> LimitOrderBookEnv:
    book = make_book_config(
        max_orders=64,
        event_log_capacity=66,
    )
    return LimitOrderBookEnv(
        make_instrument(book_config=book),
        max_price_offset_ticks=10,
        max_order_quantity=5,
        max_episode_steps=max_episode_steps,
        max_abs_inventory=max_abs_inventory,
    )


def make_external_command(
    *,
    order_id: int,
    side: eb.Side,
    price: int,
    quantity: int,
    time_in_force: eb.TimeInForce = eb.TimeInForce.IOC,
) -> eb.Command:
    command = make_add_command(
        order_id=order_id,
        side=side,
        price=price,
        quantity=quantity,
        timestamp=order_id,
    )
    command.time_in_force = time_in_force
    return command


def test_gymnasium_api_and_spaces() -> None:
    env = make_environment()
    check_env(env, skip_render_check=True)

    observation, info = env.reset(seed=7, options={})
    assert observation.shape == (2, 5, 2)
    assert observation.dtype == np.float32
    assert env.observation_space.contains(observation)
    assert info == {
        "inventory": 0,
        "cash": 0.0,
        "mark_price": 100.0,
        "wealth": 0.0,
        "fees_paid": 0.0,
        "elapsed_steps": 0,
    }

    event_buffer_address = env.event_buffer.ctypes.data
    next_observation, reward, terminated, truncated, step_info = env.step(
        np.array([1, 10, 4], dtype=np.int64)
    )
    assert next_observation is observation
    assert env.event_buffer.ctypes.data == event_buffer_address
    assert env.observation_space.contains(next_observation)
    assert reward == 0.0
    assert terminated is False
    assert truncated is False
    assert step_info["event_count"] == 2
    assert step_info["status"] == eb.Status.ACCEPTED
    assert step_info["executed_quantity"] == 0
    assert step_info["residual_quantity"] == 5
    assert step_info["resting_quantity"] == 5
    assert step_info["inventory"] == 0
    assert step_info["cash"] == 0.0
    assert step_info["wealth"] == 0.0
    np.testing.assert_array_equal(
        observation[1, 0],
        np.array([100.0, 5.0], dtype=np.float32),
    )

    _, reward, terminated, truncated, step_info = env.step(
        np.array([0, 10, 4], dtype=np.int64)
    )
    # Both orders belong to the agent. Crossing them is economically flat
    # before fees and debits both the maker and taker legs.
    assert reward == pytest.approx(-0.35)
    assert terminated is False
    assert truncated is False
    assert step_info["status"] == eb.Status.FILLED
    assert step_info["executed_quantity"] == 5
    assert step_info["residual_quantity"] == 0
    assert step_info["maker_executed_quantity"] == 5
    assert step_info["taker_executed_quantity"] == 5
    assert step_info["maker_fees"] == pytest.approx(0.1)
    assert step_info["taker_fees"] == pytest.approx(0.25)
    assert step_info["cash"] == pytest.approx(-0.35)
    assert step_info["inventory"] == 0
    assert step_info["wealth"] == pytest.approx(-0.35)
    np.testing.assert_array_equal(observation, np.zeros((2, 5, 2), dtype=np.float32))


def test_seeded_episodes_are_reproducible() -> None:
    first = make_environment(max_episode_steps=8)
    second = make_environment(max_episode_steps=8)
    first_observation, _ = first.reset(seed=123)
    second_observation, _ = second.reset(seed=123)
    np.testing.assert_array_equal(first_observation, second_observation)

    for _ in range(8):
        first_action = first.action_space.sample()
        second_action = second.action_space.sample()
        np.testing.assert_array_equal(first_action, second_action)

        first_transition = first.step(first_action)
        second_transition = second.step(second_action)
        np.testing.assert_array_equal(first_transition[0], second_transition[0])
        assert first_transition[1:4] == second_transition[1:4]
        assert first_transition[4] == second_transition[4]


def test_reset_options_and_engine_reinitialization() -> None:
    env = make_environment(max_abs_inventory=10)
    observation, info = env.reset(
        seed=1,
        options={"initial_inventory": -3, "initial_cash": 1_000.0},
    )
    assert info == {
        "inventory": -3,
        "cash": 1_000.0,
        "mark_price": 100.0,
        "wealth": 700.0,
        "fees_paid": 0.0,
        "elapsed_steps": 0,
    }

    env.step(np.array([0, 10, 0], dtype=np.int64))
    assert observation[0, 0, 1] == 1

    reset_observation, reset_info = env.reset(seed=2)
    assert reset_observation is observation
    assert reset_info == {
        "inventory": 0,
        "cash": 0.0,
        "mark_price": 100.0,
        "wealth": 0.0,
        "fees_paid": 0.0,
        "elapsed_steps": 0,
    }
    np.testing.assert_array_equal(
        reset_observation, np.zeros((2, 5, 2), dtype=np.float32)
    )

    with pytest.raises(ValueError, match="unsupported reset"):
        env.reset(options={"unknown": 1})
    with pytest.raises(TypeError, match="options"):
        env.reset(options=[])  # type: ignore[arg-type]
    with pytest.raises(ValueError, match="termination boundary"):
        env.reset(options={"initial_inventory": 10})


def test_termination_and_post_episode_lifecycle() -> None:
    env = make_environment(
        max_episode_steps=10,
        max_abs_inventory=1,
    )
    env.reset(seed=1)
    _, _, terminated, truncated, _ = env.step(np.array([0, 10, 0], dtype=np.int64))
    assert not terminated
    assert not truncated

    external_sell = make_external_command(
        order_id=1 << 63,
        side=eb.Side.SELL,
        price=100,
        quantity=1,
    )
    transition = env.dispatch_external_transition(external_sell)
    assert transition.result.status == eb.Status.FILLED
    assert transition.reward == pytest.approx(-0.02)
    assert transition.terminated
    assert not transition.truncated
    info = transition.info
    assert info["external_executed_quantity"] == 1
    assert info["maker_executed_quantity"] == 1
    assert info["inventory"] == 1
    assert info["cash"] == pytest.approx(-100.02)
    assert info["wealth"] == pytest.approx(-0.02)
    assert info["maker_fees"] == pytest.approx(0.02)

    with pytest.raises(RuntimeError, match="episode has ended"):
        env.step(np.array([0, 10, 0], dtype=np.int64))


def test_truncation_and_post_episode_lifecycle() -> None:
    env = make_environment(
        max_episode_steps=2,
        max_abs_inventory=None,
    )
    env.reset(seed=1)
    _, _, terminated, truncated, _ = env.step(np.array([1, 10, 0], dtype=np.int64))
    assert not terminated
    assert not truncated

    _, _, terminated, truncated, info = env.step(np.array([1, 10, 0], dtype=np.int64))
    assert not terminated
    assert truncated
    assert info["elapsed_steps"] == 2

    with pytest.raises(RuntimeError, match="episode has ended"):
        env.step(np.array([1, 10, 0], dtype=np.int64))


def test_invalid_actions_and_lifecycle_are_explicit() -> None:
    env = make_environment()
    valid_action = np.array([0, 10, 0], dtype=np.int64)

    with pytest.raises(RuntimeError, match=r"reset\(\)"):
        env.step(valid_action)

    env.reset(seed=1)
    with pytest.raises(ValueError, match=r"shape \(3,\)"):
        env.step(np.array([0, 10], dtype=np.int64))
    with pytest.raises(TypeError, match="integer dtype"):
        env.step(np.array([0.0, 10.0, 0.0], dtype=np.float32))
    with pytest.raises(ValueError, match="action space"):
        env.step(np.array([2, 10, 0], dtype=np.int64))
    with pytest.raises(ValueError, match="action space"):
        env.step(np.array([0, 21, 0], dtype=np.int64))
    with pytest.raises(ValueError, match="action space"):
        env.step(np.array([0, 10, 5], dtype=np.int64))

    env.close()
    with pytest.raises(RuntimeError, match="closed"):
        env.step(valid_action)
    with pytest.raises(RuntimeError, match="closed"):
        env.reset()


def test_invalid_environment_configuration_is_explicit() -> None:
    instrument = make_instrument(
        book_config=make_book_config(max_orders=8),
    )
    with pytest.raises(TypeError, match="max_price_offset_ticks"):
        LimitOrderBookEnv(instrument, max_price_offset_ticks=1.5)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="max_order_quantity"):
        LimitOrderBookEnv(instrument, max_order_quantity=1.5)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="max_episode_steps"):
        LimitOrderBookEnv(instrument, max_episode_steps=1.5)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="max_abs_inventory"):
        LimitOrderBookEnv(instrument, max_abs_inventory=1.5)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="maker_fee_rate"):
        LimitOrderBookEnv(instrument, maker_fee_rate=True)
    with pytest.raises(ValueError, match="maker_fee_rate"):
        LimitOrderBookEnv(instrument, maker_fee_rate=-0.001)
    with pytest.raises(ValueError, match="taker_fee_rate"):
        LimitOrderBookEnv(instrument, taker_fee_rate=float("inf"))
    with pytest.raises(ValueError, match="outside the configured book"):
        LimitOrderBookEnv(instrument, max_price_offset_ticks=11)
    oversized_lots = make_instrument(
        book_config=make_book_config(max_orders=8),
        lot_size=int(np.iinfo(np.uint64).max),
    )
    with pytest.raises(ValueError, match="uint64 capacity"):
        LimitOrderBookEnv(oversized_lots, max_order_quantity=2)


def test_quantities_scale_by_configured_lot_size() -> None:
    book = make_book_config(max_orders=8, event_log_capacity=10)
    env = LimitOrderBookEnv(
        make_instrument(book_config=book, lot_size=5),
        max_price_offset_ticks=10,
        max_order_quantity=3,
    )
    observation, _ = env.reset(seed=1)

    _, reward, _, _, info = env.step(np.array([1, 10, 2], dtype=np.int64))

    assert reward == 0.0
    assert info["status"] == eb.Status.ACCEPTED
    assert info["residual_quantity"] == 15
    assert info["resting_quantity"] == 15
    assert observation[1, 0, 1] == 15


def test_exact_execution_cash_and_adverse_selection_reward() -> None:
    book = make_book_config(max_orders=8, event_log_capacity=10)
    env = LimitOrderBookEnv(
        make_instrument(book_config=book),
        max_price_offset_ticks=10,
        max_order_quantity=1,
        maker_fee_rate=0.0,
        taker_fee_rate=0.0,
    )
    env.reset(seed=1)

    assert (
        env.dispatch_external(
            make_external_command(
                order_id=1 << 63,
                side=eb.Side.BUY,
                price=103,
                quantity=1,
                time_in_force=eb.TimeInForce.GTC,
            )
        ).status
        == eb.Status.ACCEPTED
    )
    assert (
        env.dispatch_external(
            make_external_command(
                order_id=(1 << 63) + 1,
                side=eb.Side.SELL,
                price=105,
                quantity=1,
                time_in_force=eb.TimeInForce.GTC,
            )
        ).status
        == eb.Status.ACCEPTED
    )

    _, reward, _, _, info = env.step(np.array([0, 15, 0], dtype=np.int64))

    assert info["executed_quantity"] == 1
    assert info["taker_executed_quantity"] == 1
    assert info["inventory"] == 1
    assert info["cash"] == -105.0
    assert info["mark_price"] == 104.0
    assert info["wealth"] == -1.0
    assert reward == -1.0


def test_external_fill_is_accounted_as_maker_execution() -> None:
    """The legacy API retains deferred reporting compatibility."""

    env = make_environment()
    env.reset(seed=1)

    env.step(np.array([1, 11, 1], dtype=np.int64))
    result = env.dispatch_external(
        make_external_command(
            order_id=1 << 63,
            side=eb.Side.BUY,
            price=101,
            quantity=2,
        )
    )
    assert result.status == eb.Status.FILLED

    _, reward, _, _, info = env.step(np.array([0, 0, 0], dtype=np.int64))

    assert info["external_event_count"] == 2
    assert info["maker_executed_quantity"] == 2
    assert info["inventory"] == -2
    assert info["cash"] == pytest.approx(201.9596)
    assert info["maker_fees"] == pytest.approx(0.0404)
    assert info["wealth"] == pytest.approx(1.9596)
    assert reward == pytest.approx(1.9596)


def test_external_partial_maker_fill_is_a_complete_transition() -> None:
    env = make_environment()
    observation, _ = env.reset(seed=1)

    # Rest five agent lots at the 101 ask, then consume only two of them.
    env.step(np.array([1, 11, 4], dtype=np.int64))
    transition = env.dispatch_external_transition(
        make_external_command(
            order_id=1 << 63,
            side=eb.Side.BUY,
            price=101,
            quantity=2,
        )
    )

    assert transition.observation is observation
    assert transition.result.status == eb.Status.FILLED
    assert transition.result.executed_quantity == 2
    assert transition.reward == pytest.approx(1.9596)
    assert not transition.terminated
    assert not transition.truncated
    assert transition.info["external_event_count"] == 2
    assert transition.info["external_executed_quantity"] == 2
    assert transition.info["maker_executed_quantity"] == 2
    assert transition.info["taker_executed_quantity"] == 0
    assert transition.info["inventory"] == -2
    assert transition.info["cash"] == pytest.approx(201.9596)
    assert transition.info["maker_fees"] == pytest.approx(0.0404)
    assert transition.info["step_fees"] == pytest.approx(0.0404)
    assert transition.info["fees_paid"] == pytest.approx(0.0404)
    assert transition.info["mark_price"] == 100.0
    assert transition.info["wealth"] == pytest.approx(1.9596)
    np.testing.assert_array_equal(
        transition.observation[1, 0],
        np.array([101.0, 3.0], dtype=np.float32),
    )

    # A bid at 99 restores a 99x101 midpoint of 100 without executing. The
    # external maker fill and fee must not be reported or rewarded twice.
    _, reward, terminated, truncated, info = env.step(
        np.array([0, 9, 0], dtype=np.int64)
    )
    assert reward == 0.0
    assert not terminated
    assert not truncated
    assert info["external_event_count"] == 0
    assert info["external_executed_quantity"] == 0
    assert info["maker_executed_quantity"] == 0
    assert info["maker_fees"] == 0.0
    assert info["step_fees"] == 0.0
    assert info["fees_paid"] == pytest.approx(0.0404)
    assert info["wealth"] == pytest.approx(1.9596)


def test_external_transition_rejects_invalid_status_and_exposes_result() -> None:
    env = make_environment()
    env.reset(seed=1)

    invalid = make_external_command(
        order_id=1 << 63,
        side=eb.Side.BUY,
        price=111,
        quantity=1,
    )
    with pytest.raises(ExternalDispatchError) as captured:
        env.dispatch_external_transition(invalid)

    assert captured.value.result.status == eb.Status.INVALID_PRICE
    assert captured.value.result.executed_quantity == 0
    assert env.inventory == 0
    assert env.cash == 0.0
    assert env.wealth == 0.0
    assert env.fees_paid == 0.0

    accepted = env.dispatch_external_transition(
        make_external_command(
            order_id=(1 << 63) + 1,
            side=eb.Side.BUY,
            price=99,
            quantity=1,
            time_in_force=eb.TimeInForce.GTC,
        )
    )
    assert accepted.result.status == eb.Status.ACCEPTED
    assert accepted.info["external_resting_quantity"] == 1
    assert accepted.reward == 0.0


def test_historical_replay_wrapper_returns_checked_external_accounting(
    tmp_path: Path,
) -> None:
    csv_path = tmp_path / "agg-trades.csv"
    csv_path.write_text(
        "1,101,2,1,1,1000,false,true\n",
        encoding="utf-8",
    )
    env = BinanceTradeReplayWrapper(
        make_environment(),
        csv_path,
        trades_per_action=1,
        price_scale=1,
        quantity_scale=1,
    )
    try:
        observation, _ = env.reset(seed=1)
        next_observation, reward, terminated, truncated, info = env.step(
            np.array([1, 11, 4], dtype=np.int64)
        )

        assert next_observation is observation
        assert reward == pytest.approx(1.9596)
        assert not terminated
        assert not truncated
        assert info["external_event_count"] == 2
        assert info["maker_executed_quantity"] == 2
        assert info["maker_fees"] == pytest.approx(0.0404)
        assert info["cash"] == pytest.approx(201.9596)
        assert info["inventory"] == -2
        assert info["wealth"] == pytest.approx(1.9596)
        assert info["replay_results"] == (
            {
                "status": eb.Status.FILLED,
                "executed_quantity": 2,
                "reward": pytest.approx(1.9596),
            },
        )
        np.testing.assert_array_equal(
            observation[1, 0],
            np.array([101.0, 3.0], dtype=np.float32),
        )
    finally:
        env.close()


def test_multiple_environments_are_independent() -> None:
    first = make_environment()
    second = make_environment()
    first_observation, _ = first.reset(seed=1)
    second_observation, _ = second.reset(seed=1)

    first.step(np.array([1, 10, 0], dtype=np.int64))
    assert first_observation[1, 0, 1] == 1
    np.testing.assert_array_equal(
        second_observation,
        np.zeros((2, 5, 2), dtype=np.float32),
    )

    second.step(np.array([0, 9, 1], dtype=np.int64))
    assert second_observation[0, 0, 1] == 2
    assert first_observation[1, 0, 1] == 1
