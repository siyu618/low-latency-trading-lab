#!/usr/bin/env bash
# Linux perf profiling harness for the Experiment 01 benchmark (Phase 3).
#
# Profiles ONE (impl, workload, scale) cell at a time, in its own process, with
# perf — the same per-process methodology as scripts/bench.sh, so perf counters
# are never attributed to a mixed run. For each cell it runs two passes:
#   1. perf stat   — aggregate hardware counters over the whole timed process
#                    (cycles, instructions, branch/cache misses).
#   2. perf record — a call-graph profile (-g) of where the time goes, saved to
#                    perf.data for `perf report`.
#
# THIS SCRIPT RUNS ONLY ON LINUX with perf installed. It is part of a phase that
# is written to run on a Linux box but is NOT validated by measurements on the
# developing machine (Apple Silicon macOS, no perf). It is intentionally
# DRY-RUN BY DEFAULT so it is safe to inspect here: without --run it only prints
# the exact perf + benchmark commands it would execute. No perf output is ever
# fabricated; --run captures only what a real Linux `perf` reports.
#
# Phase 2 deferred "why" questions this harness is meant to answer (see
# docs/profiling/README.md):
#   * Workload C: map is empirically cheap (~71 ns at 1M) — near-touch locality,
#     branch predictability, or tree-path locality? Profile map C at 1M with
#     cache-misses + branch-misses and compare against map A / map D at 1M.
#   * Flat stays ~4.3-6.1 ns at every scale — where does its per-op time go?
#     Profile flat A vs flat C at 1M (C forces the inward best re-scan).
#   * Map grows with scale (~32 -> ~183 ns A) — cache misses (pointer chasing)
#     or allocator churn? Profile map A at 1k vs 1M with cache-misses.
#
# Usage:
#   scripts/perf-profile.sh [--run] <impl> <workload> <scale> [updates=N] [reps=N]
#     --run        actually execute perf (Linux only). Default is --dry-run:
#                  print the exact commands without running them.
#     impl         map | flat
#     workload     A | B | C | D | E
#     scale        price levels per side (1000 .. 1000000)
#     updates=N    steady ops per timed block   (default 2000000)
#     reps=N       timed blocks per cell         (default 3)
#
# Examples (Linux):
#   scripts/perf-profile.sh --run map C 1000000          # the map-C "why cheap" cell
#   scripts/perf-profile.sh --run flat A 1000000         # flat update-only at 1M
#   scripts/perf-profile.sh --run map A 1000             # small map for contrast
#
# Output:
#   results/perf_<impl>_<wl>_<scale>_<timestamp>/ with
#     stat.txt      perf stat aggregate counters (cycles, instructions, misses)
#     record.log    perf record status
#     perf.data     call-graph profile for `perf report`
#   The benchmark's own stdout (incl. its best_ns_per_update row) is echoed live.

set -euo pipefail

# ---- args -------------------------------------------------------------------
# Default is dry-run (print commands, execute nothing) so the harness is safe to
# inspect on non-Linux. --run actually executes perf; --dry-run is explicit and
# equivalent to the default.
RUN=0
case "${1:-}" in
    --run)     RUN=1; shift ;;
    --dry-run) shift ;;   # explicit; still dry-run
esac
if [[ $# -lt 3 ]]; then
    echo "usage: $0 [--run] <impl> <workload> <scale> [updates=N] [reps=N]" >&2
    echo "  (default is --dry-run: print commands without running perf)" >&2
    exit 2
fi
IMPL="$1"; WORKLOAD="$2"; SCALE="$3"; shift 3
UPDATES=2000000; REPS=3
for a in "$@"; do
    case "$a" in
        updates=*) UPDATES="${a#updates=}" ;;
        reps=*)    REPS="${a#reps=}" ;;
        *) echo "unexpected argument: $a" >&2; exit 2 ;;
    esac
done

case "$IMPL" in map|flat) ;; *) echo "impl must be map|flat" >&2; exit 2 ;; esac
case "$WORKLOAD" in A|B|C|D|E) ;; *) echo "workload must be A|B|C|D|E" >&2; exit 2 ;; esac

# perf stat event set: cycles + instructions + branch/cache misses that speak to
# the Phase 2 open questions. May need adjusting for a given kernel/cpu.
EVENTS=cycles,instructions,branches,branch-misses,cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses

# ---- build ------------------------------------------------------------------
cd "$(dirname "$0")/.."            # project root

STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="results/perf_${IMPL}_${WORKLOAD}_${SCALE}_${STAMP}"

# A dry run must print the exact commands WITHOUT building, creating output dirs,
# or touching perf — it exists so the harness can be inspected on non-Linux.
if [[ "$RUN" -eq 0 ]]; then
    echo "==> DRY-RUN: no build, no perf. Exact commands for a Linux host:"
    echo "  perf stat -e ${EVENTS} ./build-bench/orderbook_bench ${IMPL} ${WORKLOAD} ${SCALE} updates=${UPDATES} reps=${REPS}"
    echo "  perf record -g -o ${OUTDIR}/perf.data ./build-bench/orderbook_bench ${IMPL} ${WORKLOAD} ${SCALE} updates=${UPDATES} reps=${REPS}"
    echo "  perf report -i ${OUTDIR}/perf.data"
    echo "Run with --run to build and actually execute perf on Linux."
    exit 0
fi

# ---- execute (Linux only) ----------------------------------------------------
if ! command -v perf >/dev/null 2>&1; then
    echo "perf not found on PATH — this harness runs on Linux with linux-tools" >&2
    exit 2
fi

echo "==> Configuring (Release)"
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release >/dev/null
echo "==> Building"
cmake --build build-bench >/dev/null
BIN=./build-bench/orderbook_bench

mkdir -p results
mkdir -p "${OUTDIR}"

# perf accepts the event list as a single comma-joined argument. The benchmark
# prints its own best_ns_per_update row to stdout (saved); perf writes its
# aggregate summary to stderr (saved to stat.txt, then echoed so it is visible).
echo "==> perf stat (${IMPL} ${WORKLOAD} ${SCALE}, ${UPDATES} ops x ${REPS} reps)"
perf stat -e "${EVENTS}" "${BIN}" "${IMPL}" "${WORKLOAD}" "${SCALE}" \
    "updates=${UPDATES}" "reps=${REPS}" \
    >"${OUTDIR}/bench_stdout.txt" 2>"${OUTDIR}/stat.txt"
cat "${OUTDIR}/bench_stdout.txt"
cat "${OUTDIR}/stat.txt"

echo "==> perf record (call graph) -> ${OUTDIR}/perf.data"
perf record -g -o "${OUTDIR}/perf.data" "${BIN}" "${IMPL}" "${WORKLOAD}" "${SCALE}" \
    "updates=${UPDATES}" "reps=${REPS}" >"${OUTDIR}/record.log" 2>&1

echo "==> done: ${OUTDIR}/"
echo "    bench_stdout.txt  benchmark stdout (its best_ns_per_update row)"
echo "    stat.txt          perf stat counters"
echo "    perf.data         perf record call graph (perf report -i ${OUTDIR}/perf.data)"
