#!/usr/bin/env bash
# Experiment 02 Phase 2 — canonical throughput runner.
#
# Runs the complete 2 x 3 x 3 matrix of the deterministic two-thread
# end-to-end message-transfer benchmark:
#
#   impl      x message_bytes x capacity
#   mutex,spsc x 8,32,64      x 1024,4096,65536     = 18 cells
#
# ONE IMPLEMENTATION PER PROCESS. Every process is a SEPARATE invocation of
# spsc_throughput_bench choosing exactly one --impl; the mutex and SPSC queues
# are never timed inside one interval, one address space, or one warmed-up
# process state. This script never passes --impl=both (the benchmark rejects it).
#
# BALANCED AB/BA IMPLEMENTATION ORDER (why there are exactly 4 sessions)
#   The previous canonical run executed all nine mutex cells before all nine SPSC
#   cells in every session, which perfectly confounded implementation with
#   position in time. On an unpinned macOS host — where scheduler placement,
#   thread migration, P/E-core selection, DVFS, thermal state and background load
#   all drift — an implementation difference and a time-of-run difference would
#   then be indistinguishable.
#
#   This runner therefore runs the two implementations of the SAME
#   (message_bytes, capacity) pair as ADJACENT processes, and balances both the
#   implementation order and the traversal direction across four sessions:
#
#     session 1: forward traversal, mutex -> spsc
#     session 2: reverse traversal, spsc  -> mutex
#     session 3: forward traversal, spsc  -> mutex
#     session 4: reverse traversal, mutex -> spsc
#
#   Every (bytes, capacity) pair therefore receives 2 mutex-first and 2
#   spsc-first comparisons, and the cell-position/time-order axis is balanced by
#   the forward/reverse traversal. The order is FIXED and DETERMINISTIC, and the
#   exact sequence of invocations is recorded in command.txt as it runs. It is
#   never randomized silently.
#
# MEASUREMENT DISCIPLINE (see docs/SPSC_THROUGHPUT.md):
#   * Release build, -O3 -DNDEBUG, forced by the CMake target itself.
#   * Thread creation, queue allocation and the expected-checksum pre-pass all
#     happen outside the timed interval; the consumer records t1 itself.
#   * ns_per_message is END-TO-END elapsed / messages DELIVERED. It is NOT a
#     per-call latency and NOT a one-way handoff time.
#   * Every repetition constructs a FRESH producer/consumer thread pair, so a
#     repetition inherits no placement from the previous one. A "session" is
#     only a grouping of repetitions inside one process/address-space lifetime.
#   * One warm-up repetition per process is EXCLUDED from every published and
#     derived figure. All measured repetitions are kept — this script never
#     drops a slow run and never selects a best run.
#   * Every cell must self-validate (all N delivered, exact FIFO sequence,
#     checksum matching an independently precomputed stream). A cell that fails
#     is NOT published: the benchmark exits non-zero and this script stops.
#
# PRESERVE-BEFORE-SUMMARIZE. Every raw per-repetition CSV is written first,
# untouched. Only after every process has finished does the script derive the
# summaries, and it then RE-VERIFIES each process summary against the raw CSV it
# claims to summarize — a summary that does not match its own raw data fails the
# run loudly. No number here is ever the output of a second, independent run.
#
# RAW OUTPUTS ARE NEVER FABRICATED OR EDITED. This script only invokes the
# benchmark and records what the machine measured. If the host is not the
# canonical development host, the results are still real — they are simply not
# the canonical M3 Max dataset, and HOST.md records what actually ran.
#
# Usage:
#   scripts/spsc-throughput.sh                 # canonical balanced matrix
#   FORCE=1 scripts/spsc-throughput.sh         # overwrite an existing results dir
#   MESSAGES=200000 REPS=3 scripts/spsc-throughput.sh   # smoke-sized matrix
#
# Env overrides:
#   MESSAGES  messages per repetition (default 10000000)
#   REPS      measured repetitions per process (default 5, all kept)
#   WARMUP    warm-up repetitions per process, excluded (default 1)
#   OUT       results directory (default docs/results/spsc-throughput)
#   FORCE     1 = allow writing into a non-empty OUT directory
#   BUILDDIR  build directory (default build-spsc-perf)
# SESSIONS is NOT overridable: the AB/BA balance is defined for exactly 4.

set -euo pipefail
cd "$(dirname "$0")/.."

REPO_ROOT="$(pwd)"
BIN_NAME="spsc_throughput_bench"

MESSAGES="${MESSAGES:-10000000}"
REPS="${REPS:-5}"
WARMUP="${WARMUP:-1}"
OUT="${OUT:-docs/results/spsc-throughput}"
FORCE="${FORCE:-0}"
BUILDDIR="${BUILDDIR:-build-spsc-perf}"

# Recorded in command.txt, so it must reflect the directory actually used.
BENCH_REL="$BUILDDIR/$BIN_NAME"

# The balanced design is defined for exactly four sessions (2 mutex-first +
# 2 spsc-first, 2 forward + 2 reverse). Anything else silently loses the
# balance guarantee, so it is not offered as an option.
SESSIONS=4
CELLS_PER_SESSION=18
EXPECTED_PROCESSES=$(( CELLS_PER_SESSION * SESSIONS ))   # 72
EXPECTED_RAW_FILES=$EXPECTED_PROCESSES

if [[ "$REPS" -lt 1 ]]; then
    echo "FATAL: REPS must be >= 1 (got $REPS)" >&2
    exit 1
fi

# Canonical bytes/capacity pairs, in forward traversal order. bash 3.2 (macOS)
# has no associative arrays, so this stays a flat list of "bytes capacity" pairs.
PAIRS=()
for bytes in 8 32 64; do
    for cap in 1024 4096 65536; do
        PAIRS+=("$bytes $cap")
    done
done

# ---------------------------------------------------------------------------
# Guards
# ---------------------------------------------------------------------------
if [[ -e "$OUT" && ! -d "$OUT" ]]; then
    echo "FATAL: $OUT exists and is not a directory" >&2
    exit 1
fi
if [[ -d "$OUT" ]] && [[ -n "$(ls -A "$OUT" 2>/dev/null || true)" ]] && [[ "$FORCE" != "1" ]]; then
    echo "FATAL: $OUT already exists and is non-empty." >&2
    echo "       Canonical results are not overwritten silently. Re-run with FORCE=1" >&2
    echo "       if you really mean to replace them, or set OUT=<other dir>." >&2
    exit 1
fi
case "$OUT" in
    *phase2-m3max*|*phase3-*|*phase4-*|*orderbook-bitmap-optimization*)
        echo "FATAL: refusing to write into the Experiment 01 results directory $OUT" >&2
        exit 1
        ;;
esac

mkdir -p "$OUT/raw" "$OUT/summaries" "$OUT/stderr"

# ---------------------------------------------------------------------------
# Build — fresh Release dir, no stale cache, no arch flags unless asked for
# ---------------------------------------------------------------------------
echo "==> Configuring (Release, fresh $BUILDDIR)"
rm -rf "$BUILDDIR"
cmake -S . -B "$BUILDDIR" -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS= >/dev/null

echo "==> Building $BIN_NAME"
cmake --build "$BUILDDIR" --target "$BIN_NAME" -j >/dev/null

BENCH="$BUILDDIR/$BIN_NAME"
if [[ ! -x "$BENCH" ]]; then
    echo "FATAL: $BENCH was not built" >&2
    exit 1
fi

# Fail fast: a tiny smoke cell through BOTH implementations before the real
# matrix. This is the only automatic benchmark execution; the canonical cells
# are never run by ctest.
echo "==> Smoke check (small, discarded)"
for impl in mutex spsc; do
    "$BENCH" --impl="$impl" --message-bytes=8 --capacity=1024 \
             --messages=20000 --reps=1 --warmup=0 >/dev/null
done
echo "    smoke check passed for both implementations"

# ---------------------------------------------------------------------------
# Session design — fixed, deterministic, and recorded as it runs
# ---------------------------------------------------------------------------
# SESSION_FIRST[session] = implementation that runs first in that session.
# Traversal is forward for odd sessions, reverse for even sessions.
session_first_impl() {
    case "$1" in
        1) echo mutex ;;
        2) echo spsc  ;;
        3) echo spsc  ;;
        4) echo mutex ;;
        *) echo "FATAL: no design for session $1" >&2; return 1 ;;
    esac
}
session_traversal() {
    case "$1" in
        1|3) echo forward ;;
        2|4) echo reverse ;;
    esac
}

# ---------------------------------------------------------------------------
# Leg 1 — run every process, preserving raw output. Nothing is summarized yet.
# ---------------------------------------------------------------------------
echo
echo "==> Running the balanced canonical matrix"
echo "    9 bytes/capacity pairs x 2 impls x $SESSIONS sessions = $EXPECTED_PROCESSES processes"
echo "    messages=$MESSAGES reps=$REPS warmup=$WARMUP (warm-up excluded from all reported data)"

: >"$OUT/command.txt"
{
    echo "# Experiment 02 Phase 2 canonical run — exact effective invocations, IN ORDER"
    echo "# UTC:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# local:     $(date +%Y-%m-%dT%H:%M:%S%z)"
    echo "# repo HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "# messages=$MESSAGES reps=$REPS warmup=$WARMUP sessions=$SESSIONS"
    echo "#"
    echo "# BALANCED AB/BA DESIGN (fixed, deterministic, never randomized):"
    echo "#   session 1: forward traversal, mutex -> spsc"
    echo "#   session 2: reverse traversal, spsc  -> mutex"
    echo "#   session 3: forward traversal, spsc  -> mutex"
    echo "#   session 4: reverse traversal, mutex -> spsc"
    echo "# Each bytes/capacity pair therefore gets 2 mutex-first and 2 spsc-first"
    echo "# comparisons, with forward/reverse traversal balancing time order."
    echo "#"
    echo "# Each line below is a SEPARATE process with exactly ONE implementation."
    echo
} >>"$OUT/command.txt"

for session in $(seq 1 "$SESSIONS"); do
    first_impl="$(session_first_impl "$session")"
    if [[ "$first_impl" == "mutex" ]]; then second_impl=spsc; else second_impl=mutex; fi
    traversal="$(session_traversal "$session")"

    {
        echo
        echo "# ---- session $session/$SESSIONS | $traversal traversal | implementation order: $first_impl then $second_impl ----"
    } >>"$OUT/command.txt"

    echo
    echo "==> Session $session/$SESSIONS — $traversal traversal, $first_impl then $second_impl"

    process_index=0
    npairs=${#PAIRS[@]}
    for (( i = 0; i < npairs; i++ )); do
        if [[ "$traversal" == "reverse" ]]; then
            pair_index=$(( npairs - 1 - i ))
        else
            pair_index=$i
        fi
        # shellcheck disable=SC2086
        set -- ${PAIRS[$pair_index]}
        bytes="$1"; cap="$2"

        for impl in "$first_impl" "$second_impl"; do
            process_index=$(( process_index + 1 ))
            cell_id="${impl}_b${bytes}_c${cap}"
            raw="$OUT/raw/${cell_id}_s${session}.csv"
            summary="$OUT/summaries/${cell_id}_s${session}.txt"

            echo "    [$process_index/$EXPECTED_PROCESSES] s$session $cell_id"

            # Build the invocation as an array and serialize it with %q quoting
            # (bash 3.2 has no ${arr[@]@Q}), so command.txt records exactly what
            # ran, in the order it ran.
            cmd=("$BENCH_REL" "--impl=$impl" "--message-bytes=$bytes" "--capacity=$cap"
                 "--messages=$MESSAGES" "--reps=$REPS" "--warmup=$WARMUP"
                 "--raw-out=$raw" "--summary-out=$summary")
            for a in "${cmd[@]}"; do printf '%q ' "$a"; done >>"$OUT/command.txt"
            printf '\n' >>"$OUT/command.txt"

            # A failing cell aborts the run: set -e propagates the benchmark's
            # non-zero exit, the cell writes no raw CSV, and nothing downstream
            # publishes it.
            "$BENCH" "--impl=$impl" "--message-bytes=$bytes" "--capacity=$cap" \
                     --messages="$MESSAGES" --reps="$REPS" --warmup="$WARMUP" \
                     --raw-out="$raw" --summary-out="$summary" \
                     >/dev/null 2>"$OUT/stderr/${cell_id}_s${session}.txt"
        done
    done
done
echo
echo "==> All $EXPECTED_PROCESSES processes completed and validated (raw output preserved)"

# ---------------------------------------------------------------------------
# Leg 2 — verify every summary against its own raw CSV, then summarize.
# ---------------------------------------------------------------------------
echo
echo "==> Verifying each process summary against the raw CSV it summarizes"

verify_cell() {
    local raw="$1" summary="$2" cell_id="$3"

    if [[ ! -s "$raw" ]]; then
        echo "FATAL: $cell_id produced no raw CSV" >&2
        return 1
    fi
    if ! grep -q '^correctness=PASS$' "$summary"; then
        echo "FATAL: $cell_id summary does not report correctness=PASS" >&2
        return 1
    fi

    # Recompute median/min/max of ns_per_message (column 7) from the raw CSV,
    # and pull the claimed values out of the summary in the same awk program.
    awk -F, -v id="$cell_id" -v sumfile="$summary" -v rawfile="$raw" '
        function close_enough(a, b) {
            return (a > b ? a - b : b - a) <= 1e-6 * (a > 1 ? a : 1)
        }
        BEGIN {
            # ---- claimed values, read from the summary file ----
            while ((getline line < sumfile) > 0) {
                if (line ~ /^measured_reps=/)                 { split(line, p, "="); s_n   = p[2] }
                else if (line ~ /^median_ns_per_message=/)    { split(line, p, "="); s_med = p[2] }
                else if (line ~ /^min_ns_per_message=/)       { split(line, p, "="); s_min = p[2] }
                else if (line ~ /^max_ns_per_message=/)       { split(line, p, "="); s_max = p[2] }
            }
            close(sumfile)
            if (s_n == "" || s_med == "" || s_min == "" || s_max == "") {
                printf "FATAL: %s summary is missing required keys\n", id > "/dev/stderr"
                bad = 1
                exit
            }
            # ---- recomputed values, read from the raw repetition data ----
            while ((getline rline < rawfile) > 0) {
                if (rline ~ /^#/ || rline == "") { continue }
                split(rline, c, ",")
                # Data rows start with a numeric repetition index; this skips
                # the column-header line of the CSV.
                if (c[1] ~ /^[0-9]+$/ && c[7] != "") { v[++n] = c[7] + 0 }
            }
            close(rawfile)
            if (n == 0) {
                printf "FATAL: %s raw CSV has no data rows\n", id > "/dev/stderr"
                bad = 1
                exit
            }
            # insertion sort (n is a handful of repetitions)
            for (i = 1; i <= n; i++) { sorted[i] = v[i] }
            for (i = 2; i <= n; i++) {
                key = sorted[i]; j = i - 1
                while (j >= 1 && sorted[j] > key) { sorted[j+1] = sorted[j]; j-- }
                sorted[j+1] = key
            }
            med = (n % 2 == 1) ? sorted[int((n+1)/2)] \
                               : (sorted[int(n/2)] + sorted[int(n/2)+1]) / 2

            if (n != s_n + 0) {
                printf "FATAL: %s rep count raw=%d summary=%s\n", id, n, s_n > "/dev/stderr"
                bad = 1; exit
            }
            if (!close_enough(med, s_med)) {
                printf "FATAL: %s median raw=%.6f summary=%s\n", id, med, s_med > "/dev/stderr"
                bad = 1; exit
            }
            if (!close_enough(sorted[1], s_min)) {
                printf "FATAL: %s min raw=%.6f summary=%s\n", id, sorted[1], s_min > "/dev/stderr"
                bad = 1; exit
            }
            if (!close_enough(sorted[n], s_max)) {
                printf "FATAL: %s max raw=%.6f summary=%s\n", id, sorted[n], s_max > "/dev/stderr"
                bad = 1; exit
            }
        }
        END { if (bad) exit 1 }
    ' /dev/null || return 1

    return 0
}

for session in $(seq 1 "$SESSIONS"); do
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in mutex spsc; do
            cell_id="${impl}_b${1}_c${2}"
            verify_cell "$OUT/raw/${cell_id}_s${session}.csv" \
                        "$OUT/summaries/${cell_id}_s${session}.txt" \
                        "${cell_id}_s${session}"
        done
    done
done
echo "    all $EXPECTED_PROCESSES summaries match their own raw repetition data"

# ---------------------------------------------------------------------------
# Leg 3 — dataset invariants. Any violation fails the canonical run.
# ---------------------------------------------------------------------------
echo
echo "==> Checking dataset invariants"

# Parse the execution order back OUT of command.txt — i.e. from the record of
# what actually ran, not from the design constants above. This is what makes the
# balance claim a verified property of the run rather than an intention.
ORDER_FILE="$OUT/run_order.txt"
awk '
    /^# ---- session / {
        # "# ---- session 2/4 | reverse traversal | ..."
        s = $4; sub(/\/.*/, "", s); next
    }
    /--impl=/ {
        impl = ""; mb = ""; cap = ""
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^--impl=/)          { split($i, a, "="); impl = a[2] }
            if ($i ~ /^--message-bytes=/) { split($i, a, "="); mb   = a[2] }
            if ($i ~ /^--capacity=/)      { split($i, a, "="); cap  = a[2] }
        }
        if (s == "" || impl == "" || mb == "" || cap == "") {
            printf "FATAL: unparseable command.txt invocation line\n" > "/dev/stderr"
            exit 1
        }
        printf "%s\t%s\t%s\t%s\n", s, mb, cap, impl
    }
' "$OUT/command.txt" >"$ORDER_FILE"

invariant_fail() {
    echo "FATAL: invariant violated — $1" >&2
    exit 1
}

# (a) exactly EXPECTED_PROCESSES invocations were recorded
recorded="$(wc -l <"$ORDER_FILE" | tr -d ' ')"
[[ "$recorded" == "$EXPECTED_PROCESSES" ]] \
    || invariant_fail "command.txt records $recorded invocations, expected $EXPECTED_PROCESSES"

# (b) exactly EXPECTED_RAW_FILES raw CSVs exist
raw_count="$(ls "$OUT"/raw/*.csv 2>/dev/null | wc -l | tr -d ' ')"
[[ "$raw_count" == "$EXPECTED_RAW_FILES" ]] \
    || invariant_fail "found $raw_count raw CSVs, expected $EXPECTED_RAW_FILES"

# (c) every raw CSV has exactly REPS measured rows, all correctness=PASS,
#     all with the same checksum, and (d) every cell has exactly SESSIONS
#     sessions and each implementation ran first exactly twice.
awk -F, -v reps="$REPS" -v sessions="$SESSIONS" -v ord="$ORDER_FILE" '
    BEGIN {
        while ((getline line < ord) > 0) {
            split(line, f, "\t")
            s = f[1]; mb = f[2]; cap = f[3]; impl = f[4]
            key = mb "_" cap
            # One invocation per (cell, session, implementation): 2 per session.
            # Count DISTINCT sessions per cell, not invocations.
            sess_seen[key SUBSEP s] = 1
            # First implementation seen for this (cell, session) is the one that
            # ran first, because run_order.txt preserves execution order.
            if (!((key SUBSEP s) in first)) { first[key SUBSEP s] = impl }
        }
        close(ord)
    }
    FNR == 1 {
        if (prev != "" && prev_n != reps) {
            printf "FATAL: %s has %d measured rows, expected %d\n", prev, prev_n, reps > "/dev/stderr"
            bad = 1
        }
        prev = FILENAME; prev_n = 0; prev_ck = ""
    }
    /^#/ || /^rep,/ || NF == 0 { next }
    {
        if ($1 !~ /^[0-9]+$/) { next }
        prev_n++
        if ($12 != "PASS") {
            printf "FATAL: %s has a non-PASS row: %s\n", FILENAME, $0 > "/dev/stderr"
            bad = 1
        }
        if ($7 == "" || $7 + 0 <= 0) {
            printf "FATAL: %s has a non-positive ns_per_message: %s\n", FILENAME, $0 > "/dev/stderr"
            bad = 1
        }
        if (prev_ck == "") { prev_ck = $11 } else if (prev_ck != $11) {
            printf "FATAL: %s checksum varies between repetitions\n", FILENAME > "/dev/stderr"
            bad = 1
        }
    }
    END {
        if (prev != "" && prev_n != reps) {
            printf "FATAL: %s has %d measured rows, expected %d\n", prev, prev_n, reps > "/dev/stderr"
            bad = 1
        }
        for (k in sess_seen) {
            split(k, p, SUBSEP)
            nsess[p[1]]++
        }
        for (key in nsess) {
            if (nsess[key] != sessions) {
                printf "FATAL: cell %s has %d sessions, expected %d\n", key, nsess[key], sessions > "/dev/stderr"
                bad = 1
            }
        }
        if (length(nsess) != 9) {
            printf "FATAL: found %d distinct cells in run_order.txt, expected 9\n", length(nsess) > "/dev/stderr"
            bad = 1
        }
        # each implementation must be FIRST exactly sessions/2 times per cell
        for (k in first) {
            split(k, p, SUBSEP)
            cell = p[1]; s = p[2]
            cnt[cell SUBSEP first[k]]++
        }
        half = sessions / 2
        for (c in cnt) {
            split(c, p, SUBSEP)
            if (p[2] == "mutex" || p[2] == "spsc") {
                if (cnt[c] != half) {
                    printf "FATAL: cell %s has %s first in %d sessions, expected %d\n",
                           p[1], p[2], cnt[c], half > "/dev/stderr"
                    bad = 1
                }
            }
        }
        if (bad) exit 1
    }
' "$OUT"/raw/*.csv || invariant_fail "raw dataset does not satisfy the Phase-2 invariants"

{
    echo "# Experiment 02 Phase 2 — dataset invariants (all must hold)"
    echo "# checked: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "expected_processes=$EXPECTED_PROCESSES"
    echo "recorded_processes=$recorded"
    echo "expected_raw_files=$EXPECTED_RAW_FILES"
    echo "raw_files=$raw_count"
    echo "measured_reps_per_process=$REPS"
    echo "sessions_per_cell=$SESSIONS"
    echo "implementation_first_per_cell=$(( SESSIONS / 2 )) each"
    echo "balanced_implementation_order=PASS"
    echo "every_row_correctness=PASS"
    echo "checksum_stable_within_cell=PASS"
    echo "summary_matches_raw=PASS"
    echo "all_invariants=PASS"
} >"$OUT/invariants.txt"

echo "    $EXPECTED_RAW_FILES raw files, $REPS measured rows each, all PASS"
echo "    $SESSIONS sessions per cell, each implementation first exactly $(( SESSIONS / 2 )) times"
echo "    balanced AB/BA order verified from command.txt (see run_order.txt)"

# ---- per-process/session statistics (no new measurement happens here) ------
# Pool EVERY measured repetition of EVERY session for a cell and recompute the
# median/min/max over the pool. Nothing is dropped and no session is preferred;
# pooled_reps is printed so a cell that lost a session cannot pass unnoticed.
# NOTE: this pools repetitions that are NOT independent process placements —
# each process contributes 5 correlated repetitions. The paired-session analysis
# below is the primary comparison; this pooled view is secondary and descriptive.
pool_cell() {
    local cell_id="$1"
    awk -F, -v id="$cell_id" '
        {
            if ($0 ~ /^#/ || $0 == "") { next }
            split($0, c, ",")
            if (c[1] !~ /^[0-9]+$/ || c[7] == "") { next }
            impl = c[2]; mb = c[3]; cap = c[4]; mc = c[5]
            v[++n] = c[7] + 0
            pfr += c[9] + 0
            cer += c[10] + 0
            if (c[12] != "PASS") { bad = 1 }
        }
        END {
            if (n == 0) { printf "FATAL: no data rows for %s\n", id > "/dev/stderr"; exit 1 }
            for (i = 1; i <= n; i++) { sorted[i] = v[i] }
            for (i = 2; i <= n; i++) {
                key = sorted[i]; j = i - 1
                while (j >= 1 && sorted[j] > key) { sorted[j+1] = sorted[j]; j-- }
                sorted[j+1] = key
            }
            med = (n % 2 == 1) ? sorted[int((n+1)/2)] \
                               : (sorted[int(n/2)] + sorted[int(n/2)+1]) / 2
            spread = (sorted[1] > 0) ? (sorted[n] / sorted[1] - 1) * 100 : 0
            printf "%s,%s,%s,%s,%d,%s,%.6f,%.6f,%.6f,%.3f,%.3f,%d,%d,%s\n",
                   impl, mb, cap, mc, n, id, med, sorted[1], sorted[n], spread,
                   1e9 / med, pfr, cer, (bad ? "FAIL" : "PASS")
        }' "$OUT"/raw/"$cell_id"_s*.csv
}

# ---- derived matrix (no new measurement happens here) ----------------------
MATRIX="$OUT/summary.csv"
{
    echo "# Experiment 02 Phase 2 — canonical throughput matrix"
    echo "# DERIVED from the raw per-repetition CSVs in raw/, one row per cell."
    echo "# ns_per_message is END-TO-END elapsed_ns / messages delivered: it is NOT a"
    echo "# per-try_push or per-try_pop latency and NOT a one-way handoff time."
    echo "# Each cell pools the measured repetitions of $SESSIONS separate processes,"
    echo "# each of which ran exactly one implementation, in a balanced AB/BA order."
    echo "# WARNING: pooled repetitions are NOT independent placements — each process"
    echo "# contributes $REPS correlated repetitions. This is a SECONDARY descriptive"
    echo "# view; paired_summary.csv is the primary implementation comparison."
    echo "# message_count=$MESSAGES reps_per_process=$REPS sessions=$SESSIONS warmup_reps=$WARMUP"
    echo "# UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "impl,message_bytes,capacity,message_count,pooled_reps,cell,median_ns_per_message,min_ns_per_message,max_ns_per_message,spread_pct,median_messages_per_second,producer_full_retries_total,consumer_empty_retries_total,correctness"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in mutex spsc; do
            pool_cell "${impl}_b${1}_c${2}"
        done
    done
} >"$MATRIX"

# ---- human-readable matrix -------------------------------------------------
MATRIX_MD="$OUT/MATRIX.md"
{
    echo "# Experiment 02 Phase 2 — canonical throughput matrix"
    echo
    echo "DERIVED from the raw per-repetition CSVs in this directory (\`raw/\`). All"
    echo "measured repetitions of all sessions for a cell are POOLED; nothing is"
    echo "dropped and no session is preferred. \`ns/msg\` is END-TO-END elapsed /"
    echo "messages delivered — not a per-call latency and not a one-way handoff time."
    echo
    echo "**This pooled table is secondary and descriptive.** Pooling treats the"
    echo "\`$REPS\` repetitions inside one process as if they were independent"
    echo "placements, which they are not — a repetition constructs a fresh thread"
    echo "pair, but shares its process's address space, allocator state and thermal"
    echo "history with its siblings. For implementation direction, read"
    echo "\`PAIRED_COMPARISON.md\`, which compares each mutex/SPSC process pair inside"
    echo "the session where they ran adjacently, under a balanced AB/BA order."
    echo
    echo "- message_count: $MESSAGES per repetition"
    echo "- measured repetitions: $REPS per process x $SESSIONS processes per cell"
    echo "- warm-up repetitions (excluded from all reported data): $WARMUP per process"
    echo "- UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "| impl | bytes | capacity | pooled reps | median ns/msg | min | max | spread | median msg/s | full retries | empty retries |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in mutex spsc; do
            pool_cell "${impl}_b${1}_c${2}" | awk -F, '{
                printf "| %s | %s | %s | %s | %s | %s | %s | %s%% | %s | %s | %s |\n",
                       $1, $2, $3, $5, $7, $8, $9, $10, $11, $12, $13 }'
        done
    done
} >"$MATRIX_MD"

# ---- per-process/session medians ------------------------------------------
SESSION_MD="$OUT/SESSIONS.md"
{
    echo "# Experiment 02 Phase 2 — per-process/session medians"
    echo
    echo "Each session is one process; each process ran exactly one implementation"
    echo "and created a **fresh producer/consumer thread pair for every repetition**."
    echo "A session is therefore NOT a fixed thread placement: it is a grouping of"
    echo "repetitions inside one process/address-space lifetime. These are"
    echo "**per-process medians**, not fixed-placement medians."
    echo
    echo "Differences between sessions may reflect scheduler placement, thread"
    echo "migration, P/E-core selection, DVFS and thermal state, allocator/address"
    echo "placement, or background system activity. Phase 2 does not identify which"
    echo "factor caused any particular fast or slow run."
    echo
    echo "All values are ns per message, END-TO-END. Session order in each row is"
    echo "session 1..$SESSIONS; see \`command.txt\` for which implementation ran first"
    echo "in each session."
    echo
    printf '%s' "| impl | bytes | capacity |"
    for session in $(seq 1 "$SESSIONS"); do printf " session %s |" "$session"; done
    printf '%s\n' " pooled |"
    # printf would read a leading '-' in the format as an option, so pass the
    # separator row as an argument instead.
    printf '%s' "|---|---|---|"
    for session in $(seq 1 "$SESSIONS"); do printf '%s' "---|---"; done
    printf '%s\n' "---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in mutex spsc; do
            cell_id="${impl}_b${1}_c${2}"
            printf "| %s | %s | %s |" "$impl" "$1" "$2"
            for session in $(seq 1 "$SESSIONS"); do
                v="$(grep '^median_ns_per_message=' "$OUT/summaries/${cell_id}_s${session}.txt" | cut -d= -f2)"
                printf " %s |" "$v"
            done
            printf " %s |\n" "$(pool_cell "$cell_id" | awk -F, '{print $7}')"
        done
    done
} >"$SESSION_MD"

# ---- paired-session comparison (the primary implementation comparison) -----
# Because each mutex/SPSC pair for a given (bytes, capacity) now runs as two
# ADJACENT processes with the order balanced AB/BA across sessions, the ratio of
# the two processes' medians within a session is a paired observation: both sides
# saw nearly the same machine state, the same position in the run, and the same
# thermal history. This is a DESCRIPTIVE paired analysis, not a significance test.
PAIRED_CSV="$OUT/paired_summary.csv"
PAIRED_MD="$OUT/PAIRED_COMPARISON.md"

# session_bytes_cap -> first implementation, from the verified run order.
first_impl_for() {  # $1=session $2=bytes $3=cap
    awk -F'\t' -v s="$1" -v b="$2" -v c="$3" \
        '$1 == s && $2 == b && $3 == c { print $4; exit }' "$ORDER_FILE"
}
session_median() {  # $1=impl $2=bytes $3=cap $4=session
    grep '^median_ns_per_message=' "$OUT/summaries/$1_b$2_c$3_s$4.txt" | cut -d= -f2
}

{
    echo "# Experiment 02 Phase 2 — paired-session comparison"
    echo "# DERIVED from the per-process summaries in summaries/, one row per cell."
    echo "# ratio_sN = SPSC session-N median / mutex session-N median (<1 => SPSC"
    echo "# completed a message faster in that session)."
    echo "# The two processes of a pair ran ADJACENTLY with order balanced AB/BA."
    echo "# DESCRIPTIVE paired analysis — NOT a significance test."
    echo "# message_count=$MESSAGES reps_per_process=$REPS sessions=$SESSIONS"
    echo "# UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '%s\n' "message_bytes,capacity,first_impl_s1,ratio_s1,first_impl_s2,ratio_s2,first_impl_s3,ratio_s3,first_impl_s4,ratio_s4,median_paired_ratio,min_paired_ratio,max_paired_ratio,sessions_spsc_faster,sessions_mutex_faster"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        row="$bytes,$cap"
        ratios_lines=""
        spsc_faster=0
        mutex_faster=0
        for session in $(seq 1 "$SESSIONS"); do
            fi_="$(first_impl_for "$session" "$bytes" "$cap")"
            m="$(session_median mutex "$bytes" "$cap" "$session")"
            s="$(session_median spsc  "$bytes" "$cap" "$session")"
            r="$(awk -v a="$s" -v b="$m" 'BEGIN { printf "%.6f", a / b }')"
            row="$row,$fi_,$r"
            ratios_lines="${ratios_lines}${r}"$'\n'
            # A ratio of exactly 1 is counted for neither side, so a tie can
            # never be silently folded into a directional claim.
            if awk -v r="$r" 'BEGIN { exit !(r < 1) }'; then
                spsc_faster=$(( spsc_faster + 1 ))
            elif awk -v r="$r" 'BEGIN { exit !(r > 1) }'; then
                mutex_faster=$(( mutex_faster + 1 ))
            fi
        done
        stats="$(printf '%s' "$ratios_lines" | sort -n | awk '
            { v[++n] = $1 }
            END {
                med = (n % 2 == 1) ? v[int((n+1)/2)] : (v[n/2] + v[n/2+1]) / 2
                printf "%.6f,%.6f,%.6f", med, v[1], v[n]
            }')"
        echo "$row,$stats,$spsc_faster,$mutex_faster"
    done
} >"$PAIRED_CSV"

{
    echo "# Experiment 02 Phase 2 — paired-session comparison"
    echo
    echo "## Why this, and not the pooled table"
    echo
    echo "Under the balanced AB/BA design, the mutex process and the SPSC process for"
    echo "a given \`(message_bytes, capacity)\` run as **adjacent processes**, with"
    echo "implementation order swapped between sessions. Both sides of a pair"
    echo "therefore ran at nearly the same point in the run, on the same machine"
    echo "state. The ratio of their per-session medians is a **paired observation**."
    echo
    echo "Pooling the raw repetitions instead would treat the $REPS repetitions"
    echo "inside one process as independent placements. They are not: a repetition"
    echo "does create a fresh producer/consumer thread pair, but it shares its"
    echo "process's address space, allocator state and thermal history with its"
    echo "siblings. The pooled view is kept in \`MATRIX.md\` as a **secondary,"
    echo "descriptive** metric."
    echo
    echo "## Per-session detail"
    echo
    echo "\`ratio\` = SPSC median / mutex median within that session; **< 1 means SPSC"
    echo "completed a message faster** in that session. \`first\` is the implementation"
    echo "that ran first in that session's pair."
    echo
    echo "| bytes | capacity | session | first | mutex ns/msg | spsc ns/msg | ratio |"
    echo "|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        for session in $(seq 1 "$SESSIONS"); do
            fi_="$(first_impl_for "$session" "$bytes" "$cap")"
            m="$(session_median mutex "$bytes" "$cap" "$session")"
            s="$(session_median spsc  "$bytes" "$cap" "$session")"
            r="$(awk -v a="$s" -v b="$m" 'BEGIN { printf "%.4f", a / b }')"
            printf "| %s | %s | %s | %s | %s | %s | %s |\n" "$bytes" "$cap" "$session" "$fi_" "$m" "$s" "$r"
        done
    done
    echo
    echo "## Per-cell summary across the $SESSIONS session ratios"
    echo
    echo "MEDIAN / MIN / MAX are over the $SESSIONS paired ratios. SIGN counts how many"
    echo "sessions put SPSC ahead (< 1) and how many put the mutex baseline ahead"
    echo "(> 1). A cell whose $SESSIONS ratios do not all point the same way is"
    echo "**not directionally stable**, whatever its median ratio says. This is a"
    echo "descriptive criterion, not a significance test."
    echo
    echo "| bytes | capacity | median ratio | min | max | SPSC faster | mutex faster | sign consistency |"
    echo "|---|---|---|---|---|---|---|---|"
    awk -F, '
        /^#/ { next }
        $1 == "message_bytes" { next }   # column-header row
        NF < 15 { next }
        {
            if ($14 == 4)        { verdict = "stable (SPSC 4/4)" }
            else if ($15 == 4)   { verdict = "stable (mutex 4/4)" }
            else if ($14 > 0 && $15 > 0) { verdict = "SPLIT " $14 "-" $15 }
            else                 { verdict = "mixed" }
            printf "| %s | %s | %s | %s | %s | %s | %s | %s |\n",
                   $1, $2, $11, $12, $13, $14, $15, verdict
        }' "$PAIRED_CSV"
    echo
    echo "## Reading these numbers"
    echo
    echo "- **MEDIAN RATIO** is the headline paired figure: < 1 favours SPSC, > 1"
    echo "  favours the mutex baseline."
    echo "- **MIN / MAX** show whether that median is representative or is averaging"
    echo "  over sessions that disagreed."
    echo "- **SIGN CONSISTENCY** is the stability test. \`stable\` means all $SESSIONS"
    echo "  sessions agreed on the direction; \`SPLIT\` means they did not, and no"
    echo "  directional claim should be made for that cell regardless of the median."
    echo "- These are **descriptive** statistics over $SESSIONS paired observations"
    echo "  per cell. They are not a hypothesis test and no p-value is implied."
    echo
    echo "## Limitations"
    echo
    echo "- $SESSIONS paired observations per cell is a small sample; \`stable\` means"
    echo "  \"$SESSIONS out of $SESSIONS agreed here\", not \"the effect is proven\"."
    echo "- Adjacency narrows but does not eliminate drift: the two processes are"
    echo "  still separated by one full benchmark run (tens of seconds)."
    echo "- No CPU pinning or affinity is used or claimed; macOS may migrate threads"
    echo "  mid-run and may place the two processes' threads on different core types."
    echo "- Phase 2 does not attribute any difference to cache-line interference."
    echo "  False sharing in the unpadded Phase-1 layout remains a Phase-3 hypothesis."
} >"$PAIRED_MD"

# ---- host / toolchain metadata --------------------------------------------
if [[ -x scripts/collect-macos-profile-metadata.sh ]]; then
    {
        echo "# Experiment 02 Phase 2 — host and toolchain metadata"
        echo
        echo "Measured with \`spsc_throughput_bench\` built from repo HEAD"
        echo "\`$(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')\`."
        echo
        echo "**Scheduling limitation:** this is Apple Silicon/macOS. No hard CPU"
        echo "pinning or affinity is implemented or claimed; scheduler placement,"
        echo "P-core/E-core placement, migration, frequency and system load can all"
        echo "influence these concurrent measurements. See docs/SPSC_THROUGHPUT.md."
        echo
    } >"$OUT/HOST.md"
    if ! scripts/collect-macos-profile-metadata.sh "$OUT/HOST.md" 2>/dev/null; then
        echo "(metadata collection reported an error; see the block above)" >>"$OUT/HOST.md"
    fi
else
    {
        echo "# Experiment 02 Phase 2 — host and toolchain metadata"
        echo
        echo "scripts/collect-macos-profile-metadata.sh not found; recording minimum."
        echo "uname: $(uname -a)"
        echo "date:  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$OUT/HOST.md"
fi

echo
echo "==> Done."
echo "    raw per-repetition data : $OUT/raw/           ($EXPECTED_RAW_FILES files)"
echo "    per-process summaries   : $OUT/summaries/     ($EXPECTED_RAW_FILES files)"
echo "    benchmark stderr        : $OUT/stderr/"
echo "    invariants              : $OUT/invariants.txt"
echo "    verified run order      : $ORDER_FILE"
echo "    matrix (derived)        : $MATRIX"
echo "    matrix (human)          : $MATRIX_MD"
echo "    per-session medians     : $SESSION_MD"
echo "    paired comparison (csv) : $PAIRED_CSV"
echo "    paired comparison (md)  : $PAIRED_MD"
echo "    exact commands          : $OUT/command.txt"
echo "    host/toolchain          : $OUT/HOST.md"
echo
echo "    Interpretation belongs in docs/SPSC_THROUGHPUT.md — not in this script."
