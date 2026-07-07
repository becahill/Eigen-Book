#!/bin/sh

set -eu

if [ "$#" -gt 4 ]; then
    echo "usage: $0 [build-directory] [operations-per-workload] [iterations] [text|json]" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$repo_root/build-benchmark-release"}
operations=${2:-50000}
iterations=${3:-1}
format=${4:-text}

case "$format" in
    text|json) ;;
    *)
        echo "error: benchmark format must be text or json" >&2
        exit 2
        ;;
esac

case "$build_dir" in
    /*) ;;
    *) build_dir="$repo_root/$build_dir" ;;
esac

if [ -e "$build_dir" ]; then
    echo "error: benchmark build directory already exists: $build_dir" >&2
    echo "use a new path so the workflow starts from a clean Release build" >&2
    exit 2
fi

configure_benchmark() {
    cmake \
        -S "$repo_root" \
        -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DEIGENBOOK_BUILD_TESTS=OFF \
        -DEIGENBOOK_BUILD_EXAMPLES=OFF \
        -DEIGENBOOK_BUILD_FUZZERS=OFF \
        -DEIGENBOOK_BUILD_PYTHON=OFF \
        -DEIGENBOOK_BUILD_BENCHMARKS=ON \
        -DEIGENBOOK_BENCHMARK_OPERATIONS="$operations" \
        -DEIGENBOOK_BENCHMARK_ITERATIONS="$iterations" \
        -DEIGENBOOK_BENCHMARK_FORMAT="$format"
}

if [ "$format" = "text" ]; then
    configure_benchmark
    cmake --build "$build_dir" --parallel --target run_benchmarks
else
    configure_benchmark >&2
    cmake --build "$build_dir" --parallel --target eigenbook_bench >&2
    benchmark_exe="$build_dir/eigenbook_bench"
    if [ ! -x "$benchmark_exe" ]; then
        benchmark_exe=$(find "$build_dir" -type f -name eigenbook_bench -print | head -n 1)
    fi
    if [ ! -x "$benchmark_exe" ]; then
        echo "error: could not locate built eigenbook_bench executable" >&2
        exit 1
    fi
    "$benchmark_exe" \
        --operations "$operations" \
        --iterations "$iterations" \
        --format "$format"
fi
