#!/usr/bin/env bash
# Linux perf profiling harness for the Experiment 01 benchmark (Phase 3 /
# Phase 3.1 profiling-methodology hardening).
#
# Profiles ONE (impl, workload, scale) cell at a time, in its own process, under
# Linux `perf` — the same per-process methodology as scripts/bench.sh, so perf
# counters are never attributed to a mixed map+flat run. Per cell it runs the
# benchmark twice:
#   1. perf stat   — aggregate hardware counters over the steady-state apply()
#                    loop ONLY (see "Measured region" below). Counters are
#                    paired with the SAME run's wall time and ns/update.
#   2. perf record — a DWARF call-graph profile (--call-graph dwarf) of where
#                    sampled time lands, for `perf report`.
#
# Measured region. perf stat is gated to the benchmark's timed apply() loop via
# the perf control interface (--delay=-1 + --control=fifo), which the benchmark
# drives through the env var LLOB_PERF_CONTROL (see order_book_bench.cpp). The
# untimed stream generation and snapshot cold-start run with counters DISABLED,
# so the counters cover the same region Phase 2 timed. The benchmark runs
# reps=1 so the counted window is exactly the one measured block.
#
#   perf record, by contrast, profiles the WHOLE process (regular sampling
#   cannot be fifo-gated portably across kernels), so its call graph is read
#   filtered to the apply() frames (docs/profiling/README.md).
#
# THIS SCRIPT RUNS ONLY ON LINUX with perf installed. It is part of a phase
# that is written to run on a Linux box but is NOT validated by measurements on
# the developing machine (Apple Silicon macOS, no perf). It is intentionally
# DRY-RUN BY DEFAULT: without --run it only prints the exact commands it would
# execute. No perf output is ever fabricated; --run captures only what a real
# Linux `perf` reports.
#
# Usage:
#   scripts/perf-profile.sh [--run] <impl> <workload> <scale> [updates=N]
#     --run        actually execute perf (Linux only). Default is dry-run:
#                  print the exact commands without running them.
#     impl         map | flat
#     workload     A | B | C | D | E
#     scale        price levels per side (1000 .. 1000000)
#     updates=N    steady ops per timed block   (default 2000000). Profiling is
#                  reps=1 (one timed block) by construction; see --reps.
#     --cpu=N      pin the benchmark to one logical CPU with taskset (optional).
#     --record     run the perf record (call-graph) pass too (default: yes).
#     --no-record  skip the perf record pass (stat only).
#
# Per-cell result layout (committed under docs/results/ after a REAL run):
#   docs/results/phase3-linux-<machine>/<cell>/
#     bench_stdout.txt   benchmark stdout (its single data row + header)
#     stat_core.txt      perf stat, core pass
#     stat_cache.txt     perf stat, generic-cache pass
#     stat_optional.txt  perf stat, optional CPU pass (if the CPU supports it)
#     perf.data          perf record call graph (if --record)
#     host.txt           full host/compiler/perf metadata (see below)
#     command.txt        the exact command line(s) run
#   The harness's default OUTDIR is results/perf_... (git-ignored /results/);
#   a real run is COPIED into docs/results by the operator, not written there
#   directly (docs/results is committed and must not get transient data).
#
# Counters are split into three PASSES so events are never multiplexed
# (each pass has <= the number of hardware counters, so every counter in a pass
# runs the whole measured window):
#   core     cycles,instructions,branches,branch-misses        (always)
#   generic  cache-references,cache-misses                     (generic: /sys/bus/event_source/devices/*/events)
#   optional L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses (CPU-specific; skipped with a
#            message when perf rejects them — never read "<not supported>" as a zero)
#
# Event handling policy (Phase 3.1):
#   * core events are REQUIRED: if perf cannot open/run any of them the run
#     FAILS loudly (a profile with no cycles is not a profile).
#   * generic/cache + optional events are BEST-EFFORT: an unsupported event is
#     reported and SKIPPED (the pass continues with the rest, or is dropped),
#     never silently turned into a fabricated zero.
#   * perf's OWN stderr (the raw stat summary, including any "<not supported>"
#     or "<not counted>" annotations and the [n%] multiplexing scale) is
#     preserved verbatim in the per-pass stat_*.txt files. perf stat's exit
#     status under `-o` is unreliable across versions, so each pass is run and
#     its raw text kept; the operator reads "<not supported>" in the raw text
#     rather than the harness inventing a number.

set -euo pipefail

# ---- args -------------------------------------------------------------------
# Default is dry-run (print commands, execute nothing) so the harness is safe to
# inspect on non-Linux. --run actually executes perf; --dry-run is explicit.
RUN=0
RECORD=1
CPU=""
UPDATES=2000000
REPS=1
for a in "$@"; do
    case "$a" in
        --run)       RUN=1 ;;
        --dry-run)   RUN=0 ;;
        --record)    RECORD=1 ;;
        --no-record) RECORD=0 ;;
        updates=*)   UPDATES="${a#updates=}" ;;
        reps=*)      REPS="${a#reps=}" ;;
        --cpu=*)     CPU="${a#--cpu=}" ;;
        -h|--help)
            sed -n '1,80p' "$0" | grep -E '^#( |$)' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)  # positional: impl workload scale (in that order)
            if [[ -z "${IMPL:-}" ]]; then IMPL="$a";
            elif [[ -z "${WORKLOAD:-}" ]]; then WORKLOAD="$a";
            elif [[ -z "${SCALE:-}" ]]; then SCALE="$a";
            else echo "unexpected argument: $a" >&2; exit 2; fi ;;
    esac
done
if [[ -z "${IMPL:-}" || -z "${WORKLOAD:-}" || -z "${SCALE:-}" ]]; then
    echo "usage: $0 [--run] [--record|--no-record] [--cpu=N] <impl> <workload> <scale> [updates=N] [reps=N]" >&2
    echo "  (default is --dry-run: print commands without running perf)" >&2
    exit 2
fi
case "$IMPL" in map|flat) ;; *) echo "impl must be map|flat" >&2; exit 2 ;; esac
case "$WORKLOAD" in A|B|C|D|E) ;; *) echo "workload must be A|B|C|D|E" >&2; exit 2 ;; esac

# Profiling cell = one (impl, workload, scale). reps is 1 by default: the perf
# gate counts a SINGLE timed block, so best-of-N would multiply the counted
# region and blur the counters. The Phase 2 ns/update a counter row is read
# against comes from the canonical CSV, not from this run's reps.
TASKSET=()
[[ -n "$CPU" ]] && TASKSET=(taskset -c "$CPU")

# ---- event passes (item 5) ---------------------------------------------------
# Split so no pass multiplexes: every event in a pass has its own hardware
# counter. core is REQUIRED; generic is generic (widely available); optional is
# CPU-specific and skipped-with-a-message when unsupported.
PASS_CORE="cycles,instructions,branches,branch-misses"
PASS_CACHE="cache-references,cache-misses"
PASS_OPT="L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses"
CORE_TAG="core"
CACHE_TAG="generic-cache"
OPT_TAG="optional-cpu"

cd "$(dirname "$0")/.."            # project root
OUTDIR="results/perf_${IMPL}_${WORKLOAD}_${SCALE}_$(date +%Y%m%d-%H%M%S)"

# ---- dry-run ----------------------------------------------------------------
if [[ "$RUN" -eq 0 ]]; then
    echo "==> DRY-RUN: no build, no perf. Exact commands for a Linux host:"
    cat <<EOF
  cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS=
  cmake --build build-perf
  (each perf stat pass below runs the benchmark under LLOB_PERF_CONTROL, reps=1)
EOF
    # Build the per-pass command lines (shared body) for display.
    show() {  # $1 = pass-tag, $2 = events
        echo "  mkfifo ${OUTDIR}/${1}_ctl ${OUTDIR}/${1}_ack"
        echo "  LLOB_PERF_CONTROL=${OUTDIR}/${1} perf stat -D -1 --control=fifo:${OUTDIR}/${1}_ctl,${OUTDIR}/${1}_ack \\"
        echo "      -e ${2} ${TASKSET[@]+"${TASKSET[@]}"} ./build-perf/orderbook_bench ${IMPL} ${WORKLOAD} ${SCALE} \\"
        echo "      updates=${UPDATES} reps=${REPS} >${OUTDIR}/bench_stdout_${1}.txt 2>${OUTDIR}/stat_${1}.txt"
        echo "  cat ${OUTDIR}/bench_stdout_${1}.txt; cat ${OUTDIR}/stat_${1}.txt"
    }
    show "$CORE_TAG"   "$PASS_CORE"
    show "$CACHE_TAG"  "$PASS_CACHE"
    show "$OPT_TAG"    "$PASS_OPT"
    if [[ "$RECORD" -eq 1 ]]; then
        echo "  # perf record pass: whole-process call graph (not fifo-gated); filter to apply() frames"
        echo "  perf record --call-graph dwarf -o ${OUTDIR}/perf.data ${TASKSET[@]+"${TASKSET[@]}"} \\"
        echo "      ./build-perf/orderbook_bench ${IMPL} ${WORKLOAD} ${SCALE} updates=${UPDATES} reps=${REPS}"
        echo "  perf report -i ${OUTDIR}/perf.data"
    fi
    echo "Run with --run to build and actually execute perf on Linux."
    exit 0
fi

# ---- execute (Linux only) ----------------------------------------------------
if [[ ! -e /proc/self/status ]]; then
    echo "This harness runs on Linux only (it needs perf + /proc)." >&2
    exit 2
fi
for tool in perf cmake taskset; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "required tool '$tool' not found on PATH — this harness runs on Linux with linux-tools." >&2
        exit 2
    fi
done

echo "==> Configuring (Release, fresh build-perf dir — no stale cache)"
# build-perf is dedicated to profiling (item 9): never reuse build-bench so a
# stale BENCH_ARCH_FLAGS / CMake cache from a canonical run cannot leak in.
rm -rf build-perf
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS= >/dev/null
cmake --build build-perf >/dev/null
BIN=./build-perf/orderbook_bench

mkdir -p results "$OUTDIR"

# Full host metadata (item 8): recorded, never auto-changed.
{
    echo "# host metadata — $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "hostname:   $(hostname)"
    echo "--- uname -a ---";          uname -a
    echo "--- lscpu (summary) ---";   lscpu 2>/dev/null | grep -E 'Architecture|Model name|CPU\(s\)|Thread|Core|Socket|NUMA|CPU MHz|Flags' | head -40 || true
    echo "--- compiler ---";          ${CXX:-c++} --version 2>/dev/null | head -1 || c++ --version | head -1
    echo "cmake:      $(cmake --version | head -1)"
    echo "perf:       $(perf --version 2>&1 | head -1)"
    echo "paranoid:   $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 'n/a')"
    echo "governor:   $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'n/a (no cpufreq)')"
    echo "cpu set:    ${CPU:-<unset: run on scheduler default>}"
    echo "affinity:   $(taskset -pc $$ 2>/dev/null || echo 'n/a')"
    [[ -n "$CPU" ]] && echo "taskset:    taskset -c $CPU"
} > "${OUTDIR}/host.txt"
cat "${OUTDIR}/host.txt"

# ---- helper: run one perf stat pass under the fifo gate -----------------------
# $1 = pass tag, $2 = event list, $3 = label (required / best-effort)
stat_pass() {
    local tag="$1" ev="$2" req="$3"
    # The harness creates the fifo pair; perf opens them at start; the benchmark
    # (perf's child, via LLOB_PERF_CONTROL) drives enable/disable/ack around its
    # timed apply loop. Clean any stale fifo first (a leftover from a crash).
    rm -f "${OUTDIR}/${tag}_ctl" "${OUTDIR}/${tag}_ack"
    mkfifo "${OUTDIR}/${tag}_ctl" "${OUTDIR}/${tag}_ack"

    echo "==> perf stat [$tag] ${IMPL} ${WORKLOAD} ${SCALE} updates=${UPDATES} reps=${REPS}"
    local stat_out="${OUTDIR}/stat_${tag}.txt"
    local bench_out="${OUTDIR}/bench_stdout_${tag}.txt"

    # LLOB_PERF_CONTROL names the fifo PAIR base: the benchmark appends _ctl/_ack.
    if ! LLOB_PERF_CONTROL="${OUTDIR}/${tag}" \
            perf stat -D -1 --control=fifo:"${OUTDIR}/${tag}_ctl,${OUTDIR}/${tag}_ack" \
            -e "$ev" \
            "${TASKSET[@]+"${TASKSET[@]}"}" \
            "$BIN" "$IMPL" "$WORKLOAD" "$SCALE" \
            "updates=${UPDATES}" "reps=${REPS}" \
            >"${bench_out}" 2>"${stat_out}"; then
        # perf exited non-zero. That is EXPECTED when an event is unsupported
        # ("<not supported>") — but the bench must still have completed (its
        # stdout carries the data row). Distinguish bench failure from perf
        # failure by checking the bench actually printed a row.
        if ! grep -qE '^(map|flat),' "${bench_out}"; then
            echo "FATAL [$tag]: benchmark did not produce a data row." >&2
            echo "  bench stdout: ${bench_out}" >&2
            echo "  perf stderr:  ${stat_out}" >&2
            if [[ "$req" == required ]]; then
                echo "  Core counters are REQUIRED — aborting. Raw output preserved." >&2
                exit 1
            fi
            echo "  Best-effort pass — continuing without [$tag] counters." >&2
            return 0
        fi
        if grep -qE '<not supported>|<not counted>' "${stat_out}"; then
            echo "  perf [$tag]: one or more events unsupported/not counted — raw output preserved." >&2
        else
            echo "  perf [$tag] returned non-zero (see raw stat_${tag}.txt) — bench row present." >&2
        fi
    fi
    cat "${bench_out}"
    cat "${stat_out}"
    echo "  [${tag}] bench row above pairs with the counters in stat_${tag}.txt (same run)."
    echo
}

# Each pass profiles the SAME single timed block in its own process (fresh
# build, same binary, same LLOB_PERF_CONTROL protocol). Three separate
# processes means the cycles/instructions you pair with a given cell's wall
# time are self-consistent within each pass; across passes they are the same
# deterministic cell, not bit-identical counters (perf re-measures each run).
stat_pass "$CORE_TAG"  "$PASS_CORE"  required
stat_pass "$CACHE_TAG" "$PASS_CACHE" best-effort
stat_pass "$OPT_TAG"   "$PASS_OPT"   best-effort

# ---- perf record pass (optional) ----------------------------------------------
if [[ "$RECORD" -eq 1 ]]; then
    echo "==> perf record (DWARF call graph, whole process) -> ${OUTDIR}/perf.data"
    echo "    NOTE: not fifo-gated — regular sampling cannot be portably gated; filter the report to apply() frames."
    perf record --call-graph dwarf -o "${OUTDIR}/perf.data" \
        "${TASKSET[@]+"${TASKSET[@]}"}" \
        "$BIN" "$IMPL" "$WORKLOAD" "$SCALE" "updates=${UPDATES}" "reps=${REPS}" \
        >"${OUTDIR}/record.log" 2>&1 || true
    cat "${OUTDIR}/record.log"
fi

# Record the exact command(s) for provenance (item 12).
{
    echo "# exact commands for this cell (phase3-linux-<machine> provenance)"
    echo "cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS="
    echo "cmake --build build-perf"
    stat_pass_cmd() { # $1 tag, $2 events
        echo "LLOB_PERF_CONTROL=${OUTDIR}/$1 perf stat -D -1 --control=fifo:${OUTDIR}/$1_ctl,${OUTDIR}/$1_ack \\"
        echo "    -e $2 ${TASKSET[@]+"${TASKSET[@]}"} $BIN $IMPL $WORKLOAD $SCALE updates=${UPDATES} reps=${REPS}"
    }
    stat_pass_cmd "$CORE_TAG"  "$PASS_CORE"
    stat_pass_cmd "$CACHE_TAG" "$PASS_CACHE"
    stat_pass_cmd "$OPT_TAG"   "$PASS_OPT"
    [[ "$RECORD" -eq 1 ]] && echo "perf record --call-graph dwarf -o ${OUTDIR}/perf.data $BIN $IMPL $WORKLOAD $SCALE updates=${UPDATES} reps=${REPS}"
} > "${OUTDIR}/command.txt"

echo "==> done: ${OUTDIR}/"
echo "    bench_stdout_*.txt  benchmark stdout (single data row per pass)"
echo "    stat_{core,cache,optional}.txt  raw perf stat per pass (source of truth)"
echo "    host.txt, command.txt  provenance metadata"
[[ "$RECORD" -eq 1 ]] && echo "    perf.data, record.log   call-graph profile for perf report"
echo
echo "    Commit under docs/results/phase3-linux-<machine>/ only after inspection:"
echo "    raw stat text says the counters are real and the bench row is present."
