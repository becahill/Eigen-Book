#!/bin/sh

set -eu

if [ "$#" -gt 3 ]; then
    echo "usage: $0 [build-directory] [operations-per-workload] [iterations]" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$repo_root/build-benchmark-release"}
operations=${2:-50000}
iterations=${3:-1}

case "$build_dir" in
    /*) ;;
    *) build_dir="$repo_root/$build_dir" ;;
esac

if [ -e "$build_dir" ]; then
    echo "error: benchmark build directory already exists: $build_dir" >&2
    echo "use a new path so the workflow starts from a clean Release build" >&2
    exit 2
fi

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
    -DEIGENBOOK_BENCHMARK_ITERATIONS="$iterations"

cmake --build "$build_dir" --parallel --target run_benchmarks
