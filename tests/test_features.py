from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
import pytest

from eigenbook.features import FEATURE_NAMES, FeatureExtractor


@dataclass(frozen=True, slots=True)
class OverlayLevel:
    side_index: int
    price: int
    size: int


@dataclass(frozen=True, slots=True)
class OverlayBatch:
    kind: str
    levels: tuple[OverlayLevel, ...]


@dataclass(frozen=True, slots=True)
class OverlayFill:
    side_index: int
    quantity: int


def base_snapshot() -> dict[str, object]:
    return {
        "type": "snapshot",
        "bids": {100: 10, 99: 7},
        "asks": {102: 20, 103: 8},
    }


def test_snapshot_microprice_spread_known_depth_and_state_contract() -> None:
    extractor = FeatureExtractor(tick_size=1, spread_scale_ticks=8)
    extractor.update(
        {
            "type": "snapshot",
            "bids": {
                105: 10,
                104: 20,
                103: 30,
                102: 40,
                101: 50,
                100: 600,
            },
            "asks": {
                107: 30,
                108: 40,
                109: 50,
                110: 60,
                111: 70,
                112: 800,
            },
        }
    )

    state = extractor.get_state()
    assert state is extractor.get_state()
    assert state.shape == (len(FEATURE_NAMES),)
    assert state.dtype == np.float32
    assert state.flags.c_contiguous
    assert state.flags.writeable
    assert np.isfinite(state).all()
    assert np.all(state >= -1.0)
    assert np.all(state <= 1.0)

    # L1 pressure is (10 - 30) / (10 + 30) = -0.5.  Cross-weighting
    # therefore moves the midpoint 106 down to a microprice of 105.5.
    assert extractor.microprice == pytest.approx(105.5)
    assert extractor.spread == pytest.approx(2.0)
    assert state[0] == 0.0
    assert state[1] == pytest.approx(-0.5)
    assert state[2] == 0.0
    assert state[3] == pytest.approx(math.tanh(2.0 / 8.0))

    # The huge sixth levels must not enter the bounded known-depth imbalance.
    # Top-five bid depth=150, ask depth=250, hence imbalance=-0.25.
    assert extractor.depth_imbalance == pytest.approx(-0.25)
    assert state[4] == pytest.approx(-0.25)
    assert state[5] == 0.0


@pytest.mark.parametrize(
    ("update", "expected_ofi", "expected_depth_scale"),
    (
        ({"side": "bid", "price": 100, "size": 15}, 5.0, 16.25),
        ({"side": "bid", "price": 101, "size": 6}, 6.0, 14.0),
        ({"side": "bid", "price": 100, "size": 0}, -10.0, 14.25),
        ({"side": "ask", "price": 102, "size": 25}, -5.0, 16.25),
        ({"side": "ask", "price": 101, "size": 6}, -6.0, 11.5),
        ({"side": "ask", "price": 102, "size": 0}, 20.0, 12.0),
    ),
)
def test_all_cks_ofi_price_and_size_branches(
    update: dict[str, object],
    expected_ofi: float,
    expected_depth_scale: float,
) -> None:
    extractor = FeatureExtractor(window_size=1)
    extractor.update(base_snapshot())
    extractor.update({"type": "update", **update})

    assert extractor.last_ofi == pytest.approx(expected_ofi)
    assert extractor.order_flow_imbalance == pytest.approx(expected_ofi)
    assert extractor.get_state()[0] == pytest.approx(
        math.tanh(expected_ofi / expected_depth_scale)
    )


def test_atomic_change_order_does_not_change_features() -> None:
    first = FeatureExtractor(window_size=4)
    second = FeatureExtractor(window_size=4)
    first.update(base_snapshot())
    second.update(base_snapshot())

    changes = (
        {"side": "bid", "price": 100, "size": 14},
        {"side": "ask", "price": 102, "size": 12},
    )
    first.update({"type": "update", "changes": changes})
    second.update({"type": "update", "changes": tuple(reversed(changes))})

    assert first.last_ofi == second.last_ofi
    np.testing.assert_array_equal(first.get_state(), second.get_state())


def test_resnapshot_and_one_sided_transitions_rebase_flow() -> None:
    extractor = FeatureExtractor(window_size=8)
    extractor.update(base_snapshot())
    extractor.update({"type": "update", "side": "bid", "price": 100, "size": 15})
    assert extractor.order_flow_imbalance == 5.0
    assert extractor.get_state()[2] > 0.0

    extractor.update(
        {
            "type": "snapshot",
            "bids": {200: 9},
            "asks": {202: 11},
        }
    )
    assert extractor.last_ofi == 0.0
    assert extractor.order_flow_imbalance == 0.0
    assert extractor.get_state()[0] == 0.0
    assert extractor.get_state()[2] == 0.0

    extractor.update(
        {
            "type": "update",
            "asks": {202: 0},
        }
    )
    assert extractor.microprice is None
    assert extractor.spread is None
    assert extractor.order_flow_imbalance == 0.0
    assert extractor.get_state()[1] == 0.0
    assert extractor.get_state()[2] == 0.0
    assert extractor.get_state()[3] == 0.0
    assert extractor.get_state()[4] == 1.0

    # Re-establishing a two-sided book seeds a new baseline; it is not flow.
    extractor.update({"type": "update", "side": "ask", "price": 202, "size": 11})
    assert extractor.last_ofi == 0.0
    assert extractor.order_flow_imbalance == 0.0


def test_trade_flow_can_adapt_resting_order_side_and_expires_causally() -> None:
    extractor = FeatureExtractor(window_size=2, trade_side_is_aggressor=False)
    extractor.update(
        base_snapshot(),
        {"side": "BUY", "quantity": 4},
    )
    # A fill of a resting BUY order was caused by a SELL aggressor.
    assert extractor.get_state()[5] == -1.0

    extractor.update(
        {"type": "update", "changes": []},
        {"trade": {"aggressor_side": "buy", "quantity": 2}},
    )
    assert extractor.get_state()[5] == pytest.approx(-2.0 / 6.0)

    # Advancing one more depth event evicts the snapshot's sell-aggressor trade.
    extractor.update({"type": "update", "changes": []})
    assert extractor.get_state()[5] == 1.0
    extractor.update({"type": "update", "changes": []})
    assert extractor.get_state()[5] == 0.0


def test_generic_trade_side_is_aggressor_by_default() -> None:
    extractor = FeatureExtractor(window_size=2)
    extractor.update(base_snapshot(), {"side": "BUY", "quantity": 4})

    assert extractor.get_state()[5] == 1.0


def test_ofi_window_expires_without_get_state_side_effects() -> None:
    extractor = FeatureExtractor(window_size=2)
    extractor.update(base_snapshot())
    extractor.update({"type": "update", "side": "bid", "price": 100, "size": 15})
    before = extractor.order_flow_imbalance
    first_state = extractor.get_state().copy()
    np.testing.assert_array_equal(extractor.get_state(), first_state)
    assert extractor.order_flow_imbalance == before

    extractor.update({"type": "update", "changes": []})
    assert extractor.order_flow_imbalance == 5.0
    extractor.update({"type": "update", "changes": []})
    assert extractor.order_flow_imbalance == 0.0


def test_known_depth_cache_refills_after_deletion() -> None:
    extractor = FeatureExtractor()
    extractor.update(
        {
            "type": "snapshot",
            "bids": {105: 1, 104: 2, 103: 3, 102: 4, 101: 5, 100: 60},
            "asks": {107: 1, 108: 2, 109: 3, 110: 4, 111: 5, 112: 60},
        }
    )
    assert extractor.depth_imbalance == 0.0

    # Deleting the fifth bid must promote the sixth bid into the bounded sum.
    extractor.update({"type": "update", "side": "bid", "price": 101, "size": 0})
    assert extractor.depth_imbalance == pytest.approx((70.0 - 15.0) / 85.0)


def test_crossed_incremental_batch_rolls_back_without_advancing_state() -> None:
    extractor = FeatureExtractor(window_size=1)
    extractor.update(base_snapshot())
    state_before = extractor.get_state().copy()

    with pytest.raises(ValueError, match="locked/crossed"):
        extractor.update({"type": "update", "side": "bid", "price": 102, "size": 99})

    np.testing.assert_array_equal(extractor.get_state(), state_before)
    assert extractor.order_flow_imbalance == 0.0

    # The rejected level must not remain in either the dictionary or top cache.
    extractor.update({"type": "update", "side": "bid", "price": 100, "size": 15})
    assert extractor.last_ofi == 5.0


def test_reset_clears_book_and_state() -> None:
    extractor = FeatureExtractor()
    extractor.update(base_snapshot())
    extractor.update(
        {"type": "update", "side": "bid", "price": 100, "size": 15},
        {"aggressor_side": "sell", "size": 0.25},
    )
    extractor.reset()

    np.testing.assert_array_equal(
        extractor.get_state(),
        np.zeros(len(FEATURE_NAMES), dtype=np.float32),
    )
    assert extractor.last_ofi == 0.0
    assert extractor.order_flow_imbalance == 0.0
    assert extractor.microprice is None
    assert extractor.spread is None
    assert extractor.depth_imbalance == 0.0


def test_overlay_dataclass_adapter_and_resting_fill_side() -> None:
    extractor = FeatureExtractor(window_size=2, trade_side_is_aggressor=False)
    extractor.update_overlay(
        OverlayBatch(
            kind="snapshot",
            levels=(
                OverlayLevel(0, 100, 10),
                OverlayLevel(0, 99, 7),
                OverlayLevel(1, 102, 20),
                OverlayLevel(1, 103, 8),
            ),
        )
    )
    extractor.update_overlay(
        OverlayBatch(
            kind="update",
            levels=(OverlayLevel(0, 100, 15),),
        ),
        OverlayFill(side_index=0, quantity=3),
    )

    assert extractor.last_ofi == 5.0
    # A resting bid fill implies a sell aggressor.
    assert extractor.get_state()[5] == -1.0


def test_update_before_initial_snapshot_and_float_prices_are_rejected() -> None:
    extractor = FeatureExtractor()
    with pytest.raises(RuntimeError, match="initial depth snapshot"):
        extractor.update({"type": "update", "side": "bid", "price": 100, "size": 1})
    with pytest.raises(TypeError, match="fixed-point integer"):
        extractor.update(
            {
                "type": "snapshot",
                "bids": {100.0: 1},
                "asks": {101: 1},
            }
        )


def test_one_sided_rebase_preserves_trade_window() -> None:
    extractor = FeatureExtractor(window_size=3)
    extractor.update(base_snapshot())
    extractor.update(
        {"type": "update", "side": "ask", "price": 102, "size": 0},
    )
    extractor.update(
        {"type": "update", "side": "ask", "price": 103, "size": 0},
        {"aggressor_side": "buy", "quantity": 2},
    )
    extractor.update(
        {"type": "update", "side": "bid", "price": 100, "size": 11},
        {"aggressor_side": "sell", "quantity": 1},
    )

    assert extractor.order_flow_imbalance == 0.0
    assert extractor.get_state()[5] == pytest.approx(1.0 / 3.0)


def test_trade_only_update_does_not_age_ofi_window() -> None:
    extractor = FeatureExtractor(window_size=2)
    extractor.update(base_snapshot())
    extractor.update({"type": "update", "side": "bid", "price": 100, "size": 15})
    ofi_state = float(extractor.get_state()[0])

    extractor.update_trades({"aggressor_side": "buy", "quantity": 3})

    assert extractor.order_flow_imbalance == 5.0
    assert extractor.get_state()[0] == ofi_state
    assert extractor.get_state()[5] == 1.0
