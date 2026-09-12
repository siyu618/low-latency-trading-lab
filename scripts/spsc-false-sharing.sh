#!/usr/bin/env bash
# Experiment 02 Phase 3A — canonical CONTROLLED CURSOR-PLACEMENT runner.
#
# Runs the complete 2 x 3 x 3 matrix of the Phase-3A cursor-placement benchmark:
#
#   cursor_layout x message_bytes x capacity
#   same_line,     x 8,32,64       x 1024,4096,65536   = 18 cells
#   separated
#
# ONE IMPLEMENTATION PER PROCESS. Every process is a SEPARATE invocation of
# spsc_false_sharing_bench choosing exactly one --impl; no two layouts are ever
# timed inside one interval, one address space, or one warmed-up process state.
#
# THE ONE VARIABLE. Phase 3A holds the algorithm fixed and varies cursor
# cache-line placement and nothing else. The two variants come from ONE algorithm
# body parameterised by a cursor layout policy
# (include/spsc_cursor_layout_ring_buffer.h), so the algorithm, the payload
# storage and indexing, the full/empty semantics, the memory orders, the retry
# policy and the message types are identical by construction. There is no cached
# remote cursor, no batching, no CAS and no affinity anywhere in this run. Those
# are Phase 3B and later, and combining any of them here would destroy the
# attribution this dataset exists to support.
#
# THE ONE VARIABLE ALSO REQUIRES AN EQUAL FOOTPRINT (Phase 3A.1). The two cursor
# policies both occupy 2 x 128 = 256 bytes, so the payload array declared after
# them starts at the SAME object offset in both variants. Earlier, the same-line
# policy was 128 bytes and the separated policy 256, which moved the payload
# between cache sets as well as moving the cursors — two changed variables at
# once. `same_line` keeps BOTH cursors in the first line and reserves an inert
# second line purely to match the footprint. The benchmark refuses to time a cell
# whose two variants disagree on object size or payload offset, and leg 3
# re-checks the recorded footprint columns of every raw row.
#
# WHAT A SAME-LINE vs SEPARATED DIFFERENCE MEASURES. The cursors are not purely
# independent write-only state: the producer also READS tail and the consumer
# also READS head, at the two gates. That required true sharing is present in
# both variants. Separating the cursors removes the unwanted line-granularity
# interference between the two independent own-cursor WRITES, but also gives up
# any benefit of keeping the two shared cursor values on one line. A measured
# difference is therefore CONTROLLED CURSOR PLACEMENT, not "pure false-sharing
# cost"; a separated-faster result is consistent with reduced false-sharing
# interference and does not prove that false sharing caused the whole gap. This
# script has no hardware counters and cannot decompose the two.
#
# THE FROZEN NATURAL BASELINE IS NOT A CONTROL HERE. The frozen Phase-1/2
# `SpscRingBuffer` (unpadded, adjacent cursors) is the historical natural
# baseline and stays frozen; its canonical numbers live in
# docs/results/spsc-throughput/. It is available as `--impl=natural` for
# OBSERVATIONAL use only, and is excluded from the canonical matrix because
# adjacency is not by itself evidence of same-line placement — Phase 2 had no
# address verification at all. Set OBSERVATIONAL_NATURAL=1 to add it as a
# clearly-labelled extra row; even then it is NOT part of the causal comparison.
#
# RUNTIME LAYOUT VERIFICATION IS MANDATORY, NOT DECORATIVE
#   The whole comparison rests on the same-line variant really being same-line
#   and the separated variant really being separated ON THE OBJECT THAT RAN, and
#   on the payload starting at the same offset in both.
#   * The benchmark queries the host's cache-line size at runtime. The canonical
#     M3 Max reports 128 bytes — not the 64 most code assumes.
#   * That reported size and the compile-time alignment produce the intended
#     CANDIDATE layout; the MEASURED addresses of the cursors and payload of the
#     object that actually ran are what decide whether the controls hold.
#   * If the reported size exceeds the compile-time layout assumption, the
#     benchmark exits non-zero BEFORE timing anything, and this script stops:
#     beyond that size the compile-time alignment can no longer keep the
#     separated control's blocks in distinct real blocks.
#   * For every repetition it measures the actual cursor addresses, converts them
#     to line indices under the reported size, and requires the layout invariant
#     (same-line: equal; separated: different) plus cursor/payload line
#     disjointness. A violation aborts the process and nothing is published.
#   * Before timing, it also instantiates BOTH policies for the cell's message
#     type and capacity and requires their object size and payload offset to
#     agree.
#   * This script independently re-checks those recorded columns in every raw
#     CSV before any summary is derived (leg 3 below).
#
# BALANCED AB/BA LAYOUT ORDER (why there are exactly 4 sessions)
#   On an unpinned macOS host — where scheduler placement, thread migration,
#   P/E-core selection, DVFS, thermal state and background load all drift — a
#   layout difference and a time-of-run difference would otherwise be
#   indistinguishable. The two layouts of the SAME (message_bytes, capacity)
#   pair are therefore run as ADJACENT processes, with both the layout order and
#   the traversal direction balanced across four sessions:
#
#     session 1: forward traversal, same_line -> separated
#     session 2: reverse traversal, separated -> same_line
#     session 3: forward traversal, separated -> same_line
#     session 4: reverse traversal, same_line -> separated
#
#   Every (bytes, capacity) pair therefore receives 2 same_line-first and 2
#   separated-first comparisons. The order is FIXED and DETERMINISTIC and is
#   recorded in command.txt as it runs; the balance is then VERIFIED by parsing
#   that record back, not asserted from the constants above.
#
# MEASUREMENT DISCIPLINE
#   * Release build, -O3 -DNDEBUG, forced by the CMake target itself.
#   * Thread creation, queue allocation, the expected-checksum pre-pass and all
#     address/layout reporting happen outside the timed interval; the consumer
#     records t1 itself.
#   * ns_per_message is END-TO-END elapsed / messages DELIVERED. It is NOT a
#     per-call latency and NOT a one-way handoff time.
#   * Every repetition constructs a FRESH producer/consumer thread pair AND a
#     fresh queue object, so cursor addresses are re-verified every repetition.
#   * One warm-up repetition per process is EXCLUDED from every published and
#     derived figure. All measured repetitions are kept — this script never
#     drops a slow run and never selects a best run.
#   * Every cell must self-validate (all N delivered, exact FIFO sequence,
#     checksum matching an independently precomputed stream). A failing cell is
#     NOT published: the benchmark exits non-zero and this script stops.
#
# PRESERVE-BEFORE-SUMMARIZE. Every raw per-repetition CSV is written first,
# untouched. Only after every process has finished does the script derive the
# summaries, and it then RE-VERIFIES each process summary against the raw CSV it
# claims to summarize. No number here is ever the output of a second,
# independent run.
#
# RAW OUTPUTS ARE NEVER FABRICATED OR EDITED. This script only invokes the
# benchmark and records what the machine measured. If the host is not the
# canonical development host, the results are still real — they are simply not
# the canonical M3 Max dataset, and HOST.md records what actually ran.
#
# Usage:
#   scripts/spsc-false-sharing.sh                # canonical balanced matrix
#   FORCE=1 scripts/spsc-false-sharing.sh        # overwrite an existing dir
#   MESSAGES=200000 REPS=3 scripts/spsc-false-sharing.sh   # smoke-sized matrix
#   OBSERVATIONAL_NATURAL=1 scripts/spsc-false-sharing.sh  # + natural extra rows
#
# Env overrides:
#   MESSAGES  messages per repetition (default 10000000)
#   REPS      measured repetitions per process (default 5, all kept)
#   WARMUP    warm-up repetitions per process, excluded (default 1)
#   OUT       results directory (default docs/results/spsc-false-sharing)
#   FORCE     1 = allow writing into a non-empty OUT directory
#   BUILDDIR  build directory (default build-spsc-false-sharing)
#   OBSERVATIONAL_NATURAL  1 = also run the frozen natural baseline (extra rows,
#                           never part of the causal comparison; default 0)
# SESSIONS is NOT overridable: the AB/BA balance is defined for exactly 4.

set -euo pipefail
cd "$(dirname "$0")/.."

REPO_ROOT="$(pwd)"
BIN_NAME="spsc_false_sharing_bench"

MESSAGES="${MESSAGES:-10000000}"
REPS="${REPS:-5}"
WARMUP="${WARMUP:-1}"
OUT="${OUT:-docs/results/spsc-false-sharing}"
FORCE="${FORCE:-0}"
BUILDDIR="${BUILDDIR:-build-spsc-false-sharing}"
OBSERVATIONAL_NATURAL="${OBSERVATIONAL_NATURAL:-0}"

# Recorded in command.txt, so it must reflect the directory actually used.
BENCH_REL="$BUILDDIR/$BIN_NAME"

# The balanced design is defined for exactly four sessions (2 same_line-first +
# 2 separated-first, 2 forward + 2 reverse). Anything else silently loses the
# balance guarantee, so it is not offered as an option.
SESSIONS=4
CELLS_PER_SESSION=18
EXPECTED_PROCESSES=$(( CELLS_PER_SESSION * SESSIONS ))   # 72
EXPECTED_RAW_FILES=$EXPECTED_PROCESSES

# The two Phase-3A controls, in forward traversal order.
IMPLS=(same_line separated)

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
    *spsc-throughput*|*phase2-m3max*|*phase3-*|*phase4-*|*orderbook-bitmap-optimization*)
        echo "FATAL: refusing to write into the frozen results directory $OUT" >&2
        exit 1
        ;;
    *spsc-false-sharing-pre3a1*)
        # The pre-3A.1 dataset is retained evidence for the payload-offset
        # confound. It is real data, it is superseded, and it must never be
        # overwritten by a rerun.
        echo "FATAL: refusing to write into the archived superseded dataset $OUT" >&2
        exit 1
        ;;
esac

mkdir -p "$OUT/raw" "$OUT/summaries" "$OUT/stderr"

# ---------------------------------------------------------------------------
# Build — fresh Release dir, no stale cache, no arch flags unless asked for
# ---------------------------------------------------------------------------
echo "==> Configuring (Release, fresh $BUILDDIR)"
rm -rf "$BUILDDIR"
# The exact flags are captured below and recorded in PROVENANCE.md: BENCH_ARCH_FLAGS
# is deliberately EMPTY for the canonical run, so the toolchain's own -O3 must be
# the only optimization setting in play.
BUILD_TYPE="Release"
ARCH_FLAGS=""
cmake -S . -B "$BUILDDIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DBENCH_ARCH_FLAGS="$ARCH_FLAGS" >/dev/null

echo "==> Building $BIN_NAME"
cmake --build "$BUILDDIR" --target "$BIN_NAME" -j >/dev/null

BENCH="$BUILDDIR/$BIN_NAME"
if [[ ! -x "$BENCH" ]]; then
    echo "FATAL: $BENCH was not built" >&2
    exit 1
fi

# Fail fast: a tiny smoke cell through BOTH Phase-3A layouts before the real
# matrix. This is the only automatic benchmark execution; the canonical cells
# are never run by ctest.
echo "==> Smoke check (small, discarded)"
for impl in "${IMPLS[@]}"; do
    "$BENCH" --impl="$impl" --message-bytes=8 --capacity=1024 \
             --messages=20000 --reps=1 --warmup=0 >/dev/null
done
echo "    smoke check passed for both Phase-3A layouts"

# ---------------------------------------------------------------------------
# Session design — fixed, deterministic, and recorded as it runs
# ---------------------------------------------------------------------------
# SESSION_FIRST[session] = layout that runs first in that session.
# Traversal is forward for odd sessions, reverse for even sessions.
session_first_impl() {
    case "$1" in
        1) echo same_line ;;
        2) echo separated ;;
        3) echo separated ;;
        4) echo same_line ;;
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
echo "    9 bytes/capacity pairs x 2 cursor layouts x $SESSIONS sessions = $EXPECTED_PROCESSES processes"
echo "    messages=$MESSAGES reps=$REPS warmup=$WARMUP (warm-up excluded from all reported data)"

: >"$OUT/command.txt"
{
    echo "# Experiment 02 Phase 3A canonical run — exact effective invocations, IN ORDER"
    echo "# UTC:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# local:     $(date +%Y-%m-%dT%H:%M:%S%z)"
    echo "# repo HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "# messages=$MESSAGES reps=$REPS warmup=$WARMUP sessions=$SESSIONS"
    echo "#"
    echo "# BALANCED AB/BA DESIGN (fixed, deterministic, never randomized):"
    echo "#   session 1: forward traversal, same_line -> separated"
    echo "#   session 2: reverse traversal, separated -> same_line"
    echo "#   session 3: forward traversal, separated -> same_line"
    echo "#   session 4: reverse traversal, same_line -> separated"
    echo "# Each bytes/capacity pair therefore gets 2 same_line-first and 2"
    echo "# separated-first comparisons, with forward/reverse traversal balancing"
    echo "# time order."
    echo "#"
    echo "# Each line below is a SEPARATE process with exactly ONE cursor layout."
    echo "# The ONLY variable between the two layouts is cursor cache-line placement."
    echo
} >>"$OUT/command.txt"

for session in $(seq 1 "$SESSIONS"); do
    first_impl="$(session_first_impl "$session")"
    if [[ "$first_impl" == "same_line" ]]; then second_impl=separated; else second_impl=same_line; fi
    traversal="$(session_traversal "$session")"

    {
        echo
        echo "# ---- session $session/$SESSIONS | $traversal traversal | layout order: $first_impl then $second_impl ----"
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
            # publishes it. This covers BOTH an unsupported host cache-line size
            # (exit 3, before any timing) and a layout-invariant violation
            # (exit 3, before that repetition's timed interval).
            "$BENCH" "--impl=$impl" --message-bytes="$bytes" --capacity="$cap" \
                     --messages="$MESSAGES" --reps="$REPS" --warmup="$WARMUP" \
                     --raw-out="$raw" --summary-out="$summary" \
                     >/dev/null 2>"$OUT/stderr/${cell_id}_s${session}.txt"
        done
    done
done
echo
echo "==> All $EXPECTED_PROCESSES processes completed and validated (raw output preserved)"

# ---------------------------------------------------------------------------
# Leg 1b — OPTIONAL observational natural baseline. Clearly separated from the
# canonical matrix: extra processes, extra files, never part of the causal
# comparison. Run only when asked for.
# ---------------------------------------------------------------------------
if [[ "$OBSERVATIONAL_NATURAL" == "1" ]]; then
    echo
    echo "==> OBSERVATIONAL extra rows: frozen natural (unpadded) Phase-1 SPSC"
    echo "    These are NOT controls and are NOT part of the causal comparison."
    {
        echo
        echo "# ---- OBSERVATIONAL EXTRA ROWS: frozen natural Phase-1 SPSC ----"
        echo "# NOT a Phase-3A control. It exposes no cursor addresses, so its layout"
        echo "# is NOT verified and it can make no cache-line claim. Observational only."
    } >>"$OUT/command.txt"

    for session in $(seq 1 "$SESSIONS"); do
        traversal="$(session_traversal "$session")"
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
            cell_id="natural_b${bytes}_c${cap}"
            raw="$OUT/observational-raw/${cell_id}_s${session}.csv"
            summary="$OUT/observational-summaries/${cell_id}_s${session}.txt"
            mkdir -p "$OUT/observational-raw" "$OUT/observational-summaries"

            cmd=("$BENCH_REL" "--impl=natural" "--message-bytes=$bytes" "--capacity=$cap"
                 "--messages=$MESSAGES" "--reps=$REPS" "--warmup=$WARMUP"
                 "--raw-out=$raw" "--summary-out=$summary")
            for a in "${cmd[@]}"; do printf '%q ' "$a"; done >>"$OUT/command.txt"
            printf '\n' >>"$OUT/command.txt"

            "$BENCH" --impl=natural --message-bytes="$bytes" --capacity="$cap" \
                     --messages="$MESSAGES" --reps="$REPS" --warmup="$WARMUP" \
                     --raw-out="$raw" --summary-out="$summary" \
                     >/dev/null 2>"$OUT/stderr/${cell_id}_s${session}.txt"
        done
    done
    echo "    observational natural rows complete (excluded from summary.csv)"
fi

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
            while ((getline rline < rawfile) > 0) {
                if (rline ~ /^#/ || rline == "") { continue }
                split(rline, c, ",")
                if (c[1] ~ /^[0-9]+$/ && c[7] != "") { v[++n] = c[7] + 0 }
            }
            close(rawfile)
            if (n == 0) {
                printf "FATAL: %s raw CSV has no data rows\n", id > "/dev/stderr"
                bad = 1
                exit
            }
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
        for impl in "${IMPLS[@]}"; do
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
        s = $4; sub(/\/.*/, "", s); next
    }
    /^# ---- OBSERVATIONAL/ { in_obs = 1; s = ""; next }
    /--impl=/ {
        impl = ""; mb = ""; cap = ""
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^--impl=/)          { split($i, a, "="); impl = a[2] }
            if ($i ~ /^--message-bytes=/) { split($i, a, "="); mb   = a[2] }
            if ($i ~ /^--capacity=/)      { split($i, a, "="); cap  = a[2] }
        }
        if (in_obs) { next }
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

# (a) exactly EXPECTED_PROCESSES canonical invocations were recorded
recorded="$(wc -l <"$ORDER_FILE" | tr -d ' ')"
[[ "$recorded" == "$EXPECTED_PROCESSES" ]] \
    || invariant_fail "command.txt records $recorded canonical invocations, expected $EXPECTED_PROCESSES"

# (b) the raw CSVs on disk must be EXACTLY the set implied by the recorded
#     execution order — no missing cell, no extra file, no renamed cell. This is
#     what ties the bytes on disk to the run that produced them.
expected_list="$(awk -F'\t' '{ printf "%s_b%s_c%s_s%s.csv\n", $4, $2, $3, $1 }' \
    "$ORDER_FILE" | sort)"
actual_list="$(cd "$OUT/raw" && ls *.csv | sort)"
[[ "$expected_list" == "$actual_list" ]] \
    || invariant_fail "raw/ does not contain exactly the cells the recorded execution order implies"
raw_count="$(printf '%s\n' "$actual_list" | wc -l | tr -d ' ')"
[[ "$raw_count" == "$EXPECTED_RAW_FILES" ]] \
    || invariant_fail "found $raw_count raw CSVs, expected $EXPECTED_RAW_FILES"

# (c) every raw CSV has exactly REPS measured rows, all correctness=PASS, one
#     stable checksum, and rows that agree with each other about what cell they
#     are;
# (d) EVERY measured row carries a PASSING runtime layout verification, and the
#     measured placement MATCHES the layout the row claims. This is the check
#     that makes the dataset evidence about cache-line placement at all.
awk -F, -v reps="$REPS" -v messages="$MESSAGES" '
    FNR == 1 {
        if (prev != "" && prev_n != reps) {
            printf "FATAL: %s has %d measured rows, expected %d\n", prev, prev_n, reps > "/dev/stderr"
            bad = 1
        }
        prev = FILENAME; prev_n = 0; prev_ck = ""
        f_impl = ""; f_mb = ""; f_cap = ""
    }
    /^#/ || /^rep,/ || NF == 0 { next }
    {
        if ($1 !~ /^[0-9]+$/) { next }
        prev_n++
        # every row must describe the SAME cell as the first row of its file
        if (f_impl == "") { f_impl = $2; f_mb = $3; f_cap = $4 }
        else if ($2 != f_impl || $3 != f_mb || $4 != f_cap) {
            printf "FATAL: %s row %s disagrees with its file about the cell\n", FILENAME, $1 > "/dev/stderr"
            bad = 1
        }
        if ($5 + 0 != messages + 0) {
            printf "FATAL: %s row %s ran %s messages, expected %s\n", FILENAME, $1, $5, messages > "/dev/stderr"
            bad = 1
        }
        if ($12 != "PASS") {
            printf "FATAL: %s has a non-PASS correctness row: %s\n", FILENAME, $0 > "/dev/stderr"
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
        # ---- Phase-3A layout evidence, re-checked here independently ----
        # Columns: 13 reported_cache_line_size, 14 head_addr, 15 tail_addr,
        #          16 head_line, 17 tail_line, 18 cursors_same_line, 19 layout_ok
        if ($19 != "PASS") {
            printf "FATAL: %s row %s has layout_ok=%s (layout not verified)\n", FILENAME, $1, $19 > "/dev/stderr"
            bad = 1
        }
        if ($13 + 0 <= 0) {
            printf "FATAL: %s row %s has no reported cache-line size\n", FILENAME, $1 > "/dev/stderr"
            bad = 1
        }
        if (clsize == "") { clsize = $13 }
        else if ($13 != clsize) {
            printf "FATAL: %s reports cache-line size %s, earlier files reported %s\n", FILENAME, $13, clsize > "/dev/stderr"
            bad = 1
        }
        if ($16 == "" || $17 == "" || $16 == "NA") {
            printf "FATAL: %s row %s has no cursor line indices\n", FILENAME, $1 > "/dev/stderr"
            bad = 1
        }
        # The MEASURED placement must match the placement the row claims. Both
        # directions are checked, so neither control can pass by accident.
        if ($2 == "same_line") {
            if ($18 != "yes" || $16 + 0 != $17 + 0) {
                printf "FATAL: %s row %s claims same_line but measured lines %s/%s (same=%s)\n", FILENAME, $1, $16, $17, $18 > "/dev/stderr"
                bad = 1
            }
        } else if ($2 == "separated") {
            if ($18 != "no" || $16 + 0 == $17 + 0) {
                printf "FATAL: %s row %s claims separated but measured lines %s/%s (same=%s)\n", FILENAME, $1, $16, $17, $18 > "/dev/stderr"
                bad = 1
            }
        } else {
            printf "FATAL: %s row %s has unexpected impl %s in the canonical matrix\n", FILENAME, $1, $2 > "/dev/stderr"
            bad = 1
        }
    }
    END {
        if (prev != "" && prev_n != reps) {
            printf "FATAL: %s has %d measured rows, expected %d\n", prev, prev_n, reps > "/dev/stderr"
            bad = 1
        }
        if (bad) exit 1
    }
' "$OUT"/raw/*.csv || invariant_fail "raw dataset does not satisfy the Phase-3A invariants"

# (d2) Phase 3A.1 EQUAL FOOTPRINT. The two cursor policies must produce queue
#     objects of the same size with the payload at the same offset, for the same
#     message size and capacity, or the run changes cursor placement AND payload
#     layout at once. Checked here from the raw columns the benchmark wrote on
#     the objects it ACTUALLY measured — per row, and across the two layouts of
#     every cell. The benchmark refuses to time a cell that fails this, so a
#     failure here means the workspace was tampered with, not that the run was
#     subtly wrong.
FOOTPRINTS="$(awk -F, -v messages="$MESSAGES" '
    /^#/ || /^rep,/ || NF == 0 { next }
    $1 !~ /^[0-9]+$/ { next }
    {
        # Columns: 20 object_addr, 21 object_size, 22 payload_offset,
        #          23 payload_begin_addr
        if ($20 == "" || $20 == "NA" || $21 == "" || $21 == "NA" ||
            $22 == "" || $22 == "NA" || $23 == "" || $23 == "NA") {
            printf "FATAL: %s row %s has no footprint evidence (object_addr/object_size/payload_offset/payload_begin_addr)\n",
                   FILENAME, $1 > "/dev/stderr"
            bad = 1
            next
        }
        if ($21 + 0 <= 0) {
            printf "FATAL: %s row %s has object_size=%s\n", FILENAME, $1, $21 > "/dev/stderr"
            bad = 1
        }
        if ($22 + 0 <= 0) {
            printf "FATAL: %s row %s has payload_offset=%s\n", FILENAME, $1, $22 > "/dev/stderr"
            bad = 1
        }
        # The reported offset must be the real one: payload start minus object base.
        if ($23 + 0 - ($20 + 0) != $22 + 0) {
            printf "FATAL: %s row %s payload_begin_addr - object_addr = %d but payload_offset says %s\n",
                   FILENAME, $1, $23 - $20, $22 > "/dev/stderr"
            bad = 1
        }
        # object = cursor policy + payload, exactly. This is what makes the
        # offset the ONLY thing that could differ between the variants.
        if ($21 + 0 - ($22 + 0) != $3 * $4) {
            printf "FATAL: %s row %s object_size - payload_offset = %d but payload is %d bytes\n",
                   FILENAME, $1, $21 - $22, $3 * $4 > "/dev/stderr"
            bad = 1
        }
        # Every row must agree with every other row of the same layout for the
        # same cell: the layout is a property of the type, so a varying value
        # would mean the wrong object was reported.
        k = $3 "_" $4 SUBSEP $2
        if (k in seen) {
            if (osize[k] != $21 || poff[k] != $22) {
                printf "FATAL: %s row %s footprint disagrees with other %s rows of the same cell\n",
                       FILENAME, $1, $2 > "/dev/stderr"
                bad = 1
            }
        } else {
            seen[k] = 1; osize[k] = $21; poff[k] = $22
        }
        cell[$3 "_" $4] = 1
    }
    END {
        # Across the two layouts of a cell: identical object size, identical
        # payload offset. This is the Phase-3A.1 invariant stated directly.
        n = 0
        for (c in cell) {
            n++
            ks = c SUBSEP "same_line"
            kp = c SUBSEP "separated"
            if (!(ks in seen) || !(kp in seen)) {
                printf "FATAL: cell %s is missing a layout in the footprint evidence\n", c > "/dev/stderr"
                bad = 1
                continue
            }
            if (osize[ks] != osize[kp]) {
                printf "FATAL: cell %s object_size differs: same_line=%s separated=%s\n",
                       c, osize[ks], osize[kp] > "/dev/stderr"
                bad = 1
            }
            if (poff[ks] != poff[kp]) {
                printf "FATAL: cell %s payload_offset differs: same_line=%s separated=%s\n",
                       c, poff[ks], poff[kp] > "/dev/stderr"
                bad = 1
            }
        }
        if (n != 9) {
            printf "FATAL: footprint evidence covers %d cells, expected 9\n", n > "/dev/stderr"
            bad = 1
        }
        if (bad) exit 1
        print (n == 9 ? "PASS" : "FAIL")
    }
' "$OUT"/raw/*.csv)" || invariant_fail "raw dataset does not satisfy the Phase-3A.1 equal-footprint invariants"
[[ "$FOOTPRINTS" == "PASS" ]] || invariant_fail "equal-footprint invariant not satisfied"

# The per-cell footprint table, for LAYOUT_VERIFICATION.md and the metadata.
# Iterated in the canonical forward order rather than sorted, because macOS awk
# (BWK) has no asorti and the matrix is a fixed, known 3x3.
FOOTPRINT_TABLE="$(awk -F, '
    /^#/ || /^rep,/ || NF == 0 { next }
    $1 !~ /^[0-9]+$/ { next }
    { k = $3 SUBSEP $4; if (!(k in o)) { o[k] = $21; p[k] = $22 } }
    END {
        nb = split("8 32 64", bytes, " ")
        nc = split("1024 4096 65536", caps, " ")
        for (i = 1; i <= nb; i++) {
            for (j = 1; j <= nc; j++) {
                k = bytes[i] SUBSEP caps[j]
                if (k in o) {
                    printf "%s %s %s %s\n", bytes[i], caps[j], o[k], p[k]
                } else {
                    printf "%s %s MISSING MISSING\n", bytes[i], caps[j]
                }
            }
        }
    }
' "$OUT"/raw/*.csv)"

# (e) the balanced AB/BA order, checked from the RECORDED execution order: every
#     cell must appear in every session, and each layout must run FIRST exactly
#     half the time. This is a property of what ran, not of the design constants.
BALANCED="$(awk -F'\t' -v sessions="$SESSIONS" '
    {
        key = $2 "_" $3
        cells[key] = 1
        k = key SUBSEP $1
        if (!(k in first)) { first[k] = $4 }
        seen[k] = 1
    }
    END {
        half = sessions / 2
        ok = 1
        n = 0
        for (key in cells) {
            n++
            sl = 0; sp = 0
            for (s = 1; s <= sessions; s++) {
                k = key SUBSEP s
                if (!(k in seen)) {
                    printf "FATAL: cell %s is missing session %d\n", key, s > "/dev/stderr"
                    ok = 0
                    continue
                }
                if      (first[k] == "same_line") { sl++ }
                else if (first[k] == "separated") { sp++ }
                else {
                    printf "FATAL: cell %s session %d has no valid first impl (%s)\n", key, s, first[k] > "/dev/stderr"
                    ok = 0
                }
            }
            if (sl != half || sp != half) {
                printf "FATAL: cell %s runs same_line first %d times and separated first %d times, expected %d each\n",
                       key, sl, sp, half > "/dev/stderr"
                ok = 0
            }
        }
        if (n != 9) {
            printf "FATAL: recorded execution order covers %d distinct cells, expected 9\n", n > "/dev/stderr"
            ok = 0
        }
        print (ok ? "PASS" : "FAIL")
    }' "$ORDER_FILE")"
[[ "$BALANCED" == "PASS" ]] || invariant_fail "balanced AB/BA layout order is not satisfied per cell"

# (f) the runtime cache-line size the host reported, taken from the raw data.
HOST_LINE="$(awk -F, '
    /^#/ || /^rep,/ || NF == 0 { next }
    $1 ~ /^[0-9]+$/ && $13 != "" { print $13; exit }
' "$OUT"/raw/*.csv)"

{
    echo "# Experiment 02 Phase 3A — dataset invariants (all must hold)"
    echo "# checked: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "expected_processes=$EXPECTED_PROCESSES"
    echo "recorded_processes=$recorded"
    echo "expected_raw_files=$EXPECTED_RAW_FILES"
    echo "raw_files=$raw_count"
    echo "measured_reps_per_process=$REPS"
    echo "sessions_per_cell=$SESSIONS"
    echo "cursor_layouts=2 (same_line, separated)"
    echo "layout_first_per_cell=$(( SESSIONS / 2 )) each"
    echo "balanced_layout_order=PASS"
    echo "host_reported_cache_line_size=$HOST_LINE"
    echo "compile_time_layout_assumption=128"
    echo "runtime_layout_verified_every_measured_rep=PASS"
    echo "same_line_rows_measured_same_line=PASS"
    echo "separated_rows_measured_separated=PASS"
    echo "cursor_payload_line_disjoint=PASS"
    echo "every_row_correctness=PASS"
    echo "checksum_stable_within_cell=PASS"
    echo "summary_matches_raw=PASS"
    echo "cursor_policies_equal_footprint=PASS"
    echo "same_line_and_separated_object_size_equal=PASS"
    echo "same_line_and_separated_payload_offset_equal=PASS"
    echo "payload_offset_equals_object_size_minus_payload=PASS"
    echo "all_invariants=PASS"
    echo "#"
    echo "# Phase 3A.1 equal-footprint evidence, per cell (the two layouts must"
    echo "# agree on both columns, or the payload moved when cursor placement"
    echo "# changed and the comparison changes two variables at once):"
    echo "# message_bytes capacity object_size payload_offset_from_object_base"
    printf '%s\n' "$FOOTPRINT_TABLE" | while read -r line; do echo "# $line"; done
} >"$OUT/invariants.txt"

echo "    $EXPECTED_RAW_FILES raw files, $REPS measured rows each, all PASS"
echo "    $SESSIONS sessions per cell, each layout first exactly $(( SESSIONS / 2 )) times"
echo "    balanced AB/BA order verified from command.txt (see run_order.txt)"
echo "    runtime layout verified on every measured repetition (host line size $HOST_LINE)"

# ---- per-process/session statistics (no new measurement happens here) ------
# Pool EVERY measured repetition of EVERY session for a cell and recompute the
# median/min/max over the pool. Nothing is dropped and no session is preferred;
# pooled_reps is printed so a cell that lost a session cannot pass unnoticed.
# NOTE: this pools repetitions that are NOT independent process placements —
# each process contributes $REPS correlated repetitions. The paired-session
# analysis below is the primary comparison; this pooled view is secondary.
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
    echo "# Experiment 02 Phase 3A — canonical cursor-placement matrix"
    echo "# DERIVED from the raw per-repetition CSVs in raw/, one row per cell."
    echo "# ns_per_message is END-TO-END elapsed_ns / messages delivered: it is NOT a"
    echo "# per-try_push or per-try_pop latency and NOT a one-way handoff time."
    echo "# Each cell pools the measured repetitions of $SESSIONS separate processes,"
    echo "# each of which ran exactly one cursor layout, in a balanced AB/BA order."
    echo "# WARNING: pooled repetitions are NOT independent placements — each process"
    echo "# contributes $REPS correlated repetitions. This is a SECONDARY descriptive"
    echo "# view; paired_summary.csv is the primary comparison."
    echo "# message_count=$MESSAGES reps_per_process=$REPS sessions=$SESSIONS warmup_reps=$WARMUP"
    echo "# UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "impl,message_bytes,capacity,message_count,pooled_reps,cell,median_ns_per_message,min_ns_per_message,max_ns_per_message,spread_pct,median_messages_per_second,producer_full_retries_total,consumer_empty_retries_total,correctness"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in "${IMPLS[@]}"; do
            pool_cell "${impl}_b${1}_c${2}"
        done
    done
} >"$MATRIX"

# ---- human-readable matrix -------------------------------------------------
MATRIX_MD="$OUT/MATRIX.md"
{
    echo "# Experiment 02 Phase 3A — canonical cursor-placement matrix"
    echo
    echo "DERIVED from the raw per-repetition CSVs in this directory (\`raw/\`). All"
    echo "measured repetitions of all sessions for a cell are POOLED; nothing is"
    echo "dropped and no session is preferred. \`ns/msg\` is END-TO-END elapsed /"
    echo "messages delivered — not a per-call latency and not a one-way handoff time."
    echo
    echo "**This pooled table is secondary and descriptive.** Pooling treats the"
    echo "\`$REPS\` repetitions inside one process as if they were independent"
    echo "placements, which they are not. For the \`separated\` vs \`same_line\`"
    echo "direction, read \`PAIRED_COMPARISON.md\`, which compares each pair of"
    echo "adjacent processes inside the session where they ran, under a balanced"
    echo "AB/BA order."
    echo
    echo "- message_count: $MESSAGES per repetition"
    echo "- measured repetitions: $REPS per process x $SESSIONS processes per cell"
    echo "- warm-up repetitions (excluded from all reported data): $WARMUP per process"
    echo "- every row's cursor placement was verified at runtime against the host's"
    echo "  reported cache-line size ($HOST_LINE bytes); see \`invariants.txt\`"
    echo "- UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "| impl | bytes | capacity | pooled reps | median ns/msg | min | max | spread | median msg/s | full retries | empty retries |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in "${IMPLS[@]}"; do
            pool_cell "${impl}_b${1}_c${2}" | awk -F, '{
                printf "| %s | %s | %s | %s | %s | %s | %s | %s%% | %s | %s | %s |\n",
                       $1, $2, $3, $5, $7, $8, $9, $10, $11, $12, $13 }'
        done
    done
} >"$MATRIX_MD"

# ---- per-process/session medians ------------------------------------------
SESSION_MD="$OUT/SESSIONS.md"
{
    echo "# Experiment 02 Phase 3A — per-process/session medians"
    echo
    echo "Each session is one process; each process ran exactly one cursor layout and"
    echo "created a **fresh producer/consumer thread pair and a fresh queue object for"
    echo "every repetition**. A session is therefore NOT a fixed thread placement: it"
    echo "is a grouping of repetitions inside one process/address-space lifetime."
    echo "These are **per-process medians**, not fixed-placement medians."
    echo
    echo "Differences between sessions may reflect scheduler placement, thread"
    echo "migration, P/E-core selection, DVFS and thermal state, allocator/address"
    echo "placement, or background system activity. Phase 3A does not identify which"
    echo "factor caused any particular fast or slow run."
    echo
    echo "All values are ns per message, END-TO-END. Session order in each row is"
    echo "session 1..$SESSIONS; see \`command.txt\` for which layout ran first in each"
    echo "session."
    echo
    printf '%s' "| impl | bytes | capacity |"
    for session in $(seq 1 "$SESSIONS"); do printf " session %s |" "$session"; done
    printf '%s\n' " pooled |"
    printf '%s' "|---|---|---|"
    for session in $(seq 1 "$SESSIONS"); do printf '%s' "---|---"; done
    printf '%s\n' "---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in "${IMPLS[@]}"; do
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

# ---- cursor-layout verification report -------------------------------------
# One row per measured repetition: the addresses and line indices that were
# actually observed, so the layout claim can be audited without re-running.
LAYOUT_MD="$OUT/LAYOUT_VERIFICATION.md"
{
    echo "# Experiment 02 Phase 3A — runtime cursor-layout verification"
    echo
    echo "Every measured repetition of the canonical matrix, with the cursor"
    echo "addresses and cache-line indices that were **measured on the very queue"
    echo "object that repetition timed**, outside the timed interval."
    echo
    echo "- host-reported cache-line size: **$HOST_LINE bytes**"
    echo "- compile-time layout assumption: **128 bytes**"
    echo "- \`same_line\` rows MUST show \`head_line == tail_line\`"
    echo "- \`separated\` rows MUST show \`head_line != tail_line\`"
    echo
    echo "The compile-time alignment creates the intended CANDIDATE layout. What"
    echo "decides whether the controls hold is the MEASURED address relationship"
    echo "below, read against the host's reported line size. If the host had"
    echo "reported a line size larger than the compile-time assumption the benchmark"
    echo "would have exited non-zero **before timing anything** and this file would"
    echo "not exist: past that size the compile-time alignment can no longer keep"
    echo "the \`separated\` control's two blocks in distinct real blocks, so the"
    echo "control could not be justified by construction."
    echo
    echo "## Equal footprint (Phase 3A.1)"
    echo
    echo "Cursor placement is the ONLY variable, so the payload array must start at"
    echo "the same offset from the object base in both variants — otherwise the"
    echo "payload moves between cache sets too and the comparison changes two things"
    echo "at once. Each row below lists the object size and payload offset the two"
    echo "layouts' processes reported, measured on the objects they actually ran."
    echo "They MUST agree. The benchmark refuses to time a cell that fails this, so"
    echo "these columns are a re-check of already-gated evidence."
    echo
    echo "| message bytes | capacity | object size | payload offset |"
    echo "|---|---|---|---|"
    printf '%s\n' "$FOOTPRINT_TABLE" | while read -r fb fc fsize foff; do
        echo "| $fb | $fc | $fsize | $foff |"
    done
    echo
    echo "Both cursor policies occupy \`2 * 128 = 256\` bytes, asserted at compile"
    echo "time in \`include/spsc_cursor_layout_ring_buffer.h\`. The \`same_line\` policy"
    echo "keeps BOTH cursors in the FIRST line and reserves an inert second line"
    echo "purely to match that footprint; the reserved line is never read or written"
    echo "by \`try_push\`/\`try_pop\`/\`empty\`."
    echo
    echo "## Per-repetition measured addresses"
    echo
    echo "| cell | session | rep | head addr | tail addr | head line | tail line | same line | layout ok |"
    echo "|---|---|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in "${IMPLS[@]}"; do
            cell_id="${impl}_b${1}_c${2}"
            for session in $(seq 1 "$SESSIONS"); do
                awk -F, -v id="$cell_id" -v s="$session" '
                    /^#/ || /^rep,/ || NF == 0 { next }
                    $1 ~ /^[0-9]+$/ {
                        printf "| %s | %s | %s | 0x%x | 0x%x | %s | %s | %s | %s |\n",
                               id, s, $1, $14 + 0, $15 + 0, $16, $17, $18, $19
                    }' "$OUT/raw/${cell_id}_s${session}.csv"
            done
        done
    done
} >"$LAYOUT_MD"

# ---- paired-session comparison (the PRIMARY comparison) --------------------
# Each same_line/separated pair for a given (bytes, capacity) runs as two
# ADJACENT processes with the order balanced AB/BA across sessions, so the ratio
# of the two processes' medians within a session is a paired observation. This is
# a DESCRIPTIVE paired analysis, not a significance test.
PAIRED_CSV="$OUT/paired_summary.csv"
PAIRED_MD="$OUT/PAIRED_COMPARISON.md"

# session_bytes_cap -> first layout, from the verified run order.
first_impl_for() {  # $1=session $2=bytes $3=cap
    awk -F'\t' -v s="$1" -v b="$2" -v c="$3" \
        '$1 == s && $2 == b && $3 == c { print $4; exit }' "$ORDER_FILE"
}
session_median() {  # $1=impl $2=bytes $3=cap $4=session
    grep '^median_ns_per_message=' "$OUT/summaries/$1_b$2_c$3_s$4.txt" | cut -d= -f2
}

{
    echo "# Experiment 02 Phase 3A — paired-session comparison (PRIMARY)"
    echo "# DERIVED from the per-process summaries in summaries/, one row per cell."
    echo "# ratio_sN = separated session-N median / same_line session-N median."
    echo "#   ratio < 1  => in that session, SEPARATED cursors completed a message faster"
    echo "#   ratio > 1  => in that session, SAME-LINE cursors completed a message faster"
    echo "# The two processes of a pair ran ADJACENTLY with order balanced AB/BA."
    echo "# DESCRIPTIVE paired analysis — NOT a significance test. Four paired"
    echo "# observations per cell cannot establish formal statistical significance."
    echo "# message_count=$MESSAGES reps_per_process=$REPS sessions=$SESSIONS"
    echo "# UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '%s\n' "message_bytes,capacity,first_impl_s1,ratio_s1,first_impl_s2,ratio_s2,first_impl_s3,ratio_s3,first_impl_s4,ratio_s4,median_paired_ratio,min_paired_ratio,max_paired_ratio,sessions_separated_faster,sessions_same_line_faster"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        row="$bytes,$cap"
        ratios_lines=""
        sep_faster=0
        same_faster=0
        for session in $(seq 1 "$SESSIONS"); do
            fi_="$(first_impl_for "$session" "$bytes" "$cap")"
            sl="$(session_median same_line "$bytes" "$cap" "$session")"
            sp="$(session_median separated "$bytes" "$cap" "$session")"
            r="$(awk -v a="$sp" -v b="$sl" 'BEGIN { printf "%.6f", a / b }')"
            row="$row,$fi_,$r"
            ratios_lines="${ratios_lines}${r}"$'\n'
            # A ratio of exactly 1 is counted for neither side, so a tie can
            # never be silently folded into a directional claim.
            if awk -v r="$r" 'BEGIN { exit !(r < 1) }'; then
                sep_faster=$(( sep_faster + 1 ))
            elif awk -v r="$r" 'BEGIN { exit !(r > 1) }'; then
                same_faster=$(( same_faster + 1 ))
            fi
        done
        stats="$(printf '%s' "$ratios_lines" | sort -n | awk '
            { v[++n] = $1 }
            END {
                med = (n % 2 == 1) ? v[int((n+1)/2)] : (v[n/2] + v[n/2+1]) / 2
                printf "%.6f,%.6f,%.6f", med, v[1], v[n]
            }')"
        echo "$row,$stats,$sep_faster,$same_faster"
    done
} >"$PAIRED_CSV"

{
    echo "# Experiment 02 Phase 3A — paired-session comparison"
    echo
    echo "## What is being compared"
    echo
    echo "The **only** difference between the two implementations compared here is"
    echo "the cache-line placement of the two SPSC cursors. Both come from one"
    echo "algorithm body parameterised by a cursor layout policy, so the algorithm,"
    echo "the payload storage and indexing, the full/empty semantics, the memory"
    echo "orders, the retry policy and the message types are identical by"
    echo "construction. There is no cached remote cursor, no batching, no CAS and no"
    echo "affinity in either variant."
    echo
    echo "\`ratio\` = **separated median / same_line median** within a session."
    echo "**\`ratio < 1\` means separated cursors completed a message faster** in that"
    echo "session."
    echo
    echo "## Why paired, and not the pooled table"
    echo
    echo "Under the balanced AB/BA design, the same-line process and the separated"
    echo "process for a given \`(message_bytes, capacity)\` run as **adjacent"
    echo "processes**, with implementation order swapped between sessions."
    echo "**Adjacent execution reduces temporal drift between the two legs but"
    echo "cannot guarantee identical scheduler, DVFS, thermal, or background-system"
    echo "state** — the two processes are still separated by a full benchmark run,"
    echo "and each leg can be placed, migrated or frequency-scaled independently."
    echo "The pairing narrows the gap; it does not close it."
    echo
    echo "Pooling the raw repetitions instead would treat the $REPS repetitions"
    echo "inside one process as independent placements. They are not: a repetition"
    echo "does create a fresh producer/consumer thread pair and a fresh queue object,"
    echo "but it shares its process's address space, allocator state and thermal"
    echo "history with its siblings. The pooled view is kept in \`MATRIX.md\` as a"
    echo "**secondary, descriptive** metric."
    echo
    echo "## Per-session detail"
    echo
    echo "\`first\` is the layout that ran first in that session's pair."
    echo
    echo "| bytes | capacity | session | first | same_line ns/msg | separated ns/msg | ratio |"
    echo "|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        for session in $(seq 1 "$SESSIONS"); do
            fi_="$(first_impl_for "$session" "$bytes" "$cap")"
            sl="$(session_median same_line "$bytes" "$cap" "$session")"
            sp="$(session_median separated "$bytes" "$cap" "$session")"
            r="$(awk -v a="$sp" -v b="$sl" 'BEGIN { printf "%.4f", a / b }')"
            printf "| %s | %s | %s | %s | %s | %s | %s |\n" "$bytes" "$cap" "$session" "$fi_" "$sl" "$sp" "$r"
        done
    done
    echo
    echo "## Per-cell summary across the $SESSIONS session ratios"
    echo
    echo "MEDIAN / MIN / MAX are over the $SESSIONS paired ratios. SIGN counts how many"
    echo "sessions put separated ahead (< 1) and how many put same-line ahead (> 1)."
    echo "A cell whose $SESSIONS ratios do not all point the same way is **not"
    echo "directionally stable**, whatever its median ratio says. This is a"
    echo "descriptive criterion, not a significance test."
    echo
    echo "| bytes | capacity | median ratio | min | max | separated faster | same_line faster | direction |"
    echo "|---|---|---|---|---|---|---|---|"
    awk -F, '
        /^#/ { next }
        $1 == "message_bytes" { next }
        NF < 15 { next }
        {
            if ($14 == 4)        { verdict = "stable — separated 4/4" }
            else if ($15 == 4)   { verdict = "stable — same_line 4/4" }
            else if ($14 > 0 && $15 > 0) { verdict = "SPLIT " $14 "-" $15 }
            else                 { verdict = "mixed" }
            printf "| %s | %s | %s | %s | %s | %s | %s | %s |\n",
                   $1, $2, $11, $12, $13, $14, $15, verdict
        }' "$PAIRED_CSV"
    echo
    echo "## Reading these numbers"
    echo
    echo "- **MEDIAN RATIO** is the headline paired figure: < 1 means separated was"
    echo "  faster, > 1 means same-line was faster."
    echo "- **MIN / MAX** show whether that median is representative or is averaging"
    echo "  over sessions that disagreed. They are the extremes of $SESSIONS"
    echo "  correlated observations, not a confidence interval."
    echo "- **DIRECTION STABILITY** is the attribution check. \`stable\` means all"
    echo "  $SESSIONS sessions agreed; \`SPLIT\` means they did not, and **no"
    echo "  directional claim should be made for that cell** regardless of its median."
    echo "- These are **descriptive** statistics over $SESSIONS paired observations per"
    echo "  cell. They are not a hypothesis test and no p-value is implied."
    echo
    echo "## Limitations"
    echo
    echo "- $SESSIONS paired observations per cell is a small sample; \`stable\` means"
    echo "  \"$SESSIONS out of $SESSIONS agreed here\", not \"the effect is proven\"."
    echo "- No CPU pinning or affinity is used or claimed; macOS may migrate threads"
    echo "  mid-run and may place the two processes' threads on different core types."
    echo "- The two implementations are not the same shape of code: the layout policy"
    echo "  changes cursor placement only, but the same-line process's threads both"
    echo "  touch the same line for their own cursor accesses, which is exactly the"
    echo "  effect under study."
    echo "- Padding removes FALSE sharing only. The producer must still observe the"
    echo "  consumer's tail cursor and vice versa; those remote observations are"
    echo "  required for correctness and remain in both variants. See"
    echo "  \`docs/SPSC_FALSE_SHARING.md\`."
    echo "- The frozen natural Phase-1/2 SPSC is **not** a control in this"
    echo "  comparison. It is unpadded, but adjacency is not proof of same-line"
    echo "  placement and Phase 2 recorded no cursor addresses."
} >"$PAIRED_MD"

# ---- host / toolchain metadata --------------------------------------------
if [[ -x scripts/collect-macos-profile-metadata.sh ]]; then
    {
        echo "# Experiment 02 Phase 3A — host and toolchain metadata"
        echo
        echo "Measured with \`spsc_false_sharing_bench\` built from repo HEAD"
        echo "\`$(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')\`."
        echo
        echo "**Cache-line size.** The host reports its cache-line size at runtime;"
        echo "this run recorded **$HOST_LINE bytes**. The compile-time layout"
        echo "assumption is 128 bytes, and it is the measured addresses of the actual"
        echo "cursor and payload objects — not the assumption — that decide whether"
        echo "the controls are what they claim. If the reported size had exceeded the"
        echo "assumption the benchmark would have exited non-zero before timing"
        echo "anything, since beyond that size the compile-time alignment can no"
        echo "longer keep the separated control's blocks in distinct real blocks."
        echo
        echo "**Scheduling limitation:** this is Apple Silicon/macOS. No hard CPU"
        echo "pinning or affinity is implemented or claimed; scheduler placement,"
        echo "P-core/E-core placement, migration, frequency and system load can all"
        echo "influence these concurrent measurements. See docs/SPSC_FALSE_SHARING.md."
        echo
    } >"$OUT/HOST.md"
    if ! scripts/collect-macos-profile-metadata.sh "$OUT/HOST.md" 2>/dev/null; then
        echo "(metadata collection reported an error; see the block above)" >>"$OUT/HOST.md"
    fi
else
    {
        echo "# Experiment 02 Phase 3A — host and toolchain metadata"
        echo
        echo "scripts/collect-macos-profile-metadata.sh not found; recording minimum."
        echo "uname: $(uname -a)"
        echo "date:  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$OUT/HOST.md"
fi

# ---- provenance (Phase 3A.1) ----------------------------------------------
#
# The tree is EXPECTED to be dirty when this runs: the hardened sources are the
# thing being measured and they are not necessarily committed yet. HEAD alone
# would then identify the WRONG code, so the exact bytes that produced the
# dataset are recorded as content hashes, and the dirty state is recorded
# verbatim rather than assumed clean.
PROVENANCE_MD="$OUT/PROVENANCE.md"
hash_file() {  # $1=path -> "sha256  path", or a NOT_PRESENT line
    if [[ -f "$1" ]]; then
        printf '%s  %s\n' "$(shasum -a 256 "$1" | awk '{print $1}')" "$1"
    else
        printf 'NOT_PRESENT  %s\n' "$1"
    fi
}
{
    echo "# Experiment 02 Phase 3A — provenance of this dataset"
    echo
    echo "Recorded so that the exact code which produced these numbers can be"
    echo "identified later, even if the working tree was not committed when the run"
    echo "happened."
    echo
    echo "## Revision"
    echo
    echo '```'
    echo "git HEAD            : $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "git describe        : $(git describe --always --dirty 2>/dev/null || echo 'unavailable')"
    echo "working tree        : $(if git rev-parse --git-dir >/dev/null 2>&1; then if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then echo 'DIRTY (uncommitted changes present)'; else echo 'clean'; fi; else echo 'not a git repository'; fi)"
    echo "run started (UTC)   : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo '```'
    echo
    echo "### \`git status --porcelain\` as recorded"
    echo
    if git rev-parse --git-dir >/dev/null 2>&1; then
        echo "\`\`\`"
        git status --porcelain 2>/dev/null || echo "(git status failed)"
        echo "\`\`\`"
        echo
        echo "The same output with entries under the archived pre-3A.1 dataset"
        echo "removed, so the entries that identify the CODE are visible:"
        echo
        echo '```'
        git status --porcelain 2>/dev/null \
            | grep -v 'spsc-false-sharing-pre3a1-payload-offset-confounded' \
            || echo "(no changes outside the archived dataset directory)"
        echo '```'
    else
        echo '```'
        echo "not a git repository"
        echo '```'
    fi
    echo
    echo "If this run happened with a non-empty status, HEAD does **not** identify"
    echo "the code that produced this dataset. The hashes below do."
    echo
    echo "## SHA-256 of the artefacts that produced the data"
    echo
    echo '```'
    hash_file "$BENCH"
    hash_file "include/cache_line.h"
    hash_file "include/spsc_cursor_layout_ring_buffer.h"
    hash_file "benchmark/spsc_false_sharing_bench.cpp"
    hash_file "scripts/spsc-false-sharing.sh"
    echo '```'
    echo
    echo "The benchmark executable hash covers the whole translation unit as"
    echo "compiled, so it changes if any header it includes changes, not only if the"
    echo "\`.cpp\` does."
    echo
    echo "## Build"
    echo
    echo '```'
    echo "build system        : CMake, fresh build directory ($BUILDDIR, removed first)"
    echo "CMAKE_BUILD_TYPE    : $BUILD_TYPE"
    echo "BENCH_ARCH_FLAGS    : '${ARCH_FLAGS}' (empty for the canonical run)"
    echo "target              : $BIN_NAME"
    echo "configure           : cmake -S . -B $BUILDDIR -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DBENCH_ARCH_FLAGS=$ARCH_FLAGS"
    echo "build               : cmake --build $BUILDDIR --target $BIN_NAME -j"
    echo "compiler            : $(c++ --version 2>/dev/null | head -1 || echo 'unknown')"
    echo '```'
    echo
    echo "## Command order"
    echo
    echo "The exact effective invocations, in execution order, are in"
    echo "\`command.txt\`; the parsed execution order with the per-cell first layout"
    echo "is in \`run_order.txt\`."
    echo
    echo "## Host and reported cache-line size"
    echo
    echo "- host-reported cache-line size: **$HOST_LINE bytes**, taken from the"
    echo "  \`reported_cache_line_size\` column of the raw data itself."
    echo "- compile-time layout assumption: 128 bytes."
    echo "- full host/toolchain metadata: \`HOST.md\`."
    echo
    echo "Analysis, methodology and limitations: \`docs/SPSC_FALSE_SHARING.md\`."
} >"$PROVENANCE_MD"

# ---- reproducibility metadata ---------------------------------------------
{
    echo "# Experiment 02 Phase 3A — results metadata"
    echo
    echo "CONTROLLED cursor-placement (coherence-layout) experiment. ONE variable:"
    echo "the cache-line placement of the two SPSC cursors, whose policies have the"
    echo "SAME footprint so the payload array starts at the same object offset in"
    echo "both variants."
    echo
    echo "| item | value |"
    echo "|---|---|"
    echo "| matrix | 2 cursor layouts (\`same_line\`, \`separated\`) x 3 message sizes (8, 32, 64) x 3 capacities (1024, 4096, 65536) = 18 cells |"
    echo "| message count | $MESSAGES per repetition |"
    echo "| measured repetitions | $REPS per process |"
    echo "| warm-up | $WARMUP per process, excluded from all reported and derived data |"
    echo "| sessions | $SESSIONS, balanced AB/BA |"
    echo "| processes | $EXPECTED_PROCESSES = 9 cells x 2 layouts x $SESSIONS sessions |"
    echo "| implementations per process | exactly 1 |"
    echo "| host-reported cache-line size | $HOST_LINE bytes |"
    echo "| compile-time layout assumption | 128 bytes |"
    echo "| cursor policy footprint | 256 bytes each, identical across the two layouts |"
    echo "| payload offset | identical across the two layouts, every cell (\`invariants.txt\`) |"
    echo "| runtime layout verification | every measured repetition, every process |"
    echo "| cross-variant footprint gate | before timing, in every control process |"
    echo "| observational natural rows | $OBSERVATIONAL_NATURAL (1 = present, excluded from \`summary.csv\`) |"
    echo "| UTC | $(date -u +%Y-%m-%dT%H:%M:%SZ) |"
    echo
    echo "The SPSC variants are \`SpscSameLineRingBuffer\` and"
    echo "\`SpscSeparatedCursorRingBuffer\` in"
    echo "\`include/spsc_cursor_layout_ring_buffer.h\`. The cache-line query and the"
    echo "guards live in \`include/cache_line.h\`. The frozen Phase-1"
    echo "\`include/spsc_ring_buffer.h\` was NOT modified and is NOT one of the two"
    echo "controls."
    echo
    echo "**Provenance:** \`PROVENANCE.md\` records the git revision, the"
    echo "\`git status --porcelain\` output, the exact build flags and the SHA-256 of"
    echo "the benchmark executable and of the four key Phase-3A source files. This"
    echo "directory's data was produced by those bytes, whether or not they were"
    echo "committed at the time."
    echo
    echo "Analysis, methodology and limitations: \`docs/SPSC_FALSE_SHARING.md\`."
} >"$OUT/RESULTS_METADATA.md"

echo
echo "==> Done."
echo "    raw per-repetition data : $OUT/raw/           ($EXPECTED_RAW_FILES files)"
echo "    per-process summaries   : $OUT/summaries/     ($EXPECTED_RAW_FILES files)"
echo "    benchmark stderr        : $OUT/stderr/"
echo "    invariants              : $OUT/invariants.txt"
echo "    layout verification     : $LAYOUT_MD"
echo "    verified run order      : $ORDER_FILE"
echo "    matrix (derived)        : $MATRIX"
echo "    matrix (human)          : $MATRIX_MD"
echo "    per-session medians     : $SESSION_MD"
echo "    paired comparison (csv) : $PAIRED_CSV"
echo "    paired comparison (md)  : $PAIRED_MD"
echo "    exact commands          : $OUT/command.txt"
echo "    host/toolchain          : $OUT/HOST.md"
echo "    provenance (hashes)     : $OUT/PROVENANCE.md"
echo "    results metadata        : $OUT/RESULTS_METADATA.md"
echo
echo "    Interpretation belongs in docs/SPSC_FALSE_SHARING.md — not in this script."
