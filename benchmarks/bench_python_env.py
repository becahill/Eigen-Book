"""Measure Python/native crossing and Gymnasium wrapper costs separately."""

from __future__ import annotations

import argparse
import platform
from pathlib import Path
import subprocess
import time

import gymnasium
import numpy as np

import eigenbook as eb
from eigenbook import _eigenbook as native
from eigenbook.env import LimitOrderBookEnv


def make_instrument() -> eb.InstrumentConfig:
    book_config = eb.BookConfig()
    book_config.min_price = 90
    book_config.max_price = 110
    book_config.max_orders = 1_024
    book_config.order_id_map_capacity = 2_048
    book_config.tick_size = 1
    book_config.event_log_capacity = 1_026
    book_config.price_level_mode = eb.PriceLevelMode.DENSE

    instrument = eb.InstrumentConfig()
    instrument.instrument_id = 101
    instrument.book_config = book_config
    instrument.tick_size = 1
    instrument.lot_size = 1
    return instrument


def make_command(side: eb.Side) -> eb.Command:
    command = eb.Command()
    command.instrument_id = 101
    command.op = eb.CommandOp.ADD
    command.side = side
    command.price = 100
    command.quantity = 1
    command.time_in_force = eb.TimeInForce.GTC
    return command


def processor_name() -> str:
    if platform.system() == "Darwin":
        completed = subprocess.run(
            ["sysctl", "-n", "machdep.cpu.brand_string"],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0 and completed.stdout.strip():
            return completed.stdout.strip()
    if platform.system() == "Linux":
        cpuinfo = Path("/proc/cpuinfo")
        if cpuinfo.exists():
            for line in cpuinfo.read_text(encoding="utf-8").splitlines():
                if line.startswith("model name"):
                    return line.partition(":")[2].strip()
    return platform.processor() or "unknown"


def time_native_round_trip(iterations: int) -> tuple[float, int]:
    checksum = 0
    start = time.perf_counter_ns()
    for index in range(iterations):
        checksum += native._binding_round_trip(index)
    elapsed = time.perf_counter_ns() - start
    return elapsed / iterations, checksum


def time_binding_dispatch(iterations: int, warmup: int) -> tuple[float, int]:
    engine = eb.MatchingEngine([make_instrument()])
    event_buffer = np.empty(
        engine.event_buffer_capacity(101),
        dtype=eb.BOOK_EVENT_DTYPE,
    )
    sell = make_command(eb.Side.SELL)
    buy = make_command(eb.Side.BUY)
    checksum = 0

    for index in range(warmup):
        command = sell if index % 2 == 0 else buy
        command.order_id = index + 1
        command.timestamp = index + 1
        checksum += engine.dispatch_with_buffer(command, event_buffer)

    start = time.perf_counter_ns()
    for index in range(iterations):
        command = sell if index % 2 == 0 else buy
        sequence = warmup + index + 1
        command.order_id = sequence
        command.timestamp = sequence
        checksum += engine.dispatch_with_buffer(command, event_buffer)
    elapsed = time.perf_counter_ns() - start
    return elapsed / iterations, checksum


def time_gym_step(iterations: int, warmup: int) -> tuple[float, int]:
    environment = LimitOrderBookEnv(
        make_instrument(),
        max_price_offset_ticks=10,
        max_order_quantity=1,
        max_episode_steps=None,
    )
    environment.reset(seed=1)
    sell = np.array([1, 10, 0], dtype=np.int64)
    buy = np.array([0, 10, 0], dtype=np.int64)
    checksum = 0

    for index in range(warmup):
        transition = environment.step(sell if index % 2 == 0 else buy)
        checksum += int(transition[4]["event_count"])

    start = time.perf_counter_ns()
    for index in range(iterations):
        transition = environment.step(sell if index % 2 == 0 else buy)
        checksum += int(transition[4]["event_count"])
    elapsed = time.perf_counter_ns() - start
    return elapsed / iterations, checksum


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=100_000)
    parser.add_argument("--warmup", type=int, default=10_000)
    args = parser.parse_args()
    if args.iterations <= 0 or args.warmup < 0:
        parser.error("iterations must be positive and warmup must be non-negative")

    native_round_trip_ns, native_checksum = time_native_round_trip(args.iterations)
    binding_dispatch_ns, binding_checksum = time_binding_dispatch(
        args.iterations,
        args.warmup,
    )
    gym_step_ns, gym_checksum = time_gym_step(args.iterations, args.warmup)

    print(f"platform: {platform.platform()}")
    print(f"machine: {platform.machine()}")
    print(f"processor: {processor_name()}")
    print(f"python: {platform.python_version()}")
    print(f"eigenbook: {eb.__version__}")
    print(f"numpy: {np.__version__}")
    print(f"gymnasium: {gymnasium.__version__}")
    print(f"compiler: {eb.NATIVE_COMPILER}")
    print(f"build_mode: {eb.NATIVE_BUILD_TYPE}")
    print("workload: alternating one-unit midpoint GTC sell/buy orders")
    print(f"warmup_iterations: {args.warmup}")
    print(f"measured_iterations: {args.iterations}")
    print(f"python_native_noop_mean_ns: {native_round_trip_ns:.1f}")
    print(f"binding_dispatch_with_copy_mean_ns: {binding_dispatch_ns:.1f}")
    print(f"gymnasium_step_mean_ns: {gym_step_ns:.1f}")
    print(f"checksum: {native_checksum + binding_checksum + gym_checksum}")
    print(
        "engine_only_latency: not measured here; use the C++ eigenbook_bench "
        "target for engine latency"
    )


if __name__ == "__main__":
    main()
