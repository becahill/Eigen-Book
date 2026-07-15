"""Strict model sidecar contract for Eigen-Book policy artifacts."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import tempfile
from typing import Any, Final, Mapping, TypeVar
import zipfile

import gymnasium as gym
import numpy as np

from .market_data import DATA_MODES
from .observation import (
    ACTION_NAMES,
    ACTION_SCHEMA_VERSION,
    OBSERVATION_NAMES,
    OBSERVATION_SCHEMA_VERSION,
)


MODEL_METADATA_VERSION: Final = "eigenbook.model_metadata.v1"


class ModelCompatibilityError(ValueError):
    """Raised when a saved policy is incompatible with the requested run."""


def model_archive_path(path: Path) -> Path:
    """Return a predictable Stable-Baselines3 ZIP path."""

    path = Path(path)
    return path if path.suffix == ".zip" else Path(f"{path}.zip")


def metadata_path(model_path: Path) -> Path:
    """Return the JSON sidecar path beside a model archive."""

    archive = model_archive_path(model_path)
    return archive.with_suffix(".metadata.json")


def _package_version(distribution: str) -> str:
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return "not-installed"


def dependency_versions() -> dict[str, str]:
    """Return deterministic runtime versions required to interpret a model."""

    return {
        "python": platform.python_version(),
        "eigenbook": _package_version("eigenbook"),
        "numpy": _package_version("numpy"),
        "gymnasium": _package_version("gymnasium"),
        "stable_baselines3": _package_version("stable-baselines3"),
        "torch": _package_version("torch"),
    }


def _numeric_list(value: np.ndarray) -> list[int | float]:
    flattened = np.asarray(value).reshape(-1)
    if np.issubdtype(flattened.dtype, np.integer):
        return [int(item) for item in flattened]
    return [float(item) for item in flattened]


def space_signature(space: gym.Space[Any]) -> dict[str, Any]:
    """Serialize the exact Gymnasium space relevant to policy compatibility."""

    if isinstance(space, gym.spaces.Box):
        return {
            "type": "Box",
            "shape": list(space.shape),
            "dtype": np.dtype(space.dtype).str,
            "low": _numeric_list(space.low),
            "high": _numeric_list(space.high),
        }
    if isinstance(space, gym.spaces.MultiDiscrete):
        return {
            "type": "MultiDiscrete",
            "shape": list(space.shape),
            "dtype": np.dtype(space.dtype).str,
            "nvec": _numeric_list(space.nvec),
            "start": _numeric_list(space.start),
        }
    raise TypeError(
        "model metadata supports Box observations and MultiDiscrete actions; "
        f"received {type(space).__name__}"
    )


@dataclass(frozen=True, slots=True)
class ModelMetadata:
    """All semantic and runtime state needed to reload a policy safely."""

    metadata_version: str
    market_data_schema_version: str
    market_data_mode: str
    observation_schema_version: str
    observation_names: tuple[str, ...]
    action_schema_version: str
    action_names: tuple[str, ...]
    observation_space: Mapping[str, Any]
    action_space: Mapping[str, Any]
    price_scale: int
    quantity_scale: int
    symbol: str
    venue: str
    maker_fee_rate: float
    taker_fee_rate: float
    fill_model: str
    feature_configuration: Mapping[str, int | float | bool | str | None]
    dependencies: Mapping[str, str]

    def __post_init__(self) -> None:
        if self.metadata_version != MODEL_METADATA_VERSION:
            raise ValueError(f"metadata_version must be {MODEL_METADATA_VERSION!r}")
        if not self.market_data_schema_version:
            raise ValueError("market_data_schema_version must be non-empty")
        if self.market_data_mode not in DATA_MODES:
            raise ValueError(f"market_data_mode must be one of {sorted(DATA_MODES)}")
        if self.observation_schema_version != OBSERVATION_SCHEMA_VERSION:
            raise ValueError("unsupported observation schema version")
        if tuple(self.observation_names) != OBSERVATION_NAMES:
            raise ValueError("observation names do not match the canonical schema")
        if self.action_schema_version != ACTION_SCHEMA_VERSION:
            raise ValueError("unsupported action schema version")
        if tuple(self.action_names) != ACTION_NAMES:
            raise ValueError("action names do not match the canonical schema")
        if self.price_scale <= 0 or self.quantity_scale <= 0:
            raise ValueError("price and quantity scales must be positive")
        if not self.symbol.strip() or not self.venue.strip():
            raise ValueError("symbol and venue must be non-empty")
        if not self.fill_model.strip():
            raise ValueError("fill_model must be non-empty")
        if not -1.0 < self.maker_fee_rate < 1.0:
            raise ValueError("maker_fee_rate must be strictly between -1 and 1")
        if not 0.0 <= self.taker_fee_rate < 1.0:
            raise ValueError("taker_fee_rate must be in [0, 1)")

    def to_dict(self) -> dict[str, Any]:
        """Return a JSON-compatible mapping with stable tuple conversion."""

        value = asdict(self)
        value["observation_names"] = list(self.observation_names)
        value["action_names"] = list(self.action_names)
        return value

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "ModelMetadata":
        """Validate and construct metadata read from JSON."""

        required = {field.name for field in cls.__dataclass_fields__.values()}
        missing = required - set(value)
        extra = set(value) - required
        if missing or extra:
            raise ModelCompatibilityError(
                "model metadata fields differ from the current contract: "
                f"missing={sorted(missing)}, extra={sorted(extra)}"
            )

        def string(name: str) -> str:
            result = value[name]
            if type(result) is not str:
                raise TypeError(f"{name} must be a string")
            return result

        def integer(name: str) -> int:
            result = value[name]
            if type(result) is not int:
                raise TypeError(f"{name} must be an integer")
            return result

        def number(name: str) -> float:
            result = value[name]
            if type(result) not in {int, float}:
                raise TypeError(f"{name} must be a JSON number")
            converted = float(result)
            if not np.isfinite(converted):
                raise ValueError(f"{name} must be finite")
            return converted

        def string_tuple(name: str) -> tuple[str, ...]:
            result = value[name]
            if not isinstance(result, list) or not all(
                type(item) is str for item in result
            ):
                raise TypeError(f"{name} must be an array of strings")
            return tuple(result)

        def mapping(name: str) -> dict[str, Any]:
            result = value[name]
            if not isinstance(result, Mapping) or not all(
                type(key) is str for key in result
            ):
                raise TypeError(f"{name} must be an object with string keys")
            return dict(result)

        try:
            dependency_mapping = mapping("dependencies")
            if not all(type(item) is str for item in dependency_mapping.values()):
                raise TypeError("dependencies values must be strings")
            feature_mapping = mapping("feature_configuration")
            allowed_feature_types = {bool, int, float, str, type(None)}
            if not all(
                type(item) in allowed_feature_types for item in feature_mapping.values()
            ):
                raise TypeError(
                    "feature_configuration values have unsupported JSON types"
                )
            return cls(
                metadata_version=string("metadata_version"),
                market_data_schema_version=string("market_data_schema_version"),
                market_data_mode=string("market_data_mode"),
                observation_schema_version=string("observation_schema_version"),
                observation_names=string_tuple("observation_names"),
                action_schema_version=string("action_schema_version"),
                action_names=string_tuple("action_names"),
                observation_space=mapping("observation_space"),
                action_space=mapping("action_space"),
                price_scale=integer("price_scale"),
                quantity_scale=integer("quantity_scale"),
                symbol=string("symbol"),
                venue=string("venue"),
                maker_fee_rate=number("maker_fee_rate"),
                taker_fee_rate=number("taker_fee_rate"),
                fill_model=string("fill_model"),
                feature_configuration=feature_mapping,
                dependencies=dependency_mapping,
            )
        except (TypeError, ValueError, KeyError) as error:
            raise ModelCompatibilityError(f"invalid model metadata: {error}") from error


def build_model_metadata(
    env: gym.Env,
    *,
    market_data_schema_version: str,
    market_data_mode: str,
    price_scale: int,
    quantity_scale: int,
    symbol: str,
    venue: str,
    maker_fee_rate: float,
    taker_fee_rate: float,
    fill_model: str,
    feature_configuration: Mapping[str, int | float | bool | str | None],
) -> ModelMetadata:
    """Build metadata from the exact environment presented to the policy."""

    return ModelMetadata(
        metadata_version=MODEL_METADATA_VERSION,
        market_data_schema_version=market_data_schema_version,
        market_data_mode=market_data_mode,
        observation_schema_version=OBSERVATION_SCHEMA_VERSION,
        observation_names=OBSERVATION_NAMES,
        action_schema_version=ACTION_SCHEMA_VERSION,
        action_names=ACTION_NAMES,
        observation_space=space_signature(env.observation_space),
        action_space=space_signature(env.action_space),
        price_scale=price_scale,
        quantity_scale=quantity_scale,
        symbol=symbol,
        venue=venue,
        maker_fee_rate=maker_fee_rate,
        taker_fee_rate=taker_fee_rate,
        fill_model=fill_model,
        feature_configuration=dict(feature_configuration),
        dependencies=dependency_versions(),
    )


def read_model_metadata(model_path: Path) -> ModelMetadata:
    """Read a required sidecar; legacy archives fail closed."""

    sidecar = metadata_path(model_path)
    try:
        raw = json.loads(sidecar.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ModelCompatibilityError(
            f"model metadata sidecar is missing: {sidecar}; legacy models "
            "must be retrained or explicitly migrated"
        ) from error
    except (OSError, json.JSONDecodeError) as error:
        raise ModelCompatibilityError(
            f"unable to read model metadata {sidecar}: {error}"
        ) from error
    if not isinstance(raw, Mapping):
        raise ModelCompatibilityError("model metadata root must be a JSON object")
    return ModelMetadata.from_dict(raw)


def _mismatches(
    saved: Any,
    expected: Any,
    *,
    prefix: str = "",
) -> list[str]:
    if type(saved) is not type(expected):
        return [prefix or "<root>"]
    mismatches: list[str] = []
    if isinstance(saved, Mapping):
        for key in sorted(set(saved) | set(expected)):
            path = f"{prefix}.{key}" if prefix else str(key)
            if key not in saved or key not in expected:
                mismatches.append(path)
                continue
            mismatches.extend(_mismatches(saved[key], expected[key], prefix=path))
        return mismatches
    if isinstance(saved, (list, tuple)):
        if len(saved) != len(expected):
            return [prefix]
        for index, (left, right) in enumerate(zip(saved, expected, strict=True)):
            mismatches.extend(_mismatches(left, right, prefix=f"{prefix}[{index}]"))
        return mismatches
    if saved != expected:
        mismatches.append(prefix)
    return mismatches


def assert_model_compatible(
    saved: ModelMetadata,
    expected: ModelMetadata,
    env: gym.Env,
) -> None:
    """Reject any semantic, dependency, or Gym-space mismatch."""

    expected_dict = expected.to_dict()
    saved_dict = saved.to_dict()
    mismatches = _mismatches(saved_dict, expected_dict)

    runtime_observation = space_signature(env.observation_space)
    runtime_action = space_signature(env.action_space)
    if saved_dict["observation_space"] != runtime_observation:
        mismatches.append("runtime.observation_space")
    if saved_dict["action_space"] != runtime_action:
        mismatches.append("runtime.action_space")
    if mismatches:
        raise ModelCompatibilityError(
            "model metadata is incompatible: " + ", ".join(sorted(set(mismatches)))
        )


ModelT = TypeVar("ModelT")


def save_model_artifact(
    model: Any,
    path: Path,
    metadata: ModelMetadata,
) -> tuple[Path, Path]:
    """Build and validate temporary artifacts before atomic file replacement."""

    archive = model_archive_path(path).expanduser().resolve()
    sidecar = metadata_path(archive)
    archive.parent.mkdir(parents=True, exist_ok=True)
    if archive.exists() and archive.is_dir():
        raise IsADirectoryError(f"model output is a directory: {archive}")
    if sidecar.exists() and sidecar.is_dir():
        raise IsADirectoryError(f"model metadata output is a directory: {sidecar}")

    metadata_json = json.dumps(metadata.to_dict(), indent=2, sort_keys=True) + "\n"
    temporary_archive: Path | None = None
    temporary_metadata: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            dir=archive.parent,
            prefix=f".{archive.name}.",
            suffix=".zip",
        )
        os.close(descriptor)
        temporary_archive = Path(temporary_name)
        temporary_archive.unlink()
        model.save(temporary_archive)
        if not temporary_archive.is_file():
            raise RuntimeError(
                f"model save did not create temporary archive {temporary_archive}"
            )
        try:
            with zipfile.ZipFile(temporary_archive, "r") as model_zip:
                corrupt_member = model_zip.testzip()
        except zipfile.BadZipFile as error:
            raise RuntimeError("model save produced an invalid ZIP archive") from error
        if corrupt_member is not None:
            raise RuntimeError(
                f"model ZIP failed CRC validation at member {corrupt_member!r}"
            )
        with temporary_archive.open("rb") as saved_archive:
            os.fsync(saved_archive.fileno())

        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=sidecar.parent,
            prefix=f".{sidecar.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_metadata = Path(output.name)
            output.write(metadata_json)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_archive, archive)
        temporary_archive = None
        os.replace(temporary_metadata, sidecar)
        temporary_metadata = None
    finally:
        if temporary_archive is not None:
            temporary_archive.unlink(missing_ok=True)
        if temporary_metadata is not None:
            temporary_metadata.unlink(missing_ok=True)
    return archive, sidecar


def load_model_artifact(
    model_class: type[ModelT],
    path: Path,
    *,
    env: gym.Env,
    expected_metadata: ModelMetadata,
    **load_kwargs: Any,
) -> ModelT:
    """Validate the sidecar and spaces before loading model parameters."""

    archive = model_archive_path(path).expanduser().resolve()
    if not archive.is_file():
        raise FileNotFoundError(f"model archive does not exist: {archive}")
    saved_metadata = read_model_metadata(archive)
    assert_model_compatible(saved_metadata, expected_metadata, env)
    return model_class.load(archive, env=env, **load_kwargs)


__all__ = [
    "MODEL_METADATA_VERSION",
    "ModelCompatibilityError",
    "ModelMetadata",
    "assert_model_compatible",
    "build_model_metadata",
    "dependency_versions",
    "load_model_artifact",
    "metadata_path",
    "model_archive_path",
    "read_model_metadata",
    "save_model_artifact",
    "space_signature",
]
