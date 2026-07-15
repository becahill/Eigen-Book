#!/usr/bin/env python3
"""Compatibility launcher for the canonical sequenced-market PPO pipeline.

The former script trained against a flattened raw engine-depth tensor that was
incompatible with market-data training and paper replay.  Keeping this filename
as a thin launcher avoids maintaining a second observation or model format.
"""

from __future__ import annotations

from pathlib import Path
import sys


PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from experiments.train_market_data import (  # noqa: E402,F401
    EvaluationResult,
    ExperimentConfig,
    TrainingResult,
    build_argument_parser,
    create_environment,
    evaluate_model,
    main,
    model_metadata,
    parse_args,
    run_training,
)


__all__ = (
    "EvaluationResult",
    "ExperimentConfig",
    "TrainingResult",
    "build_argument_parser",
    "create_environment",
    "evaluate_model",
    "main",
    "model_metadata",
    "parse_args",
    "run_training",
)


if __name__ == "__main__":
    raise SystemExit(main())
