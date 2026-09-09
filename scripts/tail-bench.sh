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
# ONE cell, ONE process, ONE book, ONE deterministic stream, ONE measured
# distribution: every cell invokes orderbook_tail_bench EXACTLY ONCE with BOTH
# --stats-out and --samples-out in the same process. The summary is never the
# output of a second, independent run — summary.txt and raw_samples.csv always
# describe the SAME distribution. After the run, verify-tail-summary.sh
# recomputes count/mean/P50/P99/P99.9/max from the already-written raw CSV and
# requires they match the already-written summary (it does NOT rerun the
# benchmark); on any mismatch the canonical runner FAILS LOUDLY.
#
# command.txt records the FULL effective invocation (a Bash array serialized
# with %q quoting — including --stats-out/--samples-out), the resolved seed from
# that run's summary, the UTC time, and host.txt carries machine/tool metadata.
#
# This is a helper for running REAL Phase 4 measurements. It never fabricates a
# result: it only invokes the benchmark, which reports what the machine measured.
#
# Usage:
#   scripts/tail-bench.sh            # canonical matrix
#   scripts/tail-bench.sh map C 1000   # single cell: map, C, 1000 levels
#
# Env overrides (small smoke cells):
#   UPDATES=1025 BATCH=512 scripts/tail-bench.sh map A 1000
#
# updates/batch division: the benchmark TIMES and RECORDS a trailing partial
# batch (updates % batch) with its ACTUAL op count and EXCLUDES it from every
# distribution metric. So the spec-literal canonical default below
# (--updates 10000000 --batch-size 512) runs as written: 19,531 full batches of
# 512 form the distribution, and the final partial of 128 updates is recorded in
# the CSV (partial_rows=1) but kept out of the distribution.

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

    # The ONE effective invocation that produces BOTH artifacts. Bash array:
    # each argument stays a single word (safe quoting), and the same array is
    # serialized verbatim into command.txt below.
    local cmd=(
        ./build-perf/orderbook_tail_bench
        --book "$impl"
        --workload "$wl"
        --levels "$lvl"
        --updates "$UPDATES"
        --batch-size "$BATCH"
        --stats-out "$dir/summary.txt"
        --samples-out "$dir/raw_samples.csv"
    )
    echo "==> $impl $wl $lvl  (updates=$UPDATES batch=$BATCH)"
    # ONE process: summary and raw CSV come from the SAME measured distribution.
    "${cmd[@]}"

    # Provenance: the full effective invocation that generated BOTH artifacts.
    local seed
    seed="$(sed -n 's/^seed=//p' "$dir/summary.txt" | head -n1)"
    {
        printf '# Phase 4 tail benchmark command (effective invocation)\n'
        printf 'command:'
        local a
        for a in "${cmd[@]}"; do printf ' %q' "$a"; done
        printf '\n'
        printf 'time:    %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'updates: %s\n' "$UPDATES"
        printf 'batch:   %s\n' "$BATCH"
        printf 'seed:    default (Phase 2) -> resolved seed=%s (also in summary.txt)\n' \
            "${seed:-<missing>}"
    } > "$dir/command.txt"
    scripts/collect-macos-profile-metadata.sh "$dir/host.txt" >/dev/null 2>&1 || true

    # Post-run verification: recompute the distribution from the ALREADY-WRITTEN
    # raw CSV and require it to match the ALREADY-WRITTEN summary (no rerun).
    # A mismatch fails the whole canonical run loudly.
    if ! scripts/verify-tail-summary.sh "$dir/raw_samples.csv" "$dir/summary.txt"; then
        echo "==> FAILED verification for $tag (summary and raw do not agree)." >&2
        exit 1
    fi
    echo "  -> $dir/ (summary+raw verified from one run)"
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
