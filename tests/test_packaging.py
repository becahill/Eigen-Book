from __future__ import annotations

import importlib.metadata
from importlib.machinery import EXTENSION_SUFFIXES
import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest

import eigenbook


def test_installed_distribution_and_native_module() -> None:
    assert importlib.metadata.version("eigenbook") == eigenbook.__version__
    assert eigenbook.NATIVE_BUILD_TYPE == "Release"
    assert eigenbook.NATIVE_COMPILER

    package_path = Path(eigenbook.__file__).resolve()
    repository = Path(__file__).resolve().parents[1]
    assert repository not in package_path.parents

    native_spec = importlib.util.find_spec("eigenbook._eigenbook")
    assert native_spec is not None
    assert native_spec.origin is not None
    assert any(native_spec.origin.endswith(suffix) for suffix in EXTENSION_SUFFIXES)


def test_import_works_in_isolated_mode_from_outside_repository(
    tmp_path: Path,
) -> None:
    completed = subprocess.run(
        [
            sys.executable,
            "-I",
            "-c",
            "import eigenbook; print(eigenbook.__version__)",
        ],
        cwd=tmp_path,
        check=True,
        capture_output=True,
        text=True,
    )
    assert completed.stdout.strip() == eigenbook.__version__


def test_optional_rl_import_has_actionable_core_only_error() -> None:
    if importlib.util.find_spec("gymnasium") is not None:
        pytest.skip("core-only dependency behavior requires Gymnasium to be absent")

    with pytest.raises(ModuleNotFoundError, match=r"eigenbook\[rl\]"):
        from eigenbook import LimitOrderBookEnv  # noqa: F401
