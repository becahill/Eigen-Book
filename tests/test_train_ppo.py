from __future__ import annotations

import importlib
from pathlib import Path
import sys

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIRECTORY = PROJECT_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIRECTORY))

try:
    from experiments import train_market_data as canonical_training  # noqa: E402
    import train_ppo  # noqa: E402
except ModuleNotFoundError as error:
    if error.name not in {"gymnasium", "stable_baselines3", "torch"}:
        raise
    pytest.skip("requires the eigenbook[training] extra", allow_module_level=True)


pytestmark = pytest.mark.training


def test_legacy_entry_point_delegates_to_canonical_pipeline() -> None:
    assert train_ppo.main is canonical_training.main
    assert train_ppo.run_training is canonical_training.run_training
    assert train_ppo.create_environment is canonical_training.create_environment
    assert train_ppo.model_metadata is canonical_training.model_metadata


def test_legacy_entry_point_parses_the_canonical_contract(tmp_path: Path) -> None:
    tape = tmp_path / "sequenced-depth.csv"
    model = tmp_path / "agent"
    config = train_ppo.parse_args(
        [
            "--market-data",
            str(tape),
            "--evaluation-market-data",
            str(tape),
            "--symbol",
            "ETHUSDT",
            "--venue",
            "binance_spot",
            "--price-scale",
            "100",
            "--quantity-scale",
            "1000",
            "--training-timesteps",
            "8",
            "--evaluation-steps",
            "4",
            "--model-path",
            str(model),
        ]
    )

    assert config.market_data_path == tape
    assert config.evaluation_market_data_path == tape
    assert config.simulator.symbol == "ETHUSDT"
    assert config.simulator.price_scale == 100
    assert config.simulator.quantity_scale == 1000
    assert config.model_path == model


def test_removed_synthetic_training_options_fail_closed() -> None:
    with pytest.raises(SystemExit) as error:
        train_ppo.parse_args(["--ppo-rollout-steps", "8"])

    assert error.value.code == 2


def test_paper_trader_is_importable_as_an_experiments_module() -> None:
    module = importlib.import_module("experiments.paper_trader")
    assert module.training.__name__ == "experiments.train_market_data"
