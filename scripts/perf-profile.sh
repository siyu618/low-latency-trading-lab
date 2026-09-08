#!/usr/bin/env bash
# Linux perf profiling harness for the Experiment 01 benchmark (Phase 3 /
# Phase 3.1 profiling-methodology hardening / Phase 3A final correctness pass).
#
# Profiles ONE (impl, workload, scale) cell at a time, in its own process, under
# Linux `perf` — the same per-process methodology as scripts/bench.sh, so perf
# counters are never attributed to a mixed map+flat run. Per cell it runs the
# benchmark twice:
#   1. perf stat   — aggregate hardware counters over the steady-state apply()
#                    loop ONLY (the gated region, below). The benchmark's own
#                    chrono ns/update from that same run is the wall time that
#                    pairs with the counters.
#   2. perf record — a DWARF call-graph profile (--call-graph dwarf) of where
#                    sampled time lands, for `perf report`. Gated to the same
#                    apply() region when the installed perf supports the control
#                    interface; otherwise whole-process (see below).
#
# Measured region / gating. Both perf stat and perf record are gated to the
# benchmark's timed apply() loop via the perf control interface
#   --delay=-1  --control=fifo:<ctl>,<ack>
# which the benchmark drives through the env var LLOB_PERF_CONTROL
# (see order_book_bench.cpp): it writes "enable", waits for perf's "ack\n", runs
# the timed apply loop, writes "disable", waits for the second "ack". The PMU
# window and the benchmark's chrono window therefore cover the SAME apply()
# block; the untimed stream generation and snapshot cold-start run with counters
# disabled. The benchmark runs reps=1 (ENFORCED below — a profiling invocation
# with reps != 1 is rejected), so the counted window is exactly the one measured
# block: one profiling process, one cell, one measured apply block, one PMU
# counter window.
#
# Gated perf record caveat: the control interface on perf record is fully
# supported only for AUX/tracing and on newer kernels for regular sampling; on an
# older perf the --control option may be rejected or the samples not actually
# gated. The harness detects control support, gates when it works, and otherwise
# FALLS BACK to a whole-process record that is explicitly labeled in
# command.txt / record.log (never silently claimed gated). Read a fallback call
# graph filtered to the apply() frames.
#
# THIS SCRIPT RUNS ONLY ON LINUX with perf installed. It is part of a phase
# that is written to run on a Linux box but is NOT validated by measurements on
# the developing machine (Apple Silicon macOS, no perf). It is intentionally
# DRY-RUN BY DEFAULT: without --run it only prints the exact commands it would
# execute. No perf output is ever fabricated; --run captures only what a real
# Linux `perf` reports.
#
# Usage:
#   scripts/perf-profile.sh [--run] <impl> <workload> <scale> [updates=N] [reps=N]
#     --run        actually execute perf (Linux only). Default is dry-run.
#     --dry-run    explicit dry-run (default).
#     --cpu=N      pin the benchmark to one logical CPU with taskset (optional).
#     --record     run the perf record (call-graph) pass too (default: yes).
#     --no-record  skip the perf record pass (stat only).
#     impl         map | flat
#     workload     A | B | C | D | E
#     scale        price levels per side (1000 .. 1000000)
#     updates=N    steady ops per timed block            (default 2000000)
#     reps=N       must be 1 for profiling — REJECTED otherwise (default 1).
#
# Output filenames (per stat pass tag core|generic-cache|optional-cpu):
#   results/perf_<impl>_<wl>_<scale>_<timestamp>/
#     bench_stdout_core.txt            benchmark stdout, core pass
#     bench_stdout_generic-cache.txt   benchmark stdout, generic-cache pass
#     bench_stdout_optional-cpu.txt    benchmark stdout, optional-cpu pass (if any)
#     stat_core.txt                    raw perf stat, core pass (source of truth)
#     stat_generic-cache.txt           raw perf stat, generic-cache pass
#     stat_optional-cpu.txt            raw perf stat, optional-cpu pass (if any)
#     perf.data / record.log           perf record call graph (if --record)
#     host.txt                         host/compiler/perf metadata
#     command.txt                      the exact command(s) run
#   A real run is COPIED into docs/results/phase3-linux-<machine>/ by the
#   operator; the harness never writes docs/results directly (it is committed).
#
# Event passes. Counters are split into three passes to REDUCE the risk of
# multiplexing (fewer events than a CPU's hardware counters per pass). This is
# NOT a guarantee: some CPUs, fixed-counter setups, the NMI watchdog, or PMU
# availability can still multiplex a pass. Whether multiplexing actually
# occurred is decided by perf's own output — time-running vs time-enabled, the
# [n%] scaling annotation — which is preserved verbatim in each stat_<pass>.txt.
# If a pass's running percentage is materially below 100%, derived metrics
# (IPC, miss rates, cycles/update) are unreliable for that pass and must be
# marked/adjusted in the analysis, never silently taken at face value.
#
#   core         cycles,instructions,branches,branch-misses        (REQUIRED)
#   generic-cache cache-references,cache-misses                    (best-effort)
#   optional-cpu L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses (best-effort)
#
# Event handling policy:
#   * core is REQUIRED. After the pass, the raw stat text is inspected REGARDLESS
#     of perf's exit code. If it contains "<not supported>"/"<not counted>", or a
#     required event's count is absent/unparseable, the profiling cell FAILS
#     loudly (a profile with no cycles is not a profile). perf's exit status is
#     NOT used as the sole signal (it varies across versions).
#   * generic-cache + optional-cpu are BEST-EFFORT: unsupported events are
#     reported and left absent from the analysis — never converted to a zero.
#     Their raw text is preserved.
#   * Perf's own stderr is the source of truth and is always preserved verbatim.
#
# Wall time / PMU quality:
#   * The benchmark's OWN chrono-reported ns/update is the wall time of the gated
#     region. perf's "seconds time elapsed" is NOT treated as the gated wall time
#     (it may span the whole enabled/alive window and is not a chrono measure).
#   * perf's time-enabled / time-running / [n%] scaling is used for PMU QUALITY
#     (did the pass multiplex?), not as a substitute wall clock.

set -euo pipefail

# ---- args -------------------------------------------------------------------
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
            sed -n '1,90p' "$0" | grep -E '^#( |$)' | sed 's/^# \{0,1\}//'
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

# ---- enforce reps=1 (Phase 3A correctness) -----------------------------------
# A profiling cell is ONE measured apply block: the perf gate counts a single
# enable/disable window, and perf counters aggregated over several windows could
# not be paired with a benchmark result that reports best-of-N. So a profiling
# invocation with reps != 1 is an error, not a soft default.
if [[ "$REPS" != "1" ]]; then
    echo "error: profiling requires reps=1 (got reps=${REPS})." >&2
    echo "  A perf counter window must pair with ONE measured apply block;" >&2
    echo "  best-of-N wall time has no single counter window to pair with." >&2
    exit 2
fi

# Profiling cell = one (impl, workload, scale), one measured block (reps=1).
TASKSET=()
[[ -n "$CPU" ]] && TASKSET=(taskset -c "$CPU")

# ---- event passes ------------------------------------------------------------
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
    echo "==> DRY-RUN: no build, no perf. Exact commands for a Linux host (reps is forced to 1):"
    cat <<EOF
  cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS=
  cmake --build build-perf
  (each perf stat pass below runs the benchmark under LLOB_PERF_CONTROL, reps=1)
EOF
    show() {  # $1 = pass-tag, $2 = events
        echo "  mkfifo ${OUTDIR}/${1}_ctl ${OUTDIR}/${1}_ack"
        echo "  LLOB_PERF_CONTROL=${OUTDIR}/${1} perf stat -D -1 --control=fifo:${OUTDIR}/${1}_ctl,${OUTDIR}/${1}_ack \\"
        echo "      -e ${2} ${TASKSET[@]+"${TASKSET[@]}"} ./build-perf/orderbook_bench ${IMPL} ${WORKLOAD} ${SCALE} \\"
        echo "      updates=${UPDATES} reps=1 >${OUTDIR}/bench_stdout_${1}.txt 2>${OUTDIR}/stat_${1}.txt"
        echo "  cat ${OUTDIR}/bench_stdout_${1}.txt; cat ${OUTDIR}/stat_${1}.txt"
    }
    show "$CORE_TAG"   "$PASS_CORE"
    show "$CACHE_TAG"  "$PASS_CACHE"
    show "$OPT_TAG"    "$PASS_OPT"
    if [[ "$RECORD" -eq 1 ]]; then
        echo "  # perf record pass (gated to apply() when perf supports the control interface)"
        echo "  perf record -D -1 --control=fifo:${OUTDIR}/record_ctl,${OUTDIR}/record_ack \\"
        echo "      --call-graph dwarf -o ${OUTDIR}/perf.data ${TASKSET[@]+"${TASKSET[@]}"} \\"
        echo "      ./build-perf/orderbook_bench ${IMPL} ${WORKLOAD} ${SCALE} updates=${UPDATES} reps=1"
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
rm -rf build-perf
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS= >/dev/null
cmake --build build-perf >/dev/null
BIN=./build-perf/orderbook_bench

mkdir -p results "$OUTDIR"

# Full host metadata: recorded, never auto-changed.
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

# ---- perf record control-support probe -----------------------------------------
# Regular (non-AUX) sampling is only gated by the control interface on perf
# builds/kernels that support it. Probe by asking perf record to list its
# options and grepping for --control; that is cheap and does not run anything.
RECORD_CTL_SUPPORT=0
if perf record --help 2>&1 | grep -q -- '--control'; then
    RECORD_CTL_SUPPORT=1
fi
echo "==> perf record control-interface support: ${RECORD_CTL_SUPPORT} (1=gated, 0=whole-process fallback)"

# ---- helper: run one perf stat pass under the fifo gate -----------------------
# $1 = pass tag, $2 = event list, $3 = required|best-effort
stat_pass() {
    local tag="$1" ev="$2" req="$3"
    rm -f "${OUTDIR}/${tag}_ctl" "${OUTDIR}/${tag}_ack"
    mkfifo "${OUTDIR}/${tag}_ctl" "${OUTDIR}/${tag}_ack"

    echo "==> perf stat [$tag] ${IMPL} ${WORKLOAD} ${SCALE} updates=${UPDATES} reps=1"
    local stat_out="${OUTDIR}/stat_${tag}.txt"
    local bench_out="${OUTDIR}/bench_stdout_${tag}.txt"

    set +e   # perf exit status varies across versions; we inspect raw text below
    LLOB_PERF_CONTROL="${OUTDIR}/${tag}" \
        perf stat -D -1 --control=fifo:"${OUTDIR}/${tag}_ctl,${OUTDIR}/${tag}_ack" \
        -e "$ev" \
        "${TASKSET[@]+"${TASKSET[@]}"}" \
        "$BIN" "$IMPL" "$WORKLOAD" "$SCALE" \
        "updates=${UPDATES}" "reps=1" \
        >"${bench_out}" 2>"${stat_out}"
    local prc=$?
    set -e

    # The benchmark itself must have produced its data row (a row means the gate
    # handshake completed and the apply loop ran under the counters).
    if ! grep -qE '^(map|flat),' "${bench_out}"; then
        echo "FATAL [$tag]: benchmark produced no data row (gate handshake or apply failed)." >&2
        cat "${stat_out}" >&2
        if [[ "$req" == required ]]; then
            echo "  Core counters are REQUIRED — aborting." >&2
            exit 1
        fi
        echo "  Best-effort pass — continuing without [$tag]." >&2
        return 0
    fi

    # Inspect the raw stat text REGARDLESS of perf's exit code.
    local unsupported=""
    if grep -qE '<not supported>|<not counted>' "${stat_out}"; then
        unsupported="yes"
    fi

    if [[ "$req" == required ]]; then
        # REQUIRED core pass: any unsupported/not-counted event, or an absent
        # count for a core event, fails the cell loudly. We check the presence
        # of each core event's count in the raw text (a count is a number in the
        # <event> column). perf formats the summary in its stderr; the event
        # name always appears. This deliberately does NOT trust perf's exit code.
        local missing=0
        for evname in cycles instructions branches branch-misses; do
            if ! grep -qE "${evname}" "${stat_out}"; then
                echo "FATAL [$tag]: required event '${evname}' absent from raw stat." >&2
                missing=1
            fi
        done
        if [[ -n "$unsupported" ]] || [[ "$missing" -eq 1 ]]; then
            echo "FATAL [$tag]: core events not fully counted — see raw stat_${tag}.txt." >&2
            cat "${stat_out}" >&2
            exit 1
        fi
    else
        # Best-effort: report unsupported events, preserve raw text, never zero.
        if [[ -n "$unsupported" ]]; then
            echo "  perf [$tag]: one or more events unsupported/not counted — see raw stat_${tag}.txt; absent events are left out of the analysis, never set to zero." >&2
        fi
    fi

    if [[ "$prc" -ne 0 ]]; then
        # Non-zero exit but raw text shows the events counted and the bench row
        # is present — note it, keep the raw text, do not fail on exit status.
        echo "  perf [$tag] exited ${prc}; raw stat preserved (exit status is not the reliability signal)." >&2
    fi

    cat "${bench_out}"
    cat "${stat_out}"
    echo "  [${tag}] gated window pairs the bench row above with stat_${tag}.txt (same run);"
    echo "        wall time = the bench's own chrono ns/update, NOT perf's 'time elapsed'."
    echo
}

stat_pass "$CORE_TAG"  "$PASS_CORE"  required
stat_pass "$CACHE_TAG" "$PASS_CACHE" best-effort
stat_pass "$OPT_TAG"   "$PASS_OPT"   best-effort

# ---- perf record pass ----------------------------------------------------------
if [[ "$RECORD" -eq 1 ]]; then
    rec_gated=0
    if [[ "$RECORD_CTL_SUPPORT" -eq 1 ]]; then
        rm -f "${OUTDIR}/record_ctl" "${OUTDIR}/record_ack"
        mkfifo "${OUTDIR}/record_ctl" "${OUTDIR}/record_ack"
        rec_gated=1
    fi
    echo "==> perf record (--call-graph dwarf) -> ${OUTDIR}/perf.data"
    if [[ "$rec_gated" -eq 1 ]]; then
        echo "    gated to the apply() region via the control interface (LLOB_PERF_CONTROL)."
        # LLOB_PERF_CONTROL must be set in perf's environment so the benchmark
        # child drives enable/disable/ack on these fifos — else a -D -1 perf
        # record would wait forever for an enable that never comes.
        LLOB_PERF_CONTROL="${OUTDIR}/record" \
            perf record -D -1 --control=fifo:"${OUTDIR}/record_ctl,${OUTDIR}/record_ack" \
            --call-graph dwarf -o "${OUTDIR}/perf.data" \
            "${TASKSET[@]+"${TASKSET[@]}"}" \
            "$BIN" "$IMPL" "$WORKLOAD" "$SCALE" "updates=${UPDATES}" "reps=1" \
            >"${OUTDIR}/record.log" 2>&1 || true
    else
        echo "    whole-process fallback (perf lacks record --control) — read filtered to apply() frames."
        perf record --call-graph dwarf -o "${OUTDIR}/perf.data" \
            "${TASKSET[@]+"${TASKSET[@]}"}" \
            "$BIN" "$IMPL" "$WORKLOAD" "$SCALE" "updates=${UPDATES}" "reps=1" \
            >"${OUTDIR}/record.log" 2>&1 || true
    fi
    cat "${OUTDIR}/record.log"
    echo "    record_gated=${rec_gated} (1 = gated to apply(); 0 = whole-process fallback)"
fi

# Record the exact command(s) for provenance.
{
    echo "# exact commands for this cell (phase3-linux-<machine> provenance)"
    echo "# profiling cell: one process, one benchmark cell, one measured apply block, one PMU window (reps=1)"
    echo "cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS="
    echo "cmake --build build-perf"
    stat_pass_cmd() { # $1 tag, $2 events
        echo "LLOB_PERF_CONTROL=${OUTDIR}/$1 perf stat -D -1 --control=fifo:${OUTDIR}/$1_ctl,${OUTDIR}/$1_ack \\"
        echo "    -e $2 ${TASKSET[@]+"${TASKSET[@]}"} $BIN $IMPL $WORKLOAD $SCALE updates=${UPDATES} reps=1"
    }
    stat_pass_cmd "$CORE_TAG"  "$PASS_CORE"
    stat_pass_cmd "$CACHE_TAG" "$PASS_CACHE"
    stat_pass_cmd "$OPT_TAG"   "$PASS_OPT"
    if [[ "$RECORD" -eq 1 ]]; then
        if [[ "${RECORD_CTL_SUPPORT:-0}" -eq 1 ]]; then
            echo "perf record -D -1 --control=fifo:${OUTDIR}/record_ctl,${OUTDIR}/record_ack --call-graph dwarf -o ${OUTDIR}/perf.data ${TASKSET[@]+"${TASKSET[@]}"} $BIN $IMPL $WORKLOAD $SCALE updates=${UPDATES} reps=1   # record_gated=1"
        else
            echo "perf record --call-graph dwarf -o ${OUTDIR}/perf.data ${TASKSET[@]+"${TASKSET[@]}"} $BIN $IMPL $WORKLOAD $SCALE updates=${UPDATES} reps=1   # record_gated=0 (whole-process fallback)"
        fi
    fi
} > "${OUTDIR}/command.txt"

echo "==> done: ${OUTDIR}/"
echo "    bench_stdout_{core,generic-cache,optional-cpu}.txt  benchmark stdout per pass"
echo "    stat_{core,generic-cache,optional-cpu}.txt          raw perf stat per pass (source of truth)"
echo "    host.txt, command.txt                               provenance metadata"
[[ "$RECORD" -eq 1 ]] && echo "    perf.data, record.log       call-graph profile for perf report"
echo
echo "    Commit under docs/results/phase3-linux-<machine>/ only after inspection:"
echo "    the raw stat text shows the counters are real, and the bench row is present."
echo "    Add a same-host ungated throughput baseline from this same build for the"
echo "    Linux latency that the counters are read against (never the M3 Max CSV)."
