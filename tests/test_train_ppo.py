from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pytest
from stable_baselines3 import PPO


SCRIPTS_DIRECTORY = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))
import train_ppo  # noqa: E402


pytestmark = pytest.mark.training


@pytest.fixture(scope="module")
def trained_result(
    tmp_path_factory: pytest.TempPathFactory,
) -> train_ppo.TrainingResult:
    output = tmp_path_factory.mktemp("ppo") / "agent"
    config = train_ppo.TrainingConfig(
        training_timesteps=8,
        evaluation_steps=6,
        seed=19,
        ppo_rollout_steps=8,
        ppo_batch_size=4,
        model_output=output,
        verbosity=0,
    )
    return train_ppo.run_training(config)


def test_cli_validation_builds_expected_configuration(tmp_path: Path) -> None:
    output = tmp_path / "models" / "candidate.zip"
    config = train_ppo.parse_args(
        [
            "--training-timesteps",
            "16",
            "--evaluation-steps",
            "7",
            "--seed",
            "23",
            "--ppo-rollout-steps",
            "8",
            "--ppo-batch-size",
            "4",
            "--model-output",
            str(output),
            "--verbose",
        ]
    )

    assert config == train_ppo.TrainingConfig(
        training_timesteps=16,
        evaluation_steps=7,
        seed=23,
        ppo_rollout_steps=8,
        ppo_batch_size=4,
        model_output=output,
        verbosity=2,
    )


@pytest.mark.parametrize(
    "arguments",
    [
        ["--training-timesteps", "0"],
        ["--evaluation-steps", "-1"],
        ["--seed", "-1"],
        ["--seed", str(2**32)],
        ["--ppo-rollout-steps", "1"],
        ["--ppo-batch-size", "1"],
        ["--ppo-rollout-steps", "8", "--ppo-batch-size", "16"],
        ["--ppo-rollout-steps", "10", "--ppo-batch-size", "4"],
        ["--quiet", "--verbose"],
    ],
)
def test_invalid_cli_arguments_fail_fast(arguments: list[str]) -> None:
    with pytest.raises(SystemExit) as error:
        train_ppo.parse_args(arguments)

    assert error.value.code == 2


def test_model_persistence_round_trip(
    trained_result: train_ppo.TrainingResult,
) -> None:
    assert trained_result.model_path.is_file()
    assert trained_result.model_path.suffix == ".zip"

    environment = train_ppo.create_environment(seed=19)
    try:
        first = PPO.load(trained_result.model_path, device="cpu")
        second = PPO.load(trained_result.model_path, device="cpu")
        observation, _ = environment.reset(seed=19)
        first_action, _ = first.predict(observation, deterministic=True)
        second_action, _ = second.predict(observation, deterministic=True)
        np.testing.assert_array_equal(first_action, second_action)
    finally:
        environment.close()


def test_deterministic_evaluation_is_reproducible(
    trained_result: train_ppo.TrainingResult,
) -> None:
    model = PPO.load(trained_result.model_path, device="cpu")
    first_environment = train_ppo.create_environment(seed=31)
    second_environment = train_ppo.create_environment(seed=31)
    try:
        first = train_ppo.evaluate_model(
            model,
            first_environment,
            evaluation_steps=12,
            seed=31,
        )
        second = train_ppo.evaluate_model(
            model,
            second_environment,
            evaluation_steps=12,
            seed=31,
        )
    finally:
        first_environment.close()
        second_environment.close()

    assert first == second
