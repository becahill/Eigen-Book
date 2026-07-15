from __future__ import annotations

import csv
from dataclasses import replace
from pathlib import Path
import sys

import numpy as np
import pytest

try:
    from gymnasium.utils.env_checker import check_env
    from stable_baselines3 import PPO

    from eigenbook.features import FeatureExtractor
    from eigenbook.market_data import CSV_COLUMNS, SCHEMA_VERSION, MarketDataError
    from eigenbook.model_artifact import (
        ModelCompatibilityError,
        load_model_artifact,
        save_model_artifact,
    )
    from eigenbook.observation import (
        OBSERVATION_NAMES,
        OBSERVATION_SCHEMA_VERSION,
        OBSERVATION_SIZE,
    )
    from eigenbook.replay import BUY_SIDE, FILL_MODEL_VERSION, ReplayConfig, SELL_SIDE
except ModuleNotFoundError as error:
    if error.name not in {"gymnasium", "stable_baselines3", "torch"}:
        raise
    pytest.skip("requires the eigenbook[training] extra", allow_module_level=True)


EXPERIMENTS = Path(__file__).resolve().parents[1] / "experiments"
sys.path.insert(0, str(EXPERIMENTS))
import paper_trader  # noqa: E402
import train_market_data as training  # noqa: E402


pytestmark = pytest.mark.training
FIXTURES = Path(__file__).parent / "fixtures" / "market_data"


def row(
    *,
    event_id: int,
    kind: str,
    side: str = "",
    price: str = "",
    size: str = "",
    first_update_id: str = "",
    last_update_id: str = "",
    previous_update_id: str = "",
    trade_id: str = "",
    first_trade_id: str = "",
    last_trade_id: str = "",
    aggressor_side: str = "",
    event_time: int | None = None,
    venue: str = "binance_spot",
    symbol: str = "BTCUSDT",
    price_scale: int = 100,
    quantity_scale: int = 1_000,
    data_mode: str = "depth_trades",
) -> list[str]:
    return [
        SCHEMA_VERSION,
        data_mode,
        venue,
        symbol,
        str(price_scale),
        str(quantity_scale),
        str(event_id),
        str(event_time if event_time is not None else 1_700_000_000_000 + event_id),
        kind,
        first_update_id,
        last_update_id,
        previous_update_id,
        trade_id,
        first_trade_id,
        last_trade_id,
        aggressor_side,
        side,
        price,
        size,
    ]


def write_tape(path: Path, rows: list[list[str]]) -> Path:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(CSV_COLUMNS)
        writer.writerows(rows)
    return path


def snapshot_rows(
    *,
    event_id: int = 1,
    update_id: int = 100,
    bid: tuple[str, str] = ("100.00", "0.005"),
    ask: tuple[str, str] = ("101.00", "0.010"),
) -> list[list[str]]:
    common = {
        "event_id": event_id,
        "kind": "snapshot",
        "first_update_id": str(update_id),
        "last_update_id": str(update_id),
    }
    return [
        row(side="bid", price=bid[0], size=bid[1], **common),
        row(side="ask", price=ask[0], size=ask[1], **common),
    ]


def depth_row(
    event_id: int,
    update_id: int,
    *,
    side: str = "bid",
    price: str = "100.00",
    size: str = "0.005",
) -> list[str]:
    return row(
        event_id=event_id,
        kind="depth_update",
        first_update_id=str(update_id),
        last_update_id=str(update_id),
        previous_update_id=str(update_id - 1),
        side=side,
        price=price,
        size=size,
    )


def trade_row(
    event_id: int,
    trade_id: int,
    *,
    aggressor_side: str,
    price: str,
    size: str,
    first_trade_id: int | None = None,
    last_trade_id: int | None = None,
) -> list[str]:
    first_raw = trade_id if first_trade_id is None else first_trade_id
    last_raw = first_raw if last_trade_id is None else last_trade_id
    return row(
        event_id=event_id,
        kind="trade",
        trade_id=str(trade_id),
        first_trade_id=str(first_raw),
        last_trade_id=str(last_raw),
        aggressor_side=aggressor_side,
        price=price,
        size=size,
    )


def config(**changes: object) -> ReplayConfig:
    defaults: dict[str, object] = {
        "symbol": "BTCUSDT",
        "venue": "binance_spot",
        "price_scale": 100,
        "quantity_scale": 1_000,
        "own_order_min_price": 0,
        "own_order_max_price": 20_000,
        "tick_size": 1,
        "lot_size": 1,
        "max_quote_distance_ticks": 10,
        "max_order_quantity_lots": 20,
        "max_abs_inventory_lots": 1_000,
        "events_per_action": 1,
        "order_entry_latency_events": 0,
        "maker_fee_rate": 0.001,
        "taker_fee_rate": 0.001,
        "liquidate_on_termination": False,
    }
    defaults.update(changes)
    return ReplayConfig(**defaults)  # type: ignore[arg-type]


def many_event_tape(path: Path, count: int = 40) -> Path:
    rows = snapshot_rows()
    for index in range(2, count + 2):
        rows.append(depth_row(index, 99 + index, size="0.005"))
    return write_tape(path, rows)


def test_bounded_fixture_replays_atomically_and_deterministically() -> None:
    path = FIXTURES / "valid_atomic.csv"
    replay_config = config(
        events_per_action=1,
        order_entry_latency_events=1,
    )
    first = training.create_environment(path, config=replay_config)
    second = training.create_environment(path, config=replay_config)
    try:
        first_observation, first_info = first.reset(seed=19)
        second_observation, second_info = second.reset(seed=19)
        np.testing.assert_array_equal(first_observation, second_observation)
        assert first_info == second_info
        assert first_observation.shape == (OBSERVATION_SIZE,)
        assert first.observation_space.contains(first_observation)
        assert first_info["source_event_id"] == 2

        action = np.asarray([BUY_SIDE, 1, 2], dtype=np.int64)
        first_transition = first.step(action)
        second_transition = second.step(action)
        np.testing.assert_array_equal(first_transition[0], second_transition[0])
        assert first_transition[1:] == second_transition[1:]
        assert first_transition[4]["feed_event_count"] == 1
        assert first_transition[4]["source_event_id"] == 3
        assert first_transition[4]["feature_event_count"] == 3
    finally:
        first.close()
        second.close()


def test_cancellation_only_market_move_cannot_fill_agent_order(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    # One atomic quote change removes both old sides and installs an uncrossed
    # lower market. It contains no aggressive trade evidence.
    rows.extend(
        [
            depth_row(3, 102, side="bid", price="100.00", size="0"),
            depth_row(3, 102, side="ask", price="101.00", size="0"),
            depth_row(3, 102, side="bid", price="98.00", size="0.005"),
            depth_row(3, 102, side="ask", price="99.00", size="0.005"),
        ]
    )
    path = write_tape(tmp_path / "cancellation-only.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        env.reset(seed=1)
        _, reward, terminated, truncated, info = env.step(
            np.asarray([BUY_SIDE, 0, 9], dtype=np.int64)
        )
        assert info["fill_count"] == 0
        assert info["filled_quantity"] == 0
        assert info["inventory"] == 0
        assert info["cash"] == 0.0
        assert reward == 0.0
        assert not terminated and not truncated
        assert env.unwrapped.own_orders.active_order(BUY_SIDE) is None
        assert info["uncertain_cross_cancelled_order_count"] == 1
        assert info["uncertain_cross_cancelled_order_ids"] == (1,)
        assert info["action_status"] == "CANCELLED_UNCONFIRMED_CROSS"
        assert info["fill_model"] == FILL_MODEL_VERSION
        assert info["execution_quality"] == "approximate_price_level_queue"
    finally:
        env.close()


def test_trade_volume_consumes_queue_then_partially_fills_with_fee(
    tmp_path: Path,
) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows.append(
        trade_row(
            3,
            900,
            aggressor_side="sell",
            price="100.00",
            size="0.003",
        )
    )
    rows.append(
        trade_row(
            4,
            901,
            aggressor_side="sell",
            price="100.00",
            size="0.006",
        )
    )
    rows.append(depth_row(5, 102))
    path = write_tape(tmp_path / "partial.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        env.reset(seed=2)
        first_observation, first_reward, _, _, first_info = env.step(
            np.asarray([BUY_SIDE, 0, 9], dtype=np.int64)
        )
        assert first_info["fill_count"] == 0
        assert first_info["inventory"] == 0
        assert first_reward == 0.0
        bid = env.unwrapped.own_orders.active_order(BUY_SIDE)
        assert bid is not None
        assert bid.queue_ahead == 2
        assert bid.quantity == 10
        queue_index = OBSERVATION_NAMES.index("active_bid_queue_ahead")
        assert first_observation[queue_index] == pytest.approx(np.tanh(2 / 20))

        second_observation, reward, terminated, truncated, info = env.step(
            np.asarray([SELL_SIDE, 0, 0], dtype=np.int64)
        )
        assert info["fill_count"] == 1
        assert info["filled_quantity"] == 4
        assert info["fills"] == (
            {
                "order_id": 1,
                "side": "buy",
                "price": 10_000,
                "quantity": 4,
                "source_event_id": 4,
                "trade_id": 901,
                "first_trade_id": 901,
                "last_trade_id": 901,
                "reported_trade_quantity": 6,
                "queue_ahead_before": 2,
                "queue_ahead_after": 0,
            },
        )
        assert info["inventory"] == 4
        assert info["step_maker_fee"] == pytest.approx(0.0004)
        assert info["cash"] == pytest.approx(-0.4004)
        assert info["wealth"] == pytest.approx(0.0016)
        # The reward wrapper subtracts the configured inventory penalty.
        assert reward == pytest.approx(0.0016 - 0.01 * (0.004**2))
        remaining = env.unwrapped.own_orders.active_order(BUY_SIDE)
        assert remaining is not None
        assert remaining.quantity == 6
        assert remaining.queue_ahead == 0
        assert second_observation[queue_index] == pytest.approx(0.0)
        assert not terminated and not truncated
    finally:
        env.close()


@pytest.mark.parametrize("trade_time", (150, 200))
def test_trade_not_provably_after_activation_is_ignored(
    tmp_path: Path,
    trade_time: int,
) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows[-1][7] = "200"
    rows.append(
        trade_row(
            3,
            900,
            aggressor_side="sell",
            price="100.00",
            size="0.020",
        )
    )
    rows[-1][7] = str(trade_time)
    path = write_tape(tmp_path / f"activation-time-{trade_time}.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        _, reset_info = env.reset(seed=201)
        assert reset_info["source_time_watermark"] == 200
        _, reward, terminated, truncated, info = env.step(
            np.asarray([BUY_SIDE, 0, 9], dtype=np.int64)
        )

        assert info["ignored_uncertain_trade_evidence_count"] == 1
        assert info["fill_count"] == 0
        assert info["inventory"] == 0
        assert reward == 0.0
        assert not terminated and not truncated
        bid = env.unwrapped.own_orders.active_order(BUY_SIDE)
        assert bid is not None
        assert bid.queue_ahead == 5
        assert bid.activation_event_time == 200
    finally:
        env.close()


def test_regressing_cross_stream_trade_is_not_late_fill_evidence(
    tmp_path: Path,
) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows[-1][7] = "100"
    rows.append(depth_row(3, 102, side="ask", price="101.00", size="0.011"))
    rows[-1][7] = "200"
    rows.append(
        trade_row(
            4,
            900,
            aggressor_side="sell",
            price="100.00",
            size="0.020",
        )
    )
    rows[-1][7] = "150"
    path = write_tape(tmp_path / "regressing-trade.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        env.reset(seed=202)
        env.step(np.asarray([BUY_SIDE, 0, 9], dtype=np.int64))
        bid = env.unwrapped.own_orders.active_order(BUY_SIDE)
        assert bid is not None and bid.activation_event_time == 100

        _, reward, _, _, info = env.step(np.asarray([SELL_SIDE, 0, 0], dtype=np.int64))
        assert info["source_time_watermark"] == 200
        assert info["ignored_uncertain_trade_evidence_count"] == 1
        assert info["fill_count"] == 0
        assert info["inventory"] == 0
        assert reward == 0.0
    finally:
        env.close()


def test_queue_consumption_preserves_activation_time_gate(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows[-1][7] = "100"
    rows.append(
        trade_row(
            3,
            900,
            aggressor_side="sell",
            price="100.00",
            size="0.003",
        )
    )
    rows[-1][7] = "150"
    rows.append(
        trade_row(
            4,
            901,
            aggressor_side="sell",
            price="100.00",
            size="0.020",
        )
    )
    rows[-1][7] = "100"
    path = write_tape(tmp_path / "queue-activation-time.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        env.reset(seed=203)
        env.step(np.asarray([BUY_SIDE, 0, 9], dtype=np.int64))
        bid = env.unwrapped.own_orders.active_order(BUY_SIDE)
        assert bid is not None
        assert bid.queue_ahead == 2
        assert bid.activation_event_time == 100

        _, _, _, _, info = env.step(np.asarray([SELL_SIDE, 0, 0], dtype=np.int64))
        assert info["ignored_uncertain_trade_evidence_count"] == 1
        assert info["fill_count"] == 0
        assert info["inventory"] == 0
    finally:
        env.close()


def test_wrong_price_or_aggressor_trade_produces_no_fill(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows.extend(
        [
            trade_row(
                3,
                900,
                aggressor_side="buy",
                price="100.00",
                size="0.020",
            ),
            trade_row(
                4,
                901,
                aggressor_side="sell",
                price="99.00",
                size="0.020",
            ),
        ]
    )
    path = write_tape(tmp_path / "wrong-evidence.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        env.reset(seed=3)
        for side in (BUY_SIDE, SELL_SIDE):
            _, _, _, _, info = env.step(np.asarray([side, 0, 4], dtype=np.int64))
            assert info["fill_count"] == 0
        assert env.unwrapped.inventory == 0
    finally:
        env.close()


def test_quote_exposure_cannot_exceed_inventory_limit(tmp_path: Path) -> None:
    path = many_event_tape(tmp_path / "risk-limit.csv", count=3)
    env = training.create_environment(
        path,
        config=config(max_abs_inventory_lots=5),
    )
    try:
        env.reset(seed=31)
        _, _, terminated, truncated, info = env.step(
            np.asarray([BUY_SIDE, 0, 9], dtype=np.int64)
        )
        assert info["action_status"] == "REJECTED_INVENTORY_LIMIT"
        assert info["rejected_order_count"] == 1
        assert info["active_order_count"] == 0
        assert info["inventory"] == 0
        assert not terminated and not truncated
    finally:
        env.close()


class TrackingFeatures(FeatureExtractor):
    def __init__(self) -> None:
        super().__init__(
            window_size=8,
            tick_size=1,
            spread_scale_ticks=8,
            trade_side_is_aggressor=True,
        )
        self.calls: list[str] = []

    def update(self, update, trade_events=None) -> None:  # type: ignore[no-untyped-def]
        self.calls.append(str(update["type"]))
        super().update(update, trade_events)

    def update_trades(self, trade_events) -> None:  # type: ignore[no-untyped-def]
        self.calls.append("trade")
        super().update_trades(trade_events)


def test_every_atomic_event_reaches_feature_extractor_once(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.extend(
        [
            depth_row(2, 101),
            trade_row(
                3,
                900,
                aggressor_side="buy",
                price="101.00",
                size="0.001",
            ),
        ]
    )
    rows.extend(snapshot_rows(event_id=4, update_id=500))
    rows.append(depth_row(5, 501))
    path = write_tape(tmp_path / "feature-events.csv", rows)
    tracker = TrackingFeatures()
    base = training.SequencedMarketEnv(
        path,
        config=config(feature_window_size=8),
        feature_extractor=tracker,
    )
    env = training.CanonicalObservationWrapper(training.MarketMakerRewardWrapper(base))
    try:
        env.reset(seed=4)
        assert tracker.calls == ["snapshot", "update"]
        for side in (BUY_SIDE, SELL_SIDE):
            env.step(np.asarray([side, 1, 0], dtype=np.int64))
        assert tracker.calls == ["snapshot", "update", "trade", "snapshot", "update"]
        assert base.feature_event_count == 5
        assert base.market.resnapshot_count == 1
    finally:
        env.close()


def test_injected_feature_configuration_must_match_metadata(
    tmp_path: Path,
) -> None:
    path = write_tape(tmp_path / "feature-mismatch.csv", snapshot_rows())
    mismatched = TrackingFeatures()

    with pytest.raises(ValueError, match="does not match ReplayConfig"):
        training.SequencedMarketEnv(
            path,
            config=config(feature_window_size=32),
            feature_extractor=mismatched,
        )


def test_resnapshot_cancels_unknown_queue_state_without_fill(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows.extend(snapshot_rows(event_id=3, update_id=500, bid=("99.00", "0.005")))
    rows.append(depth_row(4, 501, price="99.00"))
    path = write_tape(tmp_path / "resnapshot.csv", rows)
    env = training.create_environment(path, config=config())
    try:
        env.reset(seed=5)
        _, _, _, _, info = env.step(np.asarray([BUY_SIDE, 0, 4], dtype=np.int64))
        assert info["fill_count"] == 0
        assert info["resnapshot_cancelled_order_count"] == 1
        assert info["action_status"] == "CANCELLED_RESNAPSHOT"
        assert info["source_event_id"] == 4
        assert info["feed_event_count"] == 2
        assert info["feed_synchronized"] is True
        assert env.unwrapped.own_orders.active_order(BUY_SIDE) is None
        assert env.unwrapped.market.last_update_id == 501
    finally:
        env.close()


def test_resnapshot_recovery_cancels_pending_latency_without_activation(
    tmp_path: Path,
) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows.extend(snapshot_rows(event_id=3, update_id=500))
    rows.append(depth_row(4, 501))
    path = write_tape(tmp_path / "pending-resnapshot.csv", rows)
    env = training.create_environment(
        path,
        config=config(order_entry_latency_events=1),
    )
    try:
        _, reset_info = env.reset(seed=204)
        assert reset_info["source_event_id"] == 2
        assert reset_info["feed_event_count"] == 0

        _, _, _, _, info = env.step(np.asarray([BUY_SIDE, 0, 0], dtype=np.int64))
        assert info["action_status"] == "CANCELLED_RESNAPSHOT"
        assert info["activated_order_count"] == 0
        assert info["active_order_count"] == 0
        assert info["pending_order_count"] == 0
        assert info["resnapshot_cancelled_order_count"] == 1
        assert info["source_event_id"] == 4
        assert info["feature_event_count"] == 4
    finally:
        env.close()


def test_sequence_gap_marks_feed_stale_and_requires_reset(tmp_path: Path) -> None:
    path = FIXTURES / "sequence_gap.csv"
    gap_config = config(quantity_scale=1_000, data_mode="depth")
    base = training.SequencedMarketEnv(path, config=gap_config)
    with pytest.raises(MarketDataError, match="depth continuity gap"):
        base.reset(seed=6)
    assert base.feed_synchronized is False
    assert base.own_orders.active_orders == {}
    with pytest.raises(RuntimeError, match=r"reset\(\) must be called"):
        base.step(np.asarray([BUY_SIDE, 0, 0], dtype=np.int64))
    base.close()


def test_crossed_atomic_event_cancels_orders_and_marks_feed_stale() -> None:
    base = training.SequencedMarketEnv(
        FIXTURES / "crossed.csv",
        config=config(data_mode="depth", quantity_scale=1_000),
    )
    with pytest.raises(MarketDataError, match="crossed/locked"):
        base.reset(seed=61)

    assert base.feed_synchronized is False
    assert base.own_orders.active_orders == {}
    with pytest.raises(RuntimeError, match=r"reset\(\) must be called"):
        base.step(np.asarray([BUY_SIDE, 0, 0], dtype=np.int64))
    base.close()


def test_failed_rewind_cannot_leave_stale_synchronized_state() -> None:
    base = training.SequencedMarketEnv(
        FIXTURES / "valid_atomic.csv",
        config=config(),
    )
    base.reset(seed=62)
    assert base.feed_synchronized is True

    base.path = FIXTURES / "resnapshot.csv"
    with pytest.raises(MarketDataError, match="tape identity"):
        base.reset(seed=63, options={"rewind_market_data": True})
    assert base.feed_synchronized is False
    with pytest.raises(RuntimeError, match=r"reset\(\) must be called"):
        base.step(np.asarray([BUY_SIDE, 0, 0], dtype=np.int64))
    base.close()


def test_rewind_rebuilds_source_time_watermark(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows[-1][7] = "100"
    rows.append(
        trade_row(
            3,
            900,
            aggressor_side="sell",
            price="100.00",
            size="0.020",
        )
    )
    rows[-1][7] = "150"
    path = write_tape(tmp_path / "rewind-watermark.csv", rows)
    env = training.create_environment(
        path,
        config=config(max_episode_steps=1),
    )
    try:
        env.reset(seed=205)
        _, _, terminated, _, first_info = env.step(
            np.asarray([SELL_SIDE, 0, 0], dtype=np.int64)
        )
        assert terminated
        assert first_info["source_time_watermark"] == 150

        _, reset_info = env.reset(
            seed=206,
            options={"rewind_market_data": True},
        )
        assert reset_info["source_time_watermark"] == 100
        _, _, terminated, _, second_info = env.step(
            np.asarray([BUY_SIDE, 0, 9], dtype=np.int64)
        )
        assert terminated
        assert second_info["fill_count"] == 1
        assert second_info["filled_quantity"] == 10
        assert second_info["ignored_uncertain_trade_evidence_count"] == 0
        assert second_info["action_status"] == "FILLED"
    finally:
        env.close()


def test_terminal_liquidation_reports_partial_result_without_raising(
    tmp_path: Path,
) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows.append(
        trade_row(
            3,
            900,
            aggressor_side="sell",
            price="100.00",
            size="0.015",
        )
    )
    path = write_tape(tmp_path / "partial-liquidation.csv", rows)
    env = training.create_environment(
        path,
        config=config(
            max_episode_steps=1,
            liquidate_on_termination=True,
        ),
    )
    try:
        env.reset(seed=207)
        _, reward, terminated, truncated, info = env.step(
            np.asarray([BUY_SIDE, 0, 9], dtype=np.int64)
        )
        assert terminated and not truncated
        assert info["action_status"] == "FILLED"
        assert info["pre_liquidation_inventory"] == 10
        assert info["inventory"] == 5
        assert info["cumulative_maker_fee"] == pytest.approx(0.001)
        assert info["cumulative_taker_fee"] == pytest.approx(0.0005)
        assert info["cash"] == pytest.approx(-0.5015)
        assert info["wealth"] == pytest.approx(0.001)
        assert reward == pytest.approx(0.001 - 0.01 * (0.01**2))
        assert info["terminal_liquidation"] == {
            "side": "sell",
            "requested_quantity": 10,
            "executed_quantity": 5,
            "remaining_quantity": 5,
            "complete": False,
            "notional": pytest.approx(0.5),
            "fee": pytest.approx(0.0005),
            "legs": ({"price": 10_000, "quantity": 5},),
        }
        with pytest.raises(RuntimeError, match="episode has ended"):
            env.step(np.asarray([SELL_SIDE, 0, 0], dtype=np.int64))
    finally:
        env.close()


def test_canonical_observation_schema_and_atomic_latency(tmp_path: Path) -> None:
    rows = snapshot_rows()
    rows.append(depth_row(2, 101))
    rows.extend(
        [
            depth_row(3, 102, side="bid", price="100.00", size="0"),
            depth_row(3, 102, side="bid", price="100.01", size="0.005"),
            depth_row(3, 102, side="ask", price="101.00", size="0"),
            depth_row(3, 102, side="ask", price="101.01", size="0.010"),
        ]
    )
    rows.append(depth_row(4, 103, price="100.01"))
    path = write_tape(tmp_path / "atomic-latency.csv", rows)
    replay_config = config(order_entry_latency_events=1)
    env = training.create_environment(path, config=replay_config)
    try:
        observation, _ = env.reset(seed=7)
        assert observation.shape == (OBSERVATION_SIZE,)
        assert tuple(OBSERVATION_NAMES[:6]) == FeatureExtractor.FEATURE_NAMES
        assert OBSERVATION_SCHEMA_VERSION.endswith(".v2")
        _, _, _, _, info = env.step(np.asarray([BUY_SIDE, 1, 0], dtype=np.int64))
        assert info["feed_event_count"] == 1
        assert info["feature_event_count"] == 3
        assert info["action_status"] == "RESTING"
        assert info["activated_order_count"] == 1
    finally:
        env.close()


def test_latency_state_spans_policy_transitions(tmp_path: Path) -> None:
    path = many_event_tape(tmp_path / "observable-latency.csv", count=8)
    env = training.create_environment(
        path,
        config=config(events_per_action=1, order_entry_latency_events=3),
    )
    pending_present = OBSERVATION_NAMES.index("pending_bid_present")
    pending_latency = OBSERVATION_NAMES.index("pending_bid_latency")
    active_present = OBSERVATION_NAMES.index("active_bid_present")
    try:
        env.reset(seed=71)
        first, _, _, _, first_info = env.step(
            np.asarray([BUY_SIDE, 0, 0], dtype=np.int64)
        )
        assert first_info["action_status"] == "PENDING"
        assert first_info["activated_order_count"] == 0
        assert first[pending_present] == pytest.approx(1.0)
        assert first[pending_latency] == pytest.approx(2.0 / 3.0)
        assert first[active_present] == pytest.approx(0.0)

        second, _, _, _, _ = env.step(np.asarray([SELL_SIDE, 0, 0], dtype=np.int64))
        assert second[pending_latency] == pytest.approx(1.0 / 3.0)

        third, _, _, _, third_info = env.step(
            np.asarray([SELL_SIDE, 1, 0], dtype=np.int64)
        )
        assert third_info["pending_replaced_order_count"] == 1
        assert third[pending_present] == pytest.approx(0.0)
        assert third[pending_latency] == pytest.approx(0.0)
        assert third[active_present] == pytest.approx(1.0)
    finally:
        env.close()


def test_wrapped_environment_passes_gymnasium_checker(tmp_path: Path) -> None:
    path = many_event_tape(tmp_path / "checker.csv")
    env = training.create_environment(path, config=config(order_entry_latency_events=1))
    try:
        check_env(env, skip_render_check=True)
        observation, _ = env.reset(seed=11)
        assert env.observation_space.contains(observation)
    finally:
        env.close()


def test_ppo_smoke_save_reload_and_mismatch_rejection(tmp_path: Path) -> None:
    path = many_event_tape(tmp_path / "ppo.csv")
    replay_config = config(order_entry_latency_events=1)
    env = training.create_environment(path, config=replay_config)
    try:
        model = PPO(
            "MlpPolicy",
            env,
            n_steps=4,
            batch_size=2,
            seed=17,
            device="cpu",
            verbose=0,
        )
        model.learn(total_timesteps=4)
        metadata = training.model_metadata(env, replay_config)
        archive, sidecar = save_model_artifact(model, tmp_path / "agent", metadata)
        assert archive.is_file() and sidecar.is_file()

        compatible = load_model_artifact(
            PPO,
            archive,
            env=env,
            expected_metadata=metadata,
            device="cpu",
        )
        observation, _ = env.reset(seed=17, options={"rewind_market_data": True})
        action, _ = compatible.predict(observation, deterministic=True)
        assert env.action_space.contains(action)

        wrong_symbol = replace(metadata, symbol="ETHUSDT")
        with pytest.raises(ModelCompatibilityError, match="symbol"):
            load_model_artifact(
                PPO,
                archive,
                env=env,
                expected_metadata=wrong_symbol,
                device="cpu",
            )
        wrong_mode = replace(metadata, market_data_mode="depth")
        with pytest.raises(ModelCompatibilityError, match="market_data_mode"):
            load_model_artifact(
                PPO,
                archive,
                env=env,
                expected_metadata=wrong_mode,
                device="cpu",
            )
        wrong_action_space = replace(
            metadata,
            action_space={
                **metadata.action_space,
                "nvec": [2, 99, 99],
            },
        )
        with pytest.raises(ModelCompatibilityError, match="action_space"):
            load_model_artifact(
                PPO,
                archive,
                env=env,
                expected_metadata=wrong_action_space,
                device="cpu",
            )
        legacy_archive = tmp_path / "legacy.zip"
        legacy_archive.write_bytes(archive.read_bytes())
        with pytest.raises(ModelCompatibilityError, match="sidecar is missing"):
            load_model_artifact(
                PPO,
                legacy_archive,
                env=env,
                expected_metadata=metadata,
                device="cpu",
            )
    finally:
        env.close()


def test_policy_contract_rejects_wrong_tape_mode(tmp_path: Path) -> None:
    source = (FIXTURES / "valid_atomic.csv").read_text(encoding="utf-8")
    path = tmp_path / "depth-only.csv"
    path.write_text(source.replace("depth_trades", "depth"), encoding="utf-8")

    # The fixture contains a trade row, so the depth-only declaration is
    # rejected by schema validation before identity comparison.
    with pytest.raises(MarketDataError, match="trade event is prohibited"):
        training.validate_market_data_contract(path, config=config())


def test_short_training_cycle_persists_metadata_and_paper_uses_it(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    path = many_event_tape(tmp_path / "pipeline.csv", count=50)
    monkeypatch.setattr(training, "DEFAULT_PPO_ROLLOUT_STEPS", 4)
    monkeypatch.setattr(training, "DEFAULT_PPO_BATCH_SIZE", 2)
    experiment = training.ExperimentConfig(
        market_data_path=path,
        evaluation_market_data_path=path,
        training_timesteps=4,
        evaluation_steps=2,
        seed=23,
        model_path=tmp_path / "pipeline-agent",
        simulator=config(order_entry_latency_events=1),
    )
    result = training.run_training(experiment, verbosity=0)
    assert result.model_path.is_file()
    assert result.metadata_path.is_file()

    paper_config = paper_trader.replay_config_from_model(
        result.model_path,
        maximum_steps=2,
    )
    assert paper_config.symbol == experiment.simulator.symbol
    assert paper_config.venue == experiment.simulator.venue
    assert paper_config.feature_configuration() == (
        replace(experiment.simulator, max_episode_steps=2).feature_configuration()
    )

    paper_result = paper_trader.run_paper_replay(
        paper_trader.PaperConfig(path, result.model_path, 2, 23)
    )
    assert paper_result["steps"] <= 2
    assert paper_result["venue"] == "binance_spot"
    assert paper_result["symbol"] == "BTCUSDT"


def test_paper_rejects_implicit_cross_venue_transfer(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    training_path = many_event_tape(tmp_path / "training.csv", count=20)
    other_rows = snapshot_rows()
    other_rows.append(depth_row(2, 101))
    for other_row in other_rows:
        other_row[2] = "coinbase_spot"
    other_path = write_tape(tmp_path / "other-venue.csv", other_rows)
    monkeypatch.setattr(training, "DEFAULT_PPO_ROLLOUT_STEPS", 4)
    monkeypatch.setattr(training, "DEFAULT_PPO_BATCH_SIZE", 2)
    experiment = training.ExperimentConfig(
        training_path,
        training_path,
        4,
        1,
        29,
        tmp_path / "venue-agent",
        config(order_entry_latency_events=1),
    )
    result = training.run_training(experiment, verbosity=0)
    with pytest.raises(MarketDataError, match="tape identity"):
        paper_trader.run_paper_replay(
            paper_trader.PaperConfig(other_path, result.model_path, 1, 29)
        )
