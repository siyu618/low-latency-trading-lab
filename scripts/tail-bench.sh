#!/usr/bin/env bash
# Phase 4 tail-latency benchmark runner (Experiment 01).
#
# Builds the tail benchmark (fresh Release dir) and runs the canonical Phase 4
# matrix with ONE implementation per process:
#   map  x {A-1000, A-1M, C-1M, E-1M}
#   flat x {A-1M, C-1M}
# each a full deterministic distribution run (--updates 10000000 --batch-size
# 512, default Phase 2 seed), capturing the summary + raw CSV into a dated
# results/ dir.
#
# This is a helper for running REAL Phase 4 measurements. It never fabricates a
# result: it only invokes the benchmark, which reports what the machine measured.
#
# Usage:
#   scripts/tail-bench.sh            # canonical matrix
#   scripts/tail-bench.sh map C 1000   # single cell: map, C, 1000 levels
#
# updates/batch division: the benchmark TIMES and RECORDS a trailing partial
# batch (updates % batch) with its ACTUAL op count and EXCLUDES it from the
# percentile distribution. So the spec-literal canonical default below
# (--updates 10000000 --batch-size 512) runs as written: 19,531 full batches of
# 512 form the distribution, and the final partial of 128 updates is recorded in
# the CSV (partial_rows=1) but kept out of the percentiles.

set -euo pipefail
cd "$(dirname "$0")/.."

# Canonical matrix as (impl, workload, levels) triples.
CELLS=(
    "map A 1000"
    "map A 1000000"
    "map C 1000000"
    "map E 1000000"
    "flat A 1000000"
    "flat C 1000000"
)
# Canonical defaults: the spec's suggested 10,000,000 updates / 512 batch.
UPDATES="${UPDATES:-10000000}"
BATCH="${BATCH:-512}"

echo "==> Configuring (Release, fresh build-perf dir — no stale cache/arch flags)"
rm -rf build-perf
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS= >/dev/null
cmake --build build-perf >/dev/null

mkdir -p results
TS="$(date +%Y%m%d-%H%M%S)"
RDIR="results/phase4_${TS}"
mkdir -p "$RDIR"
echo "==> result dir: $RDIR"

run_cell() {
    local impl="$1" wl="$2" lvl="$3"
    local tag="${impl}_${wl}_${lvl}"
    local dir="$RDIR/$tag"
    mkdir -p "$dir"
    local cmd="./build-perf/orderbook_tail_bench --book $impl --workload $wl --levels $lvl --updates $UPDATES --batch-size $BATCH"
    echo "==> $impl $wl $lvl  (updates=$UPDATES batch=$BATCH)"
    # Summary -> file + terminal; raw CSV -> file.
    $cmd --stats-out "$dir/summary.txt"
    $cmd --samples-out "$dir/raw_samples.csv" >/dev/null
    # provenance
    {
        echo "# Phase 4 tail benchmark command"
        echo "command: $cmd"
        echo "time:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "updates: $UPDATES"
        echo "batch:   $BATCH"
        echo "seed:    default (Phase 2)"
    } > "$dir/command.txt"
    scripts/collect-macos-profile-metadata.sh "$dir/host.txt" >/dev/null 2>&1 || true
    echo "  -> $dir/"
}

if [[ "$#" -eq 3 ]]; then
    run_cell "$1" "$2" "$3"
else
    for cell in "${CELLS[@]}"; do
        # shellcheck disable=SC2086
        run_cell $cell
    done
fi
echo "==> done: $RDIR"
