#!/usr/bin/env bash
# Deterministic order-book benchmark runner (Experiment 01, Phase 2).
#
# Builds the benchmark (fresh Release dir) and runs the full matrix:
#   2 implementations (map, flat) x 5 workloads (A-E) x 4 scales (1k..1M).
# Output goes to results/bench_<timestamp>.csv (plus a terminal copy).
#
# Usage:
#   scripts/bench.sh [updates] [reps]
#     updates   steady ops per timed block   (default 2000000)
#     reps      timed blocks per cell         (default 3)
#
# The CSV columns (also printed by the benchmark itself):
#   impl,wl,scale_n,updates,best_ms,best_ns_per_update,best_updates_per_s
# Reported time is the BEST (minimum) of `reps` timed blocks per cell.
#
# For profile runs (Phase 3) run ONE implementation per invocation, e.g.:
#   perf stat ./build-bench/orderbook_bench flat all all
# (the benchmark never mixes two book designs in one process).

set -euo pipefail

cd "$(dirname "$0")/.."            # project root
UPDATES="${1:-2000000}"
REPS="${2:-3}"

echo "==> Configuring (Release)"
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release >/dev/null
echo "==> Building"
cmake --build build-bench >/dev/null

mkdir -p results
OUT="results/bench_${UPDATES}up_${REPS}reps_$(date +%Y%m%d-%H%M%S).csv"
echo "==> Running benchmark matrix (impl x workload x scale) -> ${OUT}"
echo "# $(hostname)  $(uname -m)  $(date)"
./build-bench/orderbook_bench both all all updates="${UPDATES}" reps="${REPS}" | tee "${OUT}"
echo "==> done: ${OUT}"
