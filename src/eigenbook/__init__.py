"""Python interface for the Eigen-Book matching engine.

The core package depends on NumPy but does not import Gymnasium. Accessing the
optional ``LimitOrderBookEnv`` export loads :mod:`eigenbook.env` lazily.
"""

from __future__ import annotations

from typing import Any

from . import _eigenbook as _native
from ._eigenbook import (
    BOOK_EVENT_ALIGNMENT,
    BOOK_EVENT_DTYPE,
    TRADE_EVENT_ALIGNMENT,
    TRADE_EVENT_DTYPE,
    BestQuote,
    BookConfig,
    BookEventKind,
    Command,
    CommandOp,
    DispatchResult,
    InstrumentConfig,
    MatchingEngine,
    PriceLevelMode,
    Side,
    Status,
    TimeInForce,
    TopOfBook,
)
from ._version import __version__

if _native.__version__ != __version__:
    raise ImportError(
        "Eigen-Book Python and native module versions differ: "
        f"{__version__!r} != {_native.__version__!r}"
    )

NATIVE_COMPILER: str = _native.__compiler__
NATIVE_BUILD_TYPE: str = _native.__build_type__

__all__ = [
    "BOOK_EVENT_DTYPE",
    "BOOK_EVENT_ALIGNMENT",
    "TRADE_EVENT_DTYPE",
    "TRADE_EVENT_ALIGNMENT",
    "BestQuote",
    "BookConfig",
    "BookEventKind",
    "Command",
    "CommandOp",
    "DispatchResult",
    "InstrumentConfig",
    "LimitOrderBookEnv",
    "MatchingEngine",
    "NATIVE_BUILD_TYPE",
    "NATIVE_COMPILER",
    "PriceLevelMode",
    "Side",
    "Status",
    "TimeInForce",
    "TopOfBook",
    "__version__",
]


def __getattr__(name: str) -> Any:
    if name == "LimitOrderBookEnv":
        from .env import LimitOrderBookEnv

        return LimitOrderBookEnv
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
