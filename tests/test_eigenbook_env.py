from __future__ import annotations

import numpy as np
import pytest
from gymnasium.utils.env_checker import check_env

import eigenbook as eb
from eigenbook.env import LimitOrderBookEnv

from python_helpers import make_book_config, make_instrument


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


def test_gymnasium_api_and_spaces() -> None:
    env = make_environment()
    check_env(env, skip_render_check=True)

    observation, info = env.reset(seed=7, options={})
    assert observation.shape == (2, 5, 2)
    assert observation.dtype == np.float32
    assert env.observation_space.contains(observation)
    assert info == {"inventory": 0, "elapsed_steps": 0}

    event_buffer_address = env.event_buffer.ctypes.data
    next_observation, reward, terminated, truncated, step_info = env.step(
        np.array([1, 10, 4], dtype=np.int64)
    )
    assert next_observation is observation
    assert env.event_buffer.ctypes.data == event_buffer_address
    assert env.observation_space.contains(next_observation)
    assert reward == -5.0
    assert terminated is False
    assert truncated is False
    assert step_info["event_count"] == 2
    assert step_info["status"] == eb.Status.ACCEPTED
    assert step_info["executed_quantity"] == 0
    assert step_info["residual_quantity"] == 5
    assert step_info["resting_quantity"] == 5
    assert step_info["inventory"] == 0
    np.testing.assert_array_equal(
        observation[1, 0],
        np.array([100.0, 5.0], dtype=np.float32),
    )

    _, reward, terminated, truncated, step_info = env.step(
        np.array([0, 10, 4], dtype=np.int64)
    )
    assert reward == 5.0
    assert terminated is False
    assert truncated is False
    assert step_info["status"] == eb.Status.FILLED
    assert step_info["executed_quantity"] == 5
    assert step_info["residual_quantity"] == 0
    assert step_info["inventory"] == 5
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
    observation, info = env.reset(seed=1, options={"initial_inventory": -3})
    assert info == {"inventory": -3, "elapsed_steps": 0}

    env.step(np.array([0, 10, 0], dtype=np.int64))
    assert observation[0, 0, 1] == 1

    reset_observation, reset_info = env.reset(seed=2)
    assert reset_observation is observation
    assert reset_info == {"inventory": 0, "elapsed_steps": 0}
    np.testing.assert_array_equal(reset_observation, np.zeros((2, 5, 2), dtype=np.float32))

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
    _, _, terminated, truncated, _ = env.step(
        np.array([1, 10, 0], dtype=np.int64)
    )
    assert not terminated
    assert not truncated

    _, reward, terminated, truncated, info = env.step(
        np.array([0, 10, 0], dtype=np.int64)
    )
    assert reward == 1.0
    assert terminated
    assert not truncated
    assert info["inventory"] == 1

    with pytest.raises(RuntimeError, match="episode has ended"):
        env.step(np.array([0, 10, 0], dtype=np.int64))


def test_truncation_and_post_episode_lifecycle() -> None:
    env = make_environment(
        max_episode_steps=2,
        max_abs_inventory=None,
    )
    env.reset(seed=1)
    _, _, terminated, truncated, _ = env.step(
        np.array([1, 10, 0], dtype=np.int64)
    )
    assert not terminated
    assert not truncated

    _, _, terminated, truncated, info = env.step(
        np.array([1, 10, 0], dtype=np.int64)
    )
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
    with pytest.raises(ValueError, match="outside the configured book"):
        LimitOrderBookEnv(instrument, max_price_offset_ticks=11)


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
