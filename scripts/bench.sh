#!/usr/bin/env bash
# Deterministic order-book benchmark runner (Experiment 01, Phase 2).
#
# Builds the benchmark (fresh Release dir) and runs the full matrix with ONE
# implementation per process (the canonical methodology):
#   2 implementations (map, flat) x 5 workloads (A-E) x 4 scales (1k..1M),
#   map measured to completion, then flat in its own process — so a long,
#   CPU-saturating run of one design cannot thermally throttle the other.
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
# The benchmark also has a `both` mode (map then flat back-to-back in ONE
# process) — that is only a quick local sanity check and is NOT used here,
# because thermal drift would contaminate whichever implementation runs second.
#
# For per-process profile runs (Phase 3) invoke the binary directly, e.g.:
#   perf stat ./build-bench/orderbook_bench flat all all

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

# Merge the two per-process runs into one CSV: a single header block followed
# by the 40 data rows in map-then-flat order. Each impl runs to its own temp
# file (tee'd to the terminal) so its rows are captured deterministically.
{
    printf '# orderbook_bench - deterministic steady-state apply() throughput (per-process, canonical)\n'
    printf '# domain [1, 2N]; N live levels/side; fill untimed; best of %s reps; %s steady ops per block\n' "$REPS" "$UPDATES"
    printf '# machine: %s  %s  %s\n' "$(hostname)" "$(uname -m)" "$(date)"
    printf '# impl,wl,scale_n,updates,best_ms,best_ns_per_update,best_updates_per_s\n'
} > "${OUT}"

for impl in map flat; do
    TMP="results/.${impl}.tmp"
    echo "==> Running ${impl} in its own process (canonical)"
    ./build-bench/orderbook_bench "${impl}" all all updates="${UPDATES}" reps="${REPS}" \
        | tee "${TMP}"
    grep -E "^${impl}," "${TMP}" >> "${OUT}"
    rm -f "${TMP}"
done

echo "==> done: ${OUT}  ($(grep -c -E '^(map|flat),' "${OUT}") rows)"
