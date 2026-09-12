#!/usr/bin/env bash
# Experiment 02 Phase 3B — canonical REMOTE CURSOR CACHING runner.
#
# Runs the complete 2 x 3 x 3 matrix of the Phase-3B benchmark:
#
#   variant        x message_bytes x capacity
#   baseline,      x 8,32,64       x 1024,4096,65536   = 18 cells
#   cached
#
# ONE IMPLEMENTATION PER PROCESS. Every process is a SEPARATE invocation of
# spsc_remote_cursor_bench choosing exactly one --impl; no two variants are ever
# timed inside one interval, one address space, or one warmed-up process state.
# Both variants are built into the SAME benchmark executable under the SAME
# compiler and options. That supports BUILD AND TOOLCHAIN COMPARABILITY. It does
# NOT mean the legs share compiled code or runtime state: each variant is a
# distinct template instantiation with its own emitted instruction sequence and
# code addresses, and each leg runs in an independent process that is not
# guaranteed to share scheduler placement, core type, migration history, DVFS,
# thermal state or background load.
#
# THE ONE TREATMENT. Phase 3B holds everything else fixed and applies ONE
# intended algorithmic treatment: remote-cursor caching. Its primary mechanism
# is HOW OFTEN each thread reads the opposite thread's cursor; the treatment
# also carries its own local fast-path bookkeeping (cached-value read,
# comparison, branch), so a measured difference here does not isolate the cost
# of a single remote atomic load.
#
#   baseline  the Phase-3A separated algorithm: one acquire load of the remote
#             cursor per try_push / try_pop.
#   cached    the same algorithm plus a thread-owned, non-atomic cached copy of
#             the remote cursor, refreshed only when the cached copy says the
#             queue MAY be full (producer) or MAY be empty (consumer).
#
# Everything else is identical by construction because both variants are the SAME
# template body (include/spsc_remote_cursor_ring_buffer.h) parameterised by a
# compile-time mode: identical payload storage, identical object footprint,
# identical SEPARATED cursor placement, identical capacity and slot indexing,
# identical publication protocol, identical retry/yield harness, identical
# message types. There is no batching, no changed memory ordering, no CAS, no
# MPSC/MPMC, no affinity and no NUMA tuning anywhere in this run.
#
# THE CACHE DOES NOT REPLACE SYNCHRONIZATION. The release/acquire publication
# edge exists in BOTH variants and is never weakened: the producer still
# publishes the payload with a release store to head, and the consumer still
# reads the payload only after an acquire observation of head. What the cached
# copy changes is how OFTEN the remote cursor is READ, not what reading it
# guarantees. A stale cached value can only cause a FALSE FULL (producer
# refreshes more than necessary and may refuse a push that would have fit) or a
# FALSE EMPTY (consumer refreshes and may refuse a pop that would have
# succeeded). It can never cause a slot to be reused before the consumer released
# it, and it can never cause an unpublished payload to be read. See
# docs/SPSC_REMOTE_CURSOR_CACHE.md.
#
# THE CURSOR PLACEMENT IS STILL THE VERIFIED SEPARATED LAYOUT. Phase 3B does not
# change cursor placement; it re-verifies the Phase-3A separated invariant on
# every measured object AND adds its own claim: each cached remote cursor sits on
# its OWN owner's cache line (producer-owned cached_tail on the head line,
# consumer-owned cached_head on the tail line) and NOT on the remote cursor's
# line, so the cached copies introduce no new cross-thread false-sharing
# relationship. Both variants have the same object_size and payload_offset; the
# benchmark refuses to time a cell whose two variants disagree, and this script
# re-checks the recorded footprint columns of every raw row and across every
# pair.
#
# MEASURED PERFORMANCE vs MEASURED MECHANISM. These are separate and are labelled
# separately everywhere. The canonical throughput figures come ONLY from
# --instrument=0 processes, in which the counting code is compile-time-eliminated
# (the counter STORAGE stays in the object so the layout is identical; no
# increment is emitted). The remote-load counts come from a SEPARATE
# --instrument=1 leg written to mechanism/, whose ns_per_message is explicitly NOT
# the canonical result. The counters are ordinary thread-owned non-atomic members
# read only after both threads have been joined; no global atomic is on the timed
# hot path. The script FAILS if a canonical process reports a non-zero counter,
# which is how an accidental instrumentation leak is caught rather than published.
#
# BALANCED AB/BA ORDER (why there are exactly 4 sessions)
#   On an unpinned macOS host — where scheduler placement, thread migration,
#   P/E-core selection, DVFS, thermal state and background load all drift — a
#   treatment difference and a time-of-run difference would otherwise be
#   indistinguishable. The two variants of the SAME (message_bytes, capacity)
#   pair are therefore run as ADJACENT processes, with both the variant order and
#   the traversal direction balanced across four sessions:
#
#     session 1: forward traversal, baseline -> cached
#     session 2: reverse traversal, cached   -> baseline
#     session 3: forward traversal, cached   -> baseline
#     session 4: reverse traversal, baseline -> cached
#
#   Every (bytes, capacity) pair therefore receives 2 baseline-first and 2
#   cached-first comparisons. The order is FIXED and DETERMINISTIC and is
#   recorded in command.txt as it runs; the balance is then VERIFIED by parsing
#   that record back, not asserted from the constants above.
#
# SESSION-LEVEL DRIFT IS THE DOMINANT RISK AND IS NOT HIDDEN. This cell shape
# exhibits strong run-to-run and build-to-build regime variation on the
# development host: the same binary can complete the same cell at very different
# ns/message depending on which side of the retry/yield feedback loop the run
# settles into. A separate diagnostic suggested code-layout sensitivity as one
# possible contributor to that variation, but Phase 3B does not isolate its
# cause, and no reproducible diagnostic package is preserved. That is why the raw per-repetition
# producer_full_retries / consumer_empty_retries columns are preserved and
# reported: the regime of every measured process is visible in the dataset rather
# than averaged away. No Phase-3B result may be read as resolving a difference
# smaller than that swing.
#
# MEASUREMENT DISCIPLINE
#   * Release build, -O3 -DNDEBUG, forced by the CMake target itself.
#   * Thread creation, queue allocation, the expected-checksum pre-pass and all
#     address/layout reporting happen outside the timed interval; the consumer
#     records t1 itself.
#   * ns_per_message is END-TO-END elapsed / messages DELIVERED. It is NOT a
#     per-call latency, NOT a per-try_push/try_pop latency, and NOT a one-way
#     handoff time.
#   * Every repetition constructs a FRESH producer/consumer thread pair AND a
#     fresh queue object, so cursor and cached-state addresses are re-verified
#     every repetition.
#   * One warm-up repetition per process is EXCLUDED from every published and
#     derived figure. All measured repetitions are kept — this script never drops
#     a slow run and never selects a best run.
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
#   scripts/spsc-remote-cursor.sh                # canonical balanced matrix
#   FORCE=1 scripts/spsc-remote-cursor.sh        # overwrite an existing dir
#   MESSAGES=200000 REPS=3 scripts/spsc-remote-cursor.sh   # smoke-sized matrix
#   MECHANISM=0 scripts/spsc-remote-cursor.sh    # skip the instrumented leg
#
# Env overrides:
#   MESSAGES  messages per repetition (default 10000000)
#   REPS      measured repetitions per process (default 5, all kept)
#   WARMUP    warm-up repetitions per process, excluded (default 1)
#   OUT       results directory (default docs/results/spsc-remote-cursor)
#   FORCE     1 = allow writing into a non-empty OUT directory
#   BUILDDIR  build directory (default build-spsc-remote-cursor)
#   MECHANISM 1 = also run the instrumented mechanism leg (default 1)
#   MECH_REPS measured repetitions per mechanism process (default 3)
# SESSIONS is NOT overridable: the AB/BA balance is defined for exactly 4.

set -euo pipefail
cd "$(dirname "$0")/.."

REPO_ROOT="$(pwd)"
BIN_NAME="spsc_remote_cursor_bench"

MESSAGES="${MESSAGES:-10000000}"
REPS="${REPS:-5}"
WARMUP="${WARMUP:-1}"
OUT="${OUT:-docs/results/spsc-remote-cursor}"
FORCE="${FORCE:-0}"
BUILDDIR="${BUILDDIR:-build-spsc-remote-cursor}"
MECHANISM="${MECHANISM:-1}"
MECH_REPS="${MECH_REPS:-3}"

# Recorded in command.txt, so it must reflect the directory actually used.
BENCH_REL="$BUILDDIR/$BIN_NAME"

# The balanced design is defined for exactly four sessions (2 baseline-first + 2
# cached-first, 2 forward + 2 reverse). Anything else silently loses the balance
# guarantee, so it is not offered as an option.
SESSIONS=4
CELLS_PER_SESSION=18
EXPECTED_PROCESSES=$(( CELLS_PER_SESSION * SESSIONS ))   # 72
EXPECTED_RAW_FILES=$EXPECTED_PROCESSES
EXPECTED_MECH_PROCESSES=18

# The two Phase-3B variants, in forward traversal order.
IMPLS=(baseline cached)

# The single treatment under study, named once so every message is consistent.
TREATMENT="frequency of remote cursor loads"

if [[ "$REPS" -lt 1 ]]; then
    echo "FATAL: REPS must be >= 1 (got $REPS)" >&2
    exit 1
fi
if [[ "$MECH_REPS" -lt 1 ]]; then
    echo "FATAL: MECH_REPS must be >= 1 (got $MECH_REPS)" >&2
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
# Every other experiment's dataset is frozen. Phase 3B writes its own directory
# and nothing else; a mistyped OUT must fail loudly rather than overwrite
# evidence, including the Phase-3A dataset this experiment's baseline comes from.
case "$OUT" in
    *spsc-throughput*|*spsc-false-sharing*|*phase2-*|*phase3-*|*phase4-*|*orderbook-bitmap-optimization*)
        echo "FATAL: refusing to write into the frozen results directory $OUT" >&2
        exit 1
        ;;
    *spsc-remote-cursor*/*spsc-remote-cursor*)
        # Nested lookalike, e.g. OUT=a/spsc-remote-cursor/b
        echo "FATAL: refusing to write into a nested lookalike of the canonical dir: $OUT" >&2
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

# Fail fast: a tiny smoke cell through BOTH variants, in BOTH instrumentation
# modes, before the real matrix. This is the only automatic benchmark execution;
# the canonical cells are never run by ctest.
echo "==> Smoke check (small, discarded)"
for impl in "${IMPLS[@]}"; do
    for inst in 0 1; do
        "$BENCH" --impl="$impl" --instrument="$inst" --message-bytes=8 \
                 --capacity=1024 --messages=20000 --reps=1 --warmup=0 >/dev/null
    done
done
echo "    smoke check passed for both variants in both instrumentation modes"

# ---------------------------------------------------------------------------
# Session design — fixed, deterministic, and recorded as it runs
# ---------------------------------------------------------------------------
# SESSION_FIRST[session] = variant that runs first in that session.
# Traversal is forward for odd sessions, reverse for even sessions.
session_first_impl() {
    case "$1" in
        1) echo baseline ;;
        2) echo cached ;;
        3) echo cached ;;
        4) echo baseline ;;
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
# Leg 1 — run every canonical process, preserving raw output. Nothing is
# summarized yet.
# ---------------------------------------------------------------------------
echo
echo "==> Running the balanced canonical matrix"
echo "    9 bytes/capacity pairs x 2 variants x $SESSIONS sessions = $EXPECTED_PROCESSES processes"
echo "    messages=$MESSAGES reps=$REPS warmup=$WARMUP (warm-up excluded from all reported data)"

: >"$OUT/command.txt"
{
    echo "# Experiment 02 Phase 3B canonical run — exact effective invocations, IN ORDER"
    echo "# UTC:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# local:     $(date +%Y-%m-%dT%H:%M:%S%z)"
    echo "# repo HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "# messages=$MESSAGES reps=$REPS warmup=$WARMUP sessions=$SESSIONS"
    echo "#"
    echo "# BALANCED AB/BA DESIGN (fixed, deterministic, never randomized):"
    echo "#   session 1: forward traversal, baseline -> cached"
    echo "#   session 2: reverse traversal, cached   -> baseline"
    echo "#   session 3: forward traversal, cached   -> baseline"
    echo "#   session 4: reverse traversal, baseline -> cached"
    echo "# Each bytes/capacity pair therefore gets 2 baseline-first and 2"
    echo "# cached-first comparisons, with forward/reverse traversal balancing"
    echo "# time order."
    echo "#"
    echo "# Each line below is a SEPARATE process with exactly ONE variant."
    echo "# The one intended treatment between the two variants is the $TREATMENT."
    echo "# Cursor placement is the verified SEPARATED layout in both."
    echo "# All canonical lines use --instrument=0: no counting is compiled in."
    echo
} >>"$OUT/command.txt"

for session in $(seq 1 "$SESSIONS"); do
    first_impl="$(session_first_impl "$session")"
    if [[ "$first_impl" == "baseline" ]]; then second_impl=cached; else second_impl=baseline; fi
    traversal="$(session_traversal "$session")"

    {
        echo
        echo "# ---- session $session/$SESSIONS | $traversal traversal | variant order: $first_impl then $second_impl ----"
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
            cmd=("$BENCH_REL" "--impl=$impl" "--instrument=0" "--message-bytes=$bytes"
                 "--capacity=$cap" "--messages=$MESSAGES" "--reps=$REPS"
                 "--warmup=$WARMUP" "--raw-out=$raw" "--summary-out=$summary")
            for a in "${cmd[@]}"; do printf '%q ' "$a"; done >>"$OUT/command.txt"
            printf '\n' >>"$OUT/command.txt"

            # A failing cell aborts the run: set -e propagates the benchmark's
            # non-zero exit, the cell writes no raw CSV, and nothing downstream
            # publishes it. This covers an unsupported host cache-line size, a
            # layout or cached-placement invariant violation, a cross-variant
            # footprint disagreement, and an instrumentation leak — all exit 3,
            # all before any number is published.
            "$BENCH" "--impl=$impl" --instrument=0 --message-bytes="$bytes" \
                     --capacity="$cap" --messages="$MESSAGES" --reps="$REPS" \
                     --warmup="$WARMUP" --raw-out="$raw" --summary-out="$summary" \
                     >/dev/null 2>"$OUT/stderr/${cell_id}_s${session}.txt"
        done
    done
done
echo
echo "==> All $EXPECTED_PROCESSES processes completed and validated (raw output preserved)"

# ---------------------------------------------------------------------------
# Leg 1b — the INSTRUMENTED MECHANISM leg. Separate directory, separate purpose:
# it MEASURES THE MECHANISM (how many remote cursor refreshes actually happen),
# not performance. Its ns_per_message is recorded but is explicitly NOT the
# canonical throughput figure, because the instrumented instantiation is not the
# one the canonical numbers come from.
# ---------------------------------------------------------------------------
if [[ "$MECHANISM" == "1" ]]; then
    echo
    echo "==> Mechanism leg: instrumented counting of remote cursor refreshes"
    echo "    one process per (variant, bytes, capacity) — $EXPECTED_MECH_PROCESSES processes"
    echo "    These numbers are MEASURED MECHANISM, not the canonical throughput."
    mkdir -p "$OUT/mechanism/raw" "$OUT/mechanism/summaries" "$OUT/mechanism/stderr"
    {
        echo
        echo "# ---- MECHANISM LEG: --instrument=1 ----"
        echo "# NOT the canonical throughput. These processes count ACTUAL remote cursor"
        echo "# refreshes in ordinary thread-owned counters. The counters are read only"
        echo "# after both threads joined; no global atomic is on the hot path."
        echo "# ns_per_message from these rows must never be quoted as the canonical result."
    } >>"$OUT/command.txt"

    mech_index=0
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        for impl in "${IMPLS[@]}"; do
            mech_index=$(( mech_index + 1 ))
            cell_id="${impl}_b${bytes}_c${cap}"
            raw="$OUT/mechanism/raw/${cell_id}.csv"
            summary="$OUT/mechanism/summaries/${cell_id}.txt"
            echo "    [$mech_index/$EXPECTED_MECH_PROCESSES] mechanism $cell_id"

            cmd=("$BENCH_REL" "--impl=$impl" "--instrument=1" "--message-bytes=$bytes"
                 "--capacity=$cap" "--messages=$MESSAGES" "--reps=$MECH_REPS"
                 "--warmup=$WARMUP" "--raw-out=$raw" "--summary-out=$summary")
            for a in "${cmd[@]}"; do printf '%q ' "$a"; done >>"$OUT/command.txt"
            printf '\n' >>"$OUT/command.txt"

            "$BENCH" "--impl=$impl" --instrument=1 --message-bytes="$bytes" \
                     --capacity="$cap" --messages="$MESSAGES" --reps="$MECH_REPS" \
                     --warmup="$WARMUP" --raw-out="$raw" --summary-out="$summary" \
                     >/dev/null 2>"$OUT/mechanism/stderr/${cell_id}.txt"
        done
    done
    echo "    mechanism leg complete (excluded from every canonical figure)"
else
    echo
    echo "==> Mechanism leg SKIPPED (MECHANISM=0): no remote-load counts will exist"
fi

# ---------------------------------------------------------------------------
# Leg 2 — verify every summary against its own raw CSV, then summarize.
# ---------------------------------------------------------------------------
echo
echo "==> Verifying each process summary against the raw CSV it summarizes"

# Raw CSV columns (1-based), fixed by the benchmark's raw writer:
#   1 rep, 2 measurement_mode, 3 impl, 4 message_bytes, 5 capacity,
#   6 message_count, 7 elapsed_ns, 8 ns_per_message, 9 messages_per_second,
#   10 producer_full_retries, 11 consumer_empty_retries, 12 checksum,
#   13 correctness, 14 instrumented, 15 producer_remote_tail_loads,
#   16 consumer_remote_head_loads, 17 ptl_per_message, 18 chr_per_message,
#   19 reported_cache_line_size, 20 head_addr, 21 tail_addr, 22 head_line,
#   23 tail_line, 24 cursors_same_line, 25 layout_ok, 26 cached_tail_addr,
#   27 cached_head_addr, 28 cached_tail_line, 29 cached_head_line,
#   30 cached_placement_ok, 31 object_addr, 32 object_size, 33 payload_offset,
#   34 payload_begin_addr
verify_cell() {
    local raw="$1" summary="$2" cell_id="$3" expect_mode="$4" expect_inst="$5"

    # The summary spells the instrumentation flag "yes"/"no"; the raw CSV spells
    # the same flag "1"/"0". Both spellings are checked, each in its own file.
    local raw_inst
    case "$expect_inst" in
        no)  raw_inst=0 ;;
        yes) raw_inst=1 ;;
        *)   echo "FATAL: bad instrumentation expectation '$expect_inst'" >&2; return 1 ;;
    esac

    if [[ ! -s "$raw" ]]; then
        echo "FATAL: $cell_id produced no raw CSV" >&2
        return 1
    fi
    if ! grep -q '^correctness=PASS$' "$summary"; then
        echo "FATAL: $cell_id summary does not report correctness=PASS" >&2
        return 1
    fi
    if ! grep -q "^measurement_mode=${expect_mode}$" "$summary"; then
        echo "FATAL: $cell_id summary does not report measurement_mode=${expect_mode}" >&2
        return 1
    fi
    if ! grep -q "^instrumented=${expect_inst}$" "$summary"; then
        echo "FATAL: $cell_id summary does not report instrumented=${expect_inst}" >&2
        return 1
    fi
    if ! grep -q '^layout_invariant=PASS$' "$summary"; then
        echo "FATAL: $cell_id did not verify the Phase-3A separated layout" >&2
        return 1
    fi
    if ! grep -q '^cached_placement_invariant=PASS$' "$summary"; then
        echo "FATAL: $cell_id did not verify the Phase-3B cached-state placement" >&2
        return 1
    fi

    # Recompute median/min/max of ns_per_message (column 8) from the raw CSV,
    # and pull the claimed values out of the summary in the same awk program.
    awk -F, -v id="$cell_id" -v sumfile="$summary" -v rawfile="$raw" \
        -v want_mode="$expect_mode" -v want_inst="$raw_inst" '
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
                if (c[1] ~ /^[0-9]+$/ && c[8] != "") {
                    # The raw row must carry the mode it claims, and the mode the
                    # caller expects. A row that silently came from the other
                    # instrumentation setting would mislabel every figure derived
                    # from it.
                    if (c[2] != want_mode) {
                        printf "FATAL: %s raw row %s has measurement_mode=%s, expected %s\n", id, c[1], c[2], want_mode > "/dev/stderr"
                        bad = 1; exit
                    }
                    if (c[14] != want_inst) {
                        printf "FATAL: %s raw row %s has instrumented=%s, expected %s\n", id, c[1], c[14], want_inst > "/dev/stderr"
                        bad = 1; exit
                    }
                    v[++n] = c[8] + 0
                }
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
                        "${cell_id}_s${session}" performance no
        done
    done
done
echo "    all $EXPECTED_PROCESSES summaries match their own raw repetition data"

if [[ "$MECHANISM" == "1" ]]; then
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        for impl in "${IMPLS[@]}"; do
            cell_id="${impl}_b${1}_c${2}"
            verify_cell "$OUT/mechanism/raw/${cell_id}.csv" \
                        "$OUT/mechanism/summaries/${cell_id}.txt" \
                        "mechanism_${cell_id}" mechanism yes
        done
    done
    echo "    all $EXPECTED_MECH_PROCESSES mechanism summaries match their own raw data"
fi

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
    /^# ---- MECHANISM LEG/ { in_mech = 1; s = ""; next }
    /--impl=/ {
        impl = ""; mb = ""; cap = ""; inst = ""
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^--impl=/)          { split($i, a, "="); impl = a[2] }
            if ($i ~ /^--message-bytes=/) { split($i, a, "="); mb   = a[2] }
            if ($i ~ /^--capacity=/)      { split($i, a, "="); cap  = a[2] }
            if ($i ~ /^--instrument=/)    { split($i, a, "="); inst = a[2] }
        }
        if (in_mech) { next }
        if (s == "" || impl == "" || mb == "" || cap == "" || inst == "") {
            printf "FATAL: unparseable command.txt invocation line\n" > "/dev/stderr"
            exit 1
        }
        printf "%s\t%s\t%s\t%s\t%s\n", s, mb, cap, impl, inst
    }
' "$OUT/command.txt" >"$ORDER_FILE"

invariant_fail() {
    echo "FATAL: invariant violated — $1" >&2
    exit 1
}

# (a) exactly EXPECTED_PROCESSES canonical invocations were recorded, and every
#     one of them ran UNINSTRUMENTED.
recorded="$(wc -l <"$ORDER_FILE" | tr -d ' ')"
[[ "$recorded" == "$EXPECTED_PROCESSES" ]] \
    || invariant_fail "command.txt records $recorded canonical invocations, expected $EXPECTED_PROCESSES"
instr_in_canonical="$(awk -F'\t' '$5 != "0"' "$ORDER_FILE" | wc -l | tr -d ' ')"
[[ "$instr_in_canonical" == "0" ]] \
    || invariant_fail "$instr_in_canonical canonical invocations used --instrument=1; canonical throughput must be uninstrumented"

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
#     stable checksum, rows that agree with each other about what cell they are,
#     and — for canonical rows — ZERO remote-load counters, which is what proves
#     no counting was compiled into the process that produced a throughput number;
# (d) EVERY measured row carries a PASSING runtime verification of BOTH the
#     Phase-3A separated cursor layout AND the Phase-3B cached-state placement,
#     and the measured placement MATCHES what the row claims. This is the check
#     that makes the dataset evidence about cursor placement at all, and that
#     makes "no new cross-thread false sharing" a verified claim rather than an
#     assertion.
invariant_scan() {  # $1 = directory with raw CSVs  $2 = description  $3 = reps
    awk -F, -v reps="$3" -v messages="$MESSAGES" -v what="$2" '
        FNR == 1 {
            if (prev != "" && prev_n != reps) {
                printf "FATAL: %s has %d measured rows, expected %d\n", prev, prev_n, reps > "/dev/stderr"
                bad = 1
            }
            prev = FILENAME; prev_n = 0; prev_ck = ""
            f_impl = ""; f_mb = ""; f_cap = ""
            # object_size legitimately varies BETWEEN cells (it scales with
            # capacity and message size), so footprint consistency is scoped to
            # one process, i.e. one file. The cross-variant comparison for a cell
            # is a separate check, done below on the pair.
            osize = ""; poff = ""
        }
        /^#/ || /^rep,/ || NF == 0 { next }
        {
            if ($1 !~ /^[0-9]+$/) { next }
            prev_n++
            # every row must describe the SAME cell as the first row of its file
            if (f_impl == "") { f_impl = $3; f_mb = $4; f_cap = $5 }
            else if ($3 != f_impl || $4 != f_mb || $5 != f_cap) {
                printf "FATAL: %s row %s disagrees with its file about the cell\n", FILENAME, $1 > "/dev/stderr"
                bad = 1
            }
            if ($6 + 0 != messages + 0) {
                printf "FATAL: %s row %s ran %s messages, expected %s\n", FILENAME, $1, $6, messages > "/dev/stderr"
                bad = 1
            }
            if ($13 != "PASS") {
                printf "FATAL: %s has a non-PASS correctness row: %s\n", FILENAME, $0 > "/dev/stderr"
                bad = 1
            }
            if ($8 == "" || $8 + 0 <= 0) {
                printf "FATAL: %s has a non-positive ns_per_message: %s\n", FILENAME, $0 > "/dev/stderr"
                bad = 1
            }
            if (prev_ck == "") { prev_ck = $12 } else if (prev_ck != $12) {
                printf "FATAL: %s checksum varies between repetitions\n", FILENAME > "/dev/stderr"
                bad = 1
            }

            # ---- canonical rows must be UNINSTRUMENTED, and must say so ----
            if (what == "canonical") {
                if ($2 != "performance") {
                    printf "FATAL: %s row %s has measurement_mode=%s in the canonical matrix\n", FILENAME, $1, $2 > "/dev/stderr"
                    bad = 1
                }
                if ($14 != "0") {
                    printf "FATAL: %s row %s has instrumented=%s in the canonical matrix\n", FILENAME, $1, $14 > "/dev/stderr"
                    bad = 1
                }
                if ($15 + 0 != 0 || $16 + 0 != 0) {
                    printf "FATAL: INSTRUMENTATION LEAK — %s row %s reports %s/%s remote loads in an uninstrumented canonical run\n", FILENAME, $1, $15, $16 > "/dev/stderr"
                    bad = 1
                }
            } else {
                # ---- mechanism rows must be INSTRUMENTED, and must say so ----
                if ($2 != "mechanism") {
                    printf "FATAL: %s row %s has measurement_mode=%s in the mechanism leg\n", FILENAME, $1, $2 > "/dev/stderr"
                    bad = 1
                }
                if ($14 != "1") {
                    printf "FATAL: %s row %s has instrumented=%s in the mechanism leg\n", FILENAME, $1, $14 > "/dev/stderr"
                    bad = 1
                }
            }

            # ---- Phase-3A separated layout, re-checked here independently ----
            # Columns: 19 reported_cache_line_size, 20 head_addr, 21 tail_addr,
            #          22 head_line, 23 tail_line, 24 cursors_same_line, 25 layout_ok
            if ($25 != "PASS") {
                printf "FATAL: %s row %s has layout_ok=%s (layout not verified)\n", FILENAME, $1, $25 > "/dev/stderr"
                bad = 1
            }
            if ($19 + 0 <= 0) {
                printf "FATAL: %s row %s has no reported cache-line size\n", FILENAME, $1 > "/dev/stderr"
                bad = 1
            }
            if (clsize == "") { clsize = $19 }
            else if ($19 != clsize) {
                printf "FATAL: %s reports cache-line size %s, earlier files reported %s\n", FILENAME, $19, clsize > "/dev/stderr"
                bad = 1
            }
            if ($22 == "" || $23 == "" || $22 == "NA") {
                printf "FATAL: %s row %s has no cursor line indices\n", FILENAME, $1 > "/dev/stderr"
                bad = 1
            }
            # Phase 3B NEVER claims same-line cursors: cursor placement is the
            # separated layout in BOTH variants. A row claiming otherwise is a
            # failed experiment, whatever it measured.
            if ($24 != "no" || $22 + 0 == $23 + 0) {
                printf "FATAL: %s row %s is not separated (lines %s/%s, same=%s)\n", FILENAME, $1, $22, $23, $24 > "/dev/stderr"
                bad = 1
            }
            if ($3 != "baseline" && $3 != "cached") {
                printf "FATAL: %s row %s has unexpected impl %s in the canonical matrix\n", FILENAME, $1, $3 > "/dev/stderr"
                bad = 1
            }

            # ---- Phase-3B cached-state placement, re-checked independently ----
            # Columns: 26 cached_tail_addr, 27 cached_head_addr,
            #          28 cached_tail_line, 29 cached_head_line, 30 cached_placement_ok
            # cached_tail is producer-owned and must share the head line;
            # cached_head is consumer-owned and must share the tail line. Neither
            # may sit on the REMOTE cursor line, and the two cached values must
            # not be on the same line as each other.
            if ($30 != "PASS") {
                printf "FATAL: %s row %s has cached_placement_ok=%s\n", FILENAME, $1, $30 > "/dev/stderr"
                bad = 1
            }
            if ($28 + 0 != $22 + 0) {
                printf "FATAL: %s row %s: cached_tail line %s is not the head line %s\n", FILENAME, $1, $28, $22 > "/dev/stderr"
                bad = 1
            }
            if ($29 + 0 != $23 + 0) {
                printf "FATAL: %s row %s: cached_head line %s is not the tail line %s\n", FILENAME, $1, $29, $23 > "/dev/stderr"
                bad = 1
            }
            if ($28 + 0 == $23 + 0 || $29 + 0 == $22 + 0) {
                printf "FATAL: %s row %s: a cached value sits on its REMOTE cursor\x27s line\n", FILENAME, $1 > "/dev/stderr"
                bad = 1
            }
            if ($28 + 0 == $29 + 0) {
                printf "FATAL: %s row %s: the two cached values share one line, creating a NEW shared line\n", FILENAME, $1 > "/dev/stderr"
                bad = 1
            }

            # ---- footprint stability within this process --------------------
            # Every repetition of one process measures the same instantiation, so
            # it must report the same object_size and payload_offset. A change
            # here would mean the rows are not all the same queue.
            if (osize == "") { osize = $32; poff = $33 }
            else if ($32 != osize || $33 != poff) {
                printf "FATAL: %s row %s footprint %s/%s differs from earlier rows of the same process %s/%s\n", FILENAME, $1, $32, $33, osize, poff > "/dev/stderr"
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
    ' "$1"/*.csv || return 1
    return 0
}

invariant_scan "$OUT/raw" canonical "$REPS" \
    || invariant_fail "canonical raw rows failed the invariant scan"
if [[ "$MECHANISM" == "1" ]]; then
    invariant_scan "$OUT/mechanism/raw" mechanism "$MECH_REPS" \
        || invariant_fail "mechanism raw rows failed the invariant scan"
fi

# (e) THE PAIRING INVARIANT. For every session and every (bytes, capacity), the
#     baseline and the cached process ran ADJACENTLY, in the order the design
#     says. This is checked from the recorded order, not from the constants.
awk -F'\t' '
    {
        key = $1 " " $2 " " $3
        if (key != prev_key) {
            if (seen[key] != "") {
                printf "FATAL: cell %s appears in non-contiguous runs in session %s\n", key, $1 > "/dev/stderr"
                bad = 1
            }
            seen[key] = 1
            n_here = 0
            first = $4
        }
        n_here++
        if (n_here > 2) {
            printf "FATAL: cell %s has %d processes in session %s, expected a pair\n", key, n_here, $1 > "/dev/stderr"
            bad = 1
        }
        if (n_here == 1) { first = $4; second = "" }
        if (n_here == 2) {
            second = $4
            if (first == second) {
                printf "FATAL: cell %s session %s ran %s twice instead of a baseline/cached pair\n", key, $1, first > "/dev/stderr"
                bad = 1
            }
        }
        prev_key = key
    }
    END { if (bad) exit 1 }
' "$ORDER_FILE" || invariant_fail "baseline/cached pairs are not adjacent in the recorded order"

# (f) the AB/BA BALANCE itself: each session's first variant must match the
#     design, and over the four sessions every (bytes, capacity) must get exactly
#     2 baseline-first and 2 cached-first pairs.
expected_first="$(for s in 1 2 3 4; do session_first_impl "$s"; done | tr '\n' ' ' | sed 's/ $//')"
# Take the FIRST variant seen in each session; flag a session whose first variant
# changes part-way through, which would mean the recorded order is not the design.
recorded_first="$(awk -F'\t' '
    { key = $1 " " $2 " " $3 }
    key != prev {
        if (!($1 in first))        { first[$1] = $4 }
        else if (first[$1] != $4)  { mixed[$1] = 1 }
        prev = key
    }
    END {
        for (s = 1; s <= 4; s++) {
            printf "%s%s ", (mixed[s] ? "MIXED:" : ""), first[s]
        }
    }
' "$ORDER_FILE" | sed 's/ $//')"
[[ "$recorded_first" == "$expected_first" ]] \
    || invariant_fail "session first-variant order '$recorded_first' does not match the design '$expected_first'"

# Each (bytes, capacity) must receive 2 baseline-first and 2 cached-first.
# One count per contiguous run: the first variant of that run is the one that
# ran first in that session for that cell.
awk -F'\t' '
    {
        cell = $2 " " $3
        if ($1 != prev_s || cell != prev_c) {
            first_count[cell "|" $4]++
            cells[cell] = 1
            prev_s = $1
            prev_c = cell
        }
    }
    END {
        for (c in cells) {
            b = first_count[c "|baseline"] + 0
            k = first_count[c "|cached"] + 0
            if (b != 2 || k != 2) {
                printf "FATAL: cell %s has %d baseline-first and %d cached-first sessions, expected 2 and 2\n", c, b, k > "/dev/stderr"
                bad = 1
            }
        }
        if (bad) exit 1
    }
' "$ORDER_FILE" || invariant_fail "the AB/BA balance is not 2 baseline-first / 2 cached-first per cell"

# (g) THE FOOTPRINT INVARIANT ACROSS THE PAIR. Phase 3B's claim that the two
#     variants have the same object size and payload offset is verified per row
#     above; here it is verified ACROSS the pair, i.e. between two independently
#     allocated processes, which is the form the experiment actually needs.
awk -F, '
    /^#/ || /^rep,/ || NF == 0 { next }
    $1 !~ /^[0-9]+$/ { next }
    {
        # Cell identity = (bytes, capacity, session), taken from the row itself
        # for bytes/capacity and from the file name for the session, so the two
        # variants of a pair are keyed identically regardless of which file the
        # row came from.
        f = FILENAME; sub(/^.*\//, "", f)
        sess = f; sub(/^.*_s/, "", sess); sub(/\.csv$/, "", sess)
        cell = $4 "|" $5 "|" sess
        size[cell "|" $3] = $32
        off[cell "|" $3]  = $33
        seen[cell] = 1
    }
    END {
        for (c in seen) {
            if (size[c "|baseline"] == "" || size[c "|cached"] == "") {
                printf "FATAL: cell %s is missing one variant in raw/\n", c > "/dev/stderr"
                bad = 1
                continue
            }
            if (size[c "|baseline"] != size[c "|cached"] ||
                off[c "|baseline"]  != off[c "|cached"]) {
                printf "FATAL: cell %s object_size %s vs %s, payload_offset %s vs %s\n",
                       c, size[c "|baseline"], size[c "|cached"],
                       off[c "|baseline"], off[c "|cached"] > "/dev/stderr"
                bad = 1
            }
        }
        if (bad) exit 1
    }
' "$OUT"/raw/*.csv || invariant_fail "a baseline/cached pair disagrees on object_size or payload_offset"

echo "    dataset invariants hold (counts, ordering, pairing, AB/BA balance, footprint, layout, no leak)"

# ---------------------------------------------------------------------------
# summary.csv — the pooled, SECONDARY descriptive view
# ---------------------------------------------------------------------------
echo
echo "==> Deriving summary.csv (pooled, descriptive)"

{
    echo "# Experiment 02 Phase 3B — per-process summary, ALL measured repetitions pooled"
    echo "# DERIVED from raw/, never re-measured. PRIMARY comparison is PAIRED_COMPARISON.md."
    echo "# ns_per_message mean is over every measured repetition of that process;"
    echo "# median/min/max are the process's own summary values, re-verified against raw/."
    echo "# producer_full_retries / consumer_empty_retries are recorded because this cell"
    echo "# shape is bimodal on the development host: they reveal WHICH REGIME a process"
    echo "# ran in. A process with a huge consumer_empty_retries is one where the consumer"
    echo "# spun on an empty queue rather than blocking on real handoffs."
    echo "# measurement_mode=performance for every row here; remote loads are 0 by construction."
    echo "session,impl,message_bytes,capacity,object_size,payload_offset,median_ns_per_message,min_ns_per_message,max_ns_per_message,spread_pct,producer_full_retries_total,consumer_empty_retries_total,aspect_ratio"
    for session in $(seq 1 "$SESSIONS"); do
        for pair in "${PAIRS[@]}"; do
            # shellcheck disable=SC2086
            set -- $pair
            for impl in "${IMPLS[@]}"; do
                s="$OUT/summaries/${impl}_b${1}_c${2}_s${session}.txt"
                med="$(grep '^median_ns_per_message=' "$s" | cut -d= -f2)"
                mn="$(grep '^min_ns_per_message=' "$s" | cut -d= -f2)"
                mx="$(grep '^max_ns_per_message=' "$s" | cut -d= -f2)"
                sp="$(grep '^spread_pct=' "$s" | cut -d= -f2)"
                pf="$(grep '^producer_full_retries_total=' "$s" | cut -d= -f2)"
                ce="$(grep '^consumer_empty_retries_total=' "$s" | cut -d= -f2)"
                osz="$(grep '^object_size=' "$s" | cut -d= -f2)"
                pof="$(grep '^payload_offset_from_object_base=' "$s" | cut -d= -f2)"
                ar="$(awk -v a="$pf" -v b="$ce" 'BEGIN { printf "%.4f", (b > 0) ? a / b : 0 }')"
                echo "$session,$impl,$1,$2,$osz,$pof,$med,$mn,$mx,$sp,$pf,$ce,$ar"
            done
        done
    done
} >"$OUT/summary.csv"

# ---------------------------------------------------------------------------
# MATRIX.md — human-readable pooled view
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 3B — pooled matrix (SECONDARY, descriptive)"
    echo
    echo "Derived from the per-process summaries in \`summaries/\`, which were each"
    echo "re-verified against their own raw repetition CSV in \`raw/\`."
    echo
    echo "**This is not the primary comparison.** It pools the $REPS measured"
    echo "repetitions inside each process, and those repetitions are not independent"
    echo "placements — they share one address space, one allocator state and one"
    echo "thermal history. Read \`PAIRED_COMPARISON.md\` for the paired analysis."
    echo
    echo "The **aspect ratio** column is \`producer_full_retries / consumer_empty_retries\`."
    echo "Values well below 1 mean the consumer was the side that kept finding the"
    echo "queue empty — i.e. the producer was the pacer. Values well above 1 mean the"
    echo "opposite. It is recorded because this cell shape is bimodal on this host and"
    echo "the ratio is how a reader can see which regime a process ran in."
    echo
    echo "| session | variant | bytes | capacity | median ns/msg | min | max | spread % | producer full retries | consumer empty retries | aspect |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|"
    awk -F, '/^#/ { next } $1 == "session" { next }
        { printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",
                 $1, $2, $3, $4, $7, $8, $9, $10, $11, $12, $13 }' "$OUT/summary.csv"
    echo
    echo "## Median of the per-session medians, by cell"
    echo
    echo "| bytes | capacity | baseline median ns/msg | cached median ns/msg | cached/baseline |"
    echo "|---|---|---|---|---|"
    awk -F, '/^#/ { next } $1 == "session" { next }
        { key = $3 " " $4; v[key"|"$2] = v[key"|"$2] " " $7; cells[key] = 1 }
        END {
            for (k in cells) {
                split(k, a, " ")
                for (impl_i = 1; impl_i <= 2; impl_i++) {
                    impl = (impl_i == 1) ? "baseline" : "cached"
                    n = split(v[k"|"impl], arr, " ")
                    for (i = 1; i <= n; i++) for (j = i+1; j <= n; j++) if (arr[j] < arr[i]) { t = arr[i]; arr[i] = arr[j]; arr[j] = t }
                    med = (n % 2 == 1) ? arr[int((n+1)/2)] : (arr[n/2] + arr[n/2+1]) / 2
                    if (impl == "baseline") b = med; else c = med
                }
                printf "| %s | %s | %.6f | %.6f | %.6f |\n", a[1], a[2], b, c, c / b
            }
        }' "$OUT/summary.csv"
} >"$OUT/MATRIX.md"

# ---------------------------------------------------------------------------
# SESSIONS.md — per-process/session medians, no new measurement
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 3B — per-session process medians"
    echo
    echo "One row per measured process. Every value is that process's own"
    echo "\`median_ns_per_message\`, already re-verified against its raw CSV."
    echo "Warm-up repetitions are excluded by the benchmark itself."
    echo
    echo "| session | traversal | first variant | variant | bytes | capacity | median ns/msg | measured reps |"
    echo "|---|---|---|---|---|---|---|---|"
    for session in $(seq 1 "$SESSIONS"); do
        traversal="$(session_traversal "$session")"
        first_impl="$(session_first_impl "$session")"
        for pair in "${PAIRS[@]}"; do
            # shellcheck disable=SC2086
            set -- $pair
            for impl in "${IMPLS[@]}"; do
                s="$OUT/summaries/${impl}_b${1}_c${2}_s${session}.txt"
                med="$(grep '^median_ns_per_message=' "$s" | cut -d= -f2)"
                nr="$(grep '^measured_reps=' "$s" | cut -d= -f2)"
                echo "| $session | $traversal | $first_impl | $impl | $1 | $2 | $med | $nr |"
            done
        done
    done
} >"$OUT/SESSIONS.md"

# ---------------------------------------------------------------------------
# LAYOUT_VERIFICATION.md — the placement evidence, per process
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 3B — runtime layout verification"
    echo
    echo "Phase 3B re-verifies the Phase-3A separated cursor layout on **every**"
    echo "measured object, and adds its own claim about where the cached remote"
    echo "cursors live. Neither claim is asserted from the type; both are checked on"
    echo "the addresses of the object that actually ran, in every repetition."
    echo
    echo "## What is claimed"
    echo
    echo "1. **The separated layout still holds, in BOTH variants.** \`head\` and"
    echo "   \`tail\` are on different cache lines under the host's reported line size,"
    echo "   and neither shares a line with the payload array."
    echo "2. **Each cached remote cursor sits on its OWN owner's line.** The"
    echo "   producer-owned \`cached_tail\` shares the producer's \`head\` line; the"
    echo "   consumer-owned \`cached_head\` shares the consumer's \`tail\` line."
    echo "3. **No new cross-thread false-sharing relationship is introduced.**"
    echo "   \`cached_tail\` is not on the \`tail\` line, \`cached_head\` is not on the"
    echo "   \`head\` line, and the two cached values are not on the same line as each"
    echo "   other. The cached copies therefore add no line that two different threads"
    echo "   both write."
    echo "4. **Both variants have the same footprint.** Identical \`object_size\` and"
    echo "   identical \`payload_offset_from_object_base\`, checked between the two"
    echo "   independently allocated processes of every pair."
    echo
    echo "## Per-process evidence"
    echo
    echo "| session | variant | bytes | capacity | line size | head line | tail line | same line? | layout | cached_tail line | cached_head line | cached placement | object size | payload offset |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
    for session in $(seq 1 "$SESSIONS"); do
        for pair in "${PAIRS[@]}"; do
            # shellcheck disable=SC2086
            set -- $pair
            for impl in "${IMPLS[@]}"; do
                s="$OUT/summaries/${impl}_b${1}_c${2}_s${session}.txt"
                lv="$(grep '^layout_first_measured_rep=' "$s" | cut -d= -f2-)"
                get() { printf '%s' "$lv" | tr ' ' '\n' | grep "^$1=" | cut -d= -f2; }
                printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n" \
                    "$session" "$impl" "$1" "$2" \
                    "$(get reported_cache_line_size)" "$(get head_line)" "$(get tail_line)" \
                    "$(printf '%s' "$lv" | tr ' ' '\n' | grep '^same_line=' | cut -d= -f2)" \
                    "$(get layout_ok)" "$(get cached_tail_line)" "$(get cached_head_line)" \
                    "$(printf '%s' "$lv" | tr ' ' '\n' | grep '^cached_placement=' | cut -d= -f2)" \
                    "$(grep '^object_size=' "$s" | cut -d= -f2)" \
                    "$(grep '^payload_offset_from_object_base=' "$s" | cut -d= -f2)"
            done
        done
    done
    echo
    echo "## Footprint equivalence, per (bytes, capacity)"
    echo
    echo "The benchmark independently instantiates BOTH variants for the cell before"
    echo "timing and refuses to run if their \`object_size\` or"
    echo "\`payload_offset_from_object_base\` disagree. These columns are the recorded"
    echo "result of that gate, read back from the two separately allocated processes."
    echo
    echo "| bytes | capacity | baseline object size | cached object size | baseline payload offset | cached payload offset | agree |"
    echo "|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bs="$(grep '^object_size=' "$OUT/summaries/baseline_b${1}_c${2}_s1.txt" | cut -d= -f2)"
        cs="$(grep '^object_size=' "$OUT/summaries/cached_b${1}_c${2}_s1.txt" | cut -d= -f2)"
        bo="$(grep '^payload_offset_from_object_base=' "$OUT/summaries/baseline_b${1}_c${2}_s1.txt" | cut -d= -f2)"
        co="$(grep '^payload_offset_from_object_base=' "$OUT/summaries/cached_b${1}_c${2}_s1.txt" | cut -d= -f2)"
        if [[ "$bs" == "$cs" && "$bo" == "$co" ]]; then agree=YES; else agree="NO — FAILED"; fi
        echo "| $1 | $2 | $bs | $cs | $bo | $co | $agree |"
    done
} >"$OUT/LAYOUT_VERIFICATION.md"

# ---------------------------------------------------------------------------
# paired_summary.csv + PAIRED_COMPARISON.md — the PRIMARY comparison
# ---------------------------------------------------------------------------
PAIRED_CSV="$OUT/paired_summary.csv"
PAIRED_MD="$OUT/PAIRED_COMPARISON.md"

# session_bytes_cap -> first variant, from the verified run order.
first_impl_for() {  # $1=session $2=bytes $3=cap
    awk -F'\t' -v s="$1" -v b="$2" -v c="$3" \
        '$1 == s && $2 == b && $3 == c { print $4; exit }' "$ORDER_FILE"
}
session_median() {  # $1=impl $2=bytes $3=cap $4=session
    grep '^median_ns_per_message=' "$OUT/summaries/$1_b$2_c$3_s$4.txt" | cut -d= -f2
}

{
    echo "# Experiment 02 Phase 3B — paired-session comparison (PRIMARY)"
    echo "# DERIVED from the per-process summaries in summaries/, one row per cell."
    echo "# ratio_sN = cached session-N median / baseline session-N median."
    echo "#   ratio < 1  => in that session, the CACHED variant completed a message faster"
    echo "#   ratio > 1  => in that session, the BASELINE variant completed a message faster"
    echo "# The two processes of a pair ran ADJACENTLY with order balanced AB/BA."
    echo "# DESCRIPTIVE paired analysis — NOT a significance test. Four paired"
    echo "# observations per cell cannot establish formal statistical significance."
    echo "# message_count=$MESSAGES reps_per_process=$REPS sessions=$SESSIONS"
    echo "# UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '%s\n' "message_bytes,capacity,first_impl_s1,ratio_s1,first_impl_s2,ratio_s2,first_impl_s3,ratio_s3,first_impl_s4,ratio_s4,median_paired_ratio,min_paired_ratio,max_paired_ratio,sessions_cached_faster,sessions_baseline_faster"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        row="$bytes,$cap"
        ratios_lines=""
        cached_faster=0
        baseline_faster=0
        for session in $(seq 1 "$SESSIONS"); do
            fi_="$(first_impl_for "$session" "$bytes" "$cap")"
            bl="$(session_median baseline "$bytes" "$cap" "$session")"
            ca="$(session_median cached "$bytes" "$cap" "$session")"
            r="$(awk -v a="$ca" -v b="$bl" 'BEGIN { printf "%.6f", a / b }')"
            row="$row,$fi_,$r"
            ratios_lines="${ratios_lines}${r}"$'\n'
            # A ratio of exactly 1 is counted for neither side, so a tie can
            # never be silently folded into a directional claim.
            if awk -v r="$r" 'BEGIN { exit !(r < 1) }'; then
                cached_faster=$(( cached_faster + 1 ))
            elif awk -v r="$r" 'BEGIN { exit !(r > 1) }'; then
                baseline_faster=$(( baseline_faster + 1 ))
            fi
        done
        stats="$(printf '%s' "$ratios_lines" | sort -n | awk '
            { v[++n] = $1 }
            END {
                med = (n % 2 == 1) ? v[int((n+1)/2)] : (v[n/2] + v[n/2+1]) / 2
                printf "%.6f,%.6f,%.6f", med, v[1], v[n]
            }')"
        echo "$row,$stats,$cached_faster,$baseline_faster"
    done
} >"$PAIRED_CSV"

{
    echo "# Experiment 02 Phase 3B — paired-session comparison"
    echo
    echo "## What is being compared"
    echo
    echo "The **only** difference between the two implementations compared here is"
    echo "the **$TREATMENT**. Both come from one algorithm body parameterised by a"
    echo "compile-time mode, so the payload storage, the payload offset, the object"
    echo "size, the SEPARATED cursor placement, the capacity, the slot indexing, the"
    echo "publication protocol, the retry/yield harness and the message types are"
    echo "identical by construction. There is no batching, no changed memory ordering,"
    echo "no CAS, no MPSC/MPMC, no affinity and no NUMA tuning in either variant."
    echo
    echo "**The release/acquire publication edge exists in BOTH variants and was not"
    echo "weakened.** The producer still publishes with a release store to \`head\` and"
    echo "the consumer still reads the payload only after an acquire observation of"
    echo "\`head\`. The cached copy changes how OFTEN the remote cursor is read, not"
    echo "what reading it guarantees. A stale cached value can only produce a FALSE"
    echo "FULL or a FALSE EMPTY — never a reused slot and never a read of unpublished"
    echo "data."
    echo
    echo "\`ratio\` = **cached median / baseline median** within a session."
    echo "**\`ratio < 1\` means the cached variant completed a message faster** in that"
    echo "session."
    echo
    echo "## Why paired, and not the pooled table"
    echo
    echo "Under the balanced AB/BA design, the baseline process and the cached process"
    echo "for a given \`(message_bytes, capacity)\` run as **adjacent processes**, with"
    echo "variant order swapped between sessions. **Adjacent execution reduces temporal"
    echo "drift between the two legs but cannot guarantee identical scheduler, DVFS,"
    echo "thermal, or background-system state** — the two processes are still separated"
    echo "by a full benchmark run, and each leg can be placed, migrated or"
    echo "frequency-scaled independently. The pairing narrows the gap; it does not"
    echo "close it."
    echo
    echo "Pooling the raw repetitions instead would treat the $REPS repetitions inside"
    echo "one process as independent placements. They are not: a repetition does"
    echo "create a fresh producer/consumer thread pair and a fresh queue object, but it"
    echo "shares its process's address space, allocator state and thermal history with"
    echo "its siblings. The pooled view is kept in \`MATRIX.md\` as a **secondary,"
    echo "descriptive** metric."
    echo
    echo "## Per-session detail"
    echo
    echo "\`first\` is the variant that ran first in that session's pair."
    echo
    echo "| bytes | capacity | session | first | baseline ns/msg | cached ns/msg | ratio |"
    echo "|---|---|---|---|---|---|---|"
    for pair in "${PAIRS[@]}"; do
        # shellcheck disable=SC2086
        set -- $pair
        bytes="$1"; cap="$2"
        for session in $(seq 1 "$SESSIONS"); do
            fi_="$(first_impl_for "$session" "$bytes" "$cap")"
            bl="$(session_median baseline "$bytes" "$cap" "$session")"
            ca="$(session_median cached "$bytes" "$cap" "$session")"
            r="$(awk -v a="$ca" -v b="$bl" 'BEGIN { printf "%.4f", a / b }')"
            printf "| %s | %s | %s | %s | %s | %s | %s |\n" "$bytes" "$cap" "$session" "$fi_" "$bl" "$ca" "$r"
        done
    done
    echo
    echo "## Per-cell summary across the $SESSIONS session ratios"
    echo
    echo "MEDIAN / MIN / MAX are over the $SESSIONS paired ratios. SIGN counts how many"
    echo "sessions put cached ahead (< 1) and how many put baseline ahead (> 1)."
    echo "A cell whose $SESSIONS ratios do not all point the same way is **not"
    echo "directionally stable**, whatever its median ratio says. This is a"
    echo "descriptive criterion, not a significance test."
    echo
    echo "| bytes | capacity | median ratio | min | max | cached faster | baseline faster | direction |"
    echo "|---|---|---|---|---|---|---|---|"
    awk -F, '
        /^#/ { next }
        $1 == "message_bytes" { next }
        NF < 15 { next }
        {
            if ($14 == 4)        { verdict = "stable — cached 4/4" }
            else if ($15 == 4)   { verdict = "stable — baseline 4/4" }
            else if ($14 > 0 && $15 > 0) { verdict = "SPLIT " $14 "-" $15 }
            else                 { verdict = "mixed" }
            printf "| %s | %s | %s | %s | %s | %s | %s | %s |\n",
                   $1, $2, $11, $12, $13, $14, $15, verdict
        }' "$PAIRED_CSV"
    echo
    echo "## Reading these numbers"
    echo
    echo "- **MEDIAN RATIO** is the headline paired figure: < 1 means cached was faster,"
    echo "  > 1 means baseline was faster."
    echo "- **MIN / MAX** show whether that median is representative or is averaging"
    echo "  over sessions that disagreed. They are the extremes of $SESSIONS correlated"
    echo "  observations, not a confidence interval."
    echo "- **DIRECTION STABILITY** is the attribution check. \`stable\` means all"
    echo "  $SESSIONS sessions agreed; \`SPLIT\` means they did not, and **no directional"
    echo "  claim should be made for that cell** regardless of its median."
    echo "- These are **descriptive** statistics over $SESSIONS paired observations per"
    echo "  cell. They are not a hypothesis test and no p-value is implied."
    echo
    echo "## Limitations"
    echo
    echo "- $SESSIONS paired observations per cell is a small sample; \`stable\` means"
    echo "  \"$SESSIONS out of $SESSIONS agreed here\", not \"the effect is proven\"."
    echo "- No CPU pinning or affinity is used or claimed; macOS may migrate threads"
    echo "  mid-run and may place the two processes' threads on different core types."
    echo "- **This cell shape exhibits strong run-to-run and build-to-build regime"
    echo "  variation on the development host, and the swing between regimes is larger"
    echo "  than any plausible treatment effect.** A process can settle into a state"
    echo "  where the consumer spins on an empty queue tens of millions of times instead"
    echo "  of blocking on real handoffs. A separate diagnostic suggested code-layout"
    echo "  sensitivity as **one possible contributor** to that bimodality, but **Phase 3B"
    echo "  does not isolate its cause** — and no reproducible diagnostic package is"
    echo "  preserved alongside this dataset. Note also that the two variants are distinct"
    echo "  template instantiations with different emitted code, and the two legs are"
    echo "  independent processes that are not guaranteed to share scheduler placement,"
    echo "  core type, migration history, DVFS, thermal state or background load. The"
    echo "  \`producer_full_retries\` and \`consumer_empty_retries\` columns are preserved in"
    echo "  \`summary.csv\` and in every raw CSV precisely so the state of each process is"
    echo "  visible. **No ratio here should be read as resolving a difference smaller"
    echo "  than that swing.**"
    echo "- The mechanism leg (\`mechanism/\`, \`MECHANISM.md\`) is a SEPARATE set of"
    echo "  runs at a different instrumentation setting. Its throughput numbers are NOT"
    echo "  the canonical figures and are not used here."
    echo "- ns_per_message is end-to-end elapsed / messages delivered, including queue"
    echo "  synchronization, payload assignment, coherence traffic, harness"
    echo "  retry/backpressure and OS scheduling. It is not a per-call latency and not"
    echo "  a one-way handoff time."
} >"$PAIRED_MD"

# ---------------------------------------------------------------------------
# MECHANISM.md — the separate, instrumented remote-load measurement
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 3B — MEASURED MECHANISM"
    echo
    echo "> **This file is not the canonical result.** The numbers here come from"
    echo "> \`--instrument=1\` processes, which are a DIFFERENT instantiation from the"
    echo "> one the canonical throughput figures come from. The \`ns_per_message\`"
    echo "> values below are recorded for completeness and **must not be quoted as the"
    echo "> Phase-3B throughput result**."
    echo
    echo "## What is measured"
    echo
    echo "How many times each thread ACTUALLY read the opposite thread's cursor. In"
    echo "the baseline this equals the \`try_push\`/\`try_pop\` call count by"
    echo "construction. In the cached variant it counts real refresh loads — which is"
    echo "the mechanism the experiment is about."
    echo
    echo "The counters are **ordinary non-atomic members owned by one thread each**,"
    echo "incremented on the hot path and read only AFTER both threads have been"
    echo "joined. No global atomic was added to the timed hot path to count these."
    echo
    echo "## What this can and cannot show"
    echo
    echo "- It CAN show that the cached variant performs fewer remote cursor loads,"
    echo "  and by roughly how much, per cell."
    echo "- It CANNOT show why: there are no hardware performance counters here. This"
    echo "  file does not measure cache misses, coherence transactions, or line"
    echo "  invalidations, and nothing here may be described in those terms."
    echo "- **A reduction in remote loads does not imply a reduction in wall clock.**"
    echo "  The cached variant can refresh on every failed attempt while the queue is"
    echo "  genuinely full or empty, because the real remote cursor has not moved and"
    echo "  so the cached copy cannot improve. Section \"Where the mechanism does not"
    echo "  pay\" below is the place to look for exactly that."
    echo
    if [[ "$MECHANISM" == "1" ]]; then
        echo "## Remote loads per cell"
        echo
        echo "| bytes | capacity | variant | producer remote tail loads | consumer remote head loads | per message (prod) | per message (cons) |"
        echo "|---|---|---|---|---|---|---|"
        for pair in "${PAIRS[@]}"; do
            # shellcheck disable=SC2086
            set -- $pair
            for impl in "${IMPLS[@]}"; do
                s="$OUT/mechanism/summaries/${impl}_b${1}_c${2}.txt"
                ptl="$(grep '^producer_remote_tail_loads_total=' "$s" | cut -d= -f2)"
                chr="$(grep '^consumer_remote_head_loads_total=' "$s" | cut -d= -f2)"
                ppm="$(grep '^producer_remote_tail_loads_per_message=' "$s" | cut -d= -f2)"
                cpm="$(grep '^consumer_remote_head_loads_per_message=' "$s" | cut -d= -f2)"
                echo "| $1 | $2 | $impl | $ptl | $chr | $ppm | $cpm |"
            done
        done
        echo
        echo "## Reduction factor, cached vs baseline"
        echo
        echo "| bytes | capacity | producer loads: baseline -> cached | reduction | consumer loads: baseline -> cached | reduction |"
        echo "|---|---|---|---|---|---|"
        for pair in "${PAIRS[@]}"; do
            # shellcheck disable=SC2086
            set -- $pair
            bp="$(grep '^producer_remote_tail_loads_total=' "$OUT/mechanism/summaries/baseline_b${1}_c${2}.txt" | cut -d= -f2)"
            cp="$(grep '^producer_remote_tail_loads_total=' "$OUT/mechanism/summaries/cached_b${1}_c${2}.txt" | cut -d= -f2)"
            bc="$(grep '^consumer_remote_head_loads_total=' "$OUT/mechanism/summaries/baseline_b${1}_c${2}.txt" | cut -d= -f2)"
            cc="$(grep '^consumer_remote_head_loads_total=' "$OUT/mechanism/summaries/cached_b${1}_c${2}.txt" | cut -d= -f2)"
            # A cached count of zero means the mechanism was eliminated entirely
            # for that side, which is NOT a reduction factor of zero. The two
            # cases are rendered differently so the table cannot be misread as
            # "no improvement" when the improvement was total.
            reduction() {  # $1 = baseline count, $2 = cached count
                awk -v a="$1" -v b="$2" 'BEGIN {
                    if (b > 0)      { printf "%.3fx", a / b }
                    else if (a > 0) { printf "ALL (0 loads)" }
                    else            { printf "n/a (both 0)" }
                }'
            }
            rp="$(reduction "$bp" "$cp")"
            rc="$(reduction "$bc" "$cc")"
            echo "| $1 | $2 | $bp -> $cp | $rp | $bc -> $cc | $rc |"
        done
        echo
        echo "## Cells where the cached variant did MORE remote loads than the baseline"
        echo
        echo "These are the cases where the cached copy could not help: the remote"
        echo "cursor genuinely was not moving, so every failed attempt refreshed"
        echo "anyway. They are expected in this design and are called out rather than"
        echo "left for the reader to notice."
        echo
        echo "| bytes | capacity | side | baseline loads | cached loads |"
        echo "|---|---|---|---|---|"
        any_more=0
        for pair in "${PAIRS[@]}"; do
            # shellcheck disable=SC2086
            set -- $pair
            bp="$(grep '^producer_remote_tail_loads_total=' "$OUT/mechanism/summaries/baseline_b${1}_c${2}.txt" | cut -d= -f2)"
            cp="$(grep '^producer_remote_tail_loads_total=' "$OUT/mechanism/summaries/cached_b${1}_c${2}.txt" | cut -d= -f2)"
            bc="$(grep '^consumer_remote_head_loads_total=' "$OUT/mechanism/summaries/baseline_b${1}_c${2}.txt" | cut -d= -f2)"
            cc="$(grep '^consumer_remote_head_loads_total=' "$OUT/mechanism/summaries/cached_b${1}_c${2}.txt" | cut -d= -f2)"
            if awk -v a="$bp" -v b="$cp" 'BEGIN { exit !(b > a) }'; then
                echo "| $1 | $2 | producer | $bp | $cp |"
                any_more=1
            fi
            if awk -v a="$bc" -v b="$cc" 'BEGIN { exit !(b > a) }'; then
                echo "| $1 | $2 | consumer | $bc | $cc |"
                any_more=1
            fi
        done
        if [[ "$any_more" == "0" ]]; then
            echo
            echo "_No cell in this dataset had the cached variant performing more_"
            echo "_remote loads than the baseline._"
        fi
        echo
        echo "## Where the mechanism does not pay"
        echo
        echo "Compare the two tables above with \`summary.csv\`. The important case is a"
        echo "cell where the producer's remote loads fall dramatically and the"
        echo "throughput does NOT improve, or gets worse. The consumer's refresh count"
        echo "is the one to watch: a consumer that keeps finding the queue empty cannot"
        echo "learn anything from a cached \`head\`, because the real \`head\` has not"
        echo "moved — so it refreshes on every failed attempt, which is exactly as many"
        echo "remote loads as the baseline performs, plus the extra comparison. That is"
        echo "a real property of this design and is reported as such, not smoothed over."
    else
        echo "## Not run"
        echo
        echo "The instrumented mechanism leg was skipped (\`MECHANISM=0\`). No remote-load"
        echo "counts exist for this dataset, and none may be claimed."
    fi
} >"$OUT/MECHANISM.md"

# ---------------------------------------------------------------------------
# HOST.md — host and toolchain metadata
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 3B — host and toolchain metadata"
    echo
    echo "Measured with \`$BIN_NAME\` built from repo HEAD"
    echo "\`$(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')\`."
    echo
    echo "**Cache-line size.** The host reports its cache-line size at runtime; the"
    echo "benchmark queries it before timing anything and refuses to run if the report"
    echo "exceeds the compile-time layout assumption. On the canonical M3 Max host"
    echo "this is **128 bytes**, not the 64 most code assumes."
    echo
    echo "| property | value |"
    echo "|---|---|"
    echo "| hostname | \`$(hostname)\` |"
    echo "| uname | \`$(uname -a)\` |"
    echo "| macOS | \`$(sw_vers -productVersion 2>/dev/null || echo unknown)\` |"
    echo "| CPU | \`$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)\` |"
    echo "| logical CPUs | $(sysctl -n hw.ncpu 2>/dev/null || echo unknown) |"
    echo "| performance cores | $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo unknown) |"
    echo "| efficiency cores | $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo unknown) |"
    echo "| reported cache line | $(sysctl -n hw.cachelinesize 2>/dev/null || echo unknown) bytes |"
    echo "| compiler | \`$(/usr/bin/c++ --version 2>/dev/null | head -2 | tr '\n' ' ')\` |"
    echo "| build type | Release, forced \`-O3 -DNDEBUG\` |"
    echo "| extra arch flags | \`${ARCH_FLAGS}\` (empty = compiler default) |"
    echo "| load average at start | \`$(uptime | sed 's/.*load averages: //')\` |"
    echo
    echo "Thread placement is NOT controlled: no affinity, no pinning and no thread"
    echo "priority is set anywhere in this experiment, and macOS may migrate either"
    echo "thread mid-run or place the two processes on different core types."
    echo
    echo "## Reported by the measured processes themselves"
    echo
    echo "\`\`\`"
    grep -h '^cache_line_size_reported=' "$OUT"/summaries/*.txt 2>/dev/null | sort -u || true
    grep -h '^cursor_policy_size=' "$OUT"/summaries/*.txt 2>/dev/null | sort -u || true
    grep -h '^cursor_layout=' "$OUT"/summaries/*.txt 2>/dev/null | sort -u || true
    echo "\`\`\`"
} >"$OUT/HOST.md"

# ---------------------------------------------------------------------------
# PROVENANCE.md — what exactly produced this dataset
# ---------------------------------------------------------------------------
hash_file() {  # $1=path -> "sha256  path", or a NOT_PRESENT line
    if [[ -f "$1" ]]; then
        printf '%s  %s\n' "$(shasum -a 256 "$1" | cut -d' ' -f1)" "$1"
    else
        printf 'NOT_PRESENT  %s\n' "$1"
    fi
}

{
    echo "# Experiment 02 Phase 3B — provenance"
    echo
    echo "## Repository state"
    echo
    echo '```'
    echo "HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "describe: $(git describe --always --dirty 2>/dev/null || echo n/a)"
    echo "branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo n/a)"
    echo '```'
    echo
    echo "### \`git status --porcelain\` at collection time"
    echo
    echo '```'
    git status --porcelain 2>/dev/null || echo "# not a git repository"
    echo '```'
    echo
    echo "A clean tree is preferred. If anything is listed above, the dataset was"
    echo "produced from a working tree that differed from the recorded HEAD, and the"
    echo "source hashes below are the authoritative record of what was compiled."
    echo
    echo "## Source hashes"
    echo
    echo '```'
    hash_file include/spsc_remote_cursor_ring_buffer.h
    hash_file include/spsc_cursor_layout_ring_buffer.h
    hash_file include/spsc_ring_buffer.h
    hash_file benchmark/spsc_remote_cursor_bench.cpp
    hash_file tests/spsc_remote_cursor_tests.cpp
    hash_file CMakeLists.txt
    hash_file scripts/spsc-remote-cursor.sh
    echo '```'
    echo
    echo "## Binaries"
    echo
    echo '```'
    hash_file "$BENCH"
    echo '```'
    echo
    echo "## Exact commands"
    echo
    echo "Every effective invocation, in the order it ran, is recorded verbatim in"
    echo "\`command.txt\`. The canonical matrix and the mechanism leg are separate"
    echo "sections there."
    echo
    echo '```'
    echo "cmake -S . -B $BUILDDIR -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DBENCH_ARCH_FLAGS=\"$ARCH_FLAGS\""
    echo "cmake --build $BUILDDIR --target $BIN_NAME -j"
    echo '```'
    echo
    echo "## What was NOT done"
    echo
    echo "- No frozen Phase-1, Phase-2, Phase-3A or Phase-4 result file was read,"
    echo "  modified or regenerated by this run."
    echo "- Phase 3A was NOT rerun. The historical Phase-3A same-line vs separated"
    echo "  result is untouched and remains where it was."
    echo "- No source file belonging to another phase was modified."
    echo "- The canonical dataset was produced only by uninstrumented"
    echo "  (\`--instrument=0\`) processes; the instrumented mechanism leg lives in"
    echo "  \`mechanism/\` and is labelled separately everywhere it appears."
} >"$OUT/PROVENANCE.md"

# ---------------------------------------------------------------------------
# invariants.txt + RESULTS_METADATA.md
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 3B — dataset invariants"
    echo "# Every line below was CHECKED, not assumed. Any violation aborted the run."
    echo "expected_processes=$EXPECTED_PROCESSES"
    echo "recorded_canonical_invocations=$recorded"
    echo "canonical_invocations_instrumented=$instr_in_canonical"
    echo "raw_csv_files=$raw_count"
    echo "sessions=$SESSIONS"
    echo "messages_per_repetition=$MESSAGES"
    echo "reps_per_process=$REPS"
    echo "warmup_reps_per_process=$WARMUP"
    echo "mechanism_leg=$MECHANISM"
    echo "mechanism_processes=$([[ "$MECHANISM" == "1" ]] && echo "$EXPECTED_MECH_PROCESSES" || echo 0)"
    echo "mechanism_reps=$MECH_REPS"
    echo "session_first_variant_order=$recorded_first"
    echo "ab_ba_balance=2 baseline-first + 2 cached-first per cell"
    echo "pair_adjacency=VERIFIED"
    echo "footprint_equality_across_pairs=VERIFIED"
    echo "separated_layout_verified_on_every_row=VERIFIED"
    echo "cached_placement_verified_on_every_row=VERIFIED"
    echo "instrumentation_leak_check=PASS"
    echo "summary_vs_raw_verification=VERIFIED for every process"
} >"$OUT/invariants.txt"

{
    echo "# Experiment 02 Phase 3B — results metadata"
    echo
    echo '```'
    cat "$OUT/invariants.txt"
    echo '```'
    echo
    echo "## Files"
    echo
    echo "| path | contents |"
    echo "|---|---|"
    echo "| \`command.txt\` | every effective invocation, in execution order |"
    echo "| \`raw/\` | one raw per-repetition CSV per canonical process ($EXPECTED_RAW_FILES files) |"
    echo "| \`summaries/\` | one process summary per canonical process |"
    echo "| \`stderr/\` | the layout-verification trace of each canonical process |"
    echo "| \`mechanism/\` | the instrumented leg: raw, summaries and stderr |"
    echo "| \`summary.csv\` | pooled per-process view (SECONDARY) |"
    echo "| \`paired_summary.csv\` | the paired ratios (PRIMARY), machine-readable |"
    echo "| \`PAIRED_COMPARISON.md\` | the paired analysis (PRIMARY) |"
    echo "| \`MECHANISM.md\` | MEASURED MECHANISM (remote loads), not performance |"
    echo "| \`MATRIX.md\` | human-readable pooled matrix |"
    echo "| \`SESSIONS.md\` | per-session process medians |"
    echo "| \`LAYOUT_VERIFICATION.md\` | runtime placement and footprint evidence |"
    echo "| \`invariants.txt\` | the checked dataset invariants |"
    echo "| \`run_order.txt\` | the execution order parsed back out of command.txt |"
    echo "| \`HOST.md\` | host and toolchain metadata |"
    echo "| \`PROVENANCE.md\` | repo state, source and binary hashes, exact commands |"
    echo
    echo "## Status"
    echo
    echo "**Phase 3B: collected.** The canonical status is recorded in"
    echo "\`docs/SPSC_REMOTE_CURSOR_CACHE.md\` and in the top-level README, not here."
    echo "This file describes the dataset; it does not declare the phase complete."
} >"$OUT/RESULTS_METADATA.md"

# run_order.txt is a derived working file; keep it, it is referenced by the docs.
echo
echo "==> Done."
echo "    canonical: $OUT"
echo "    processes: $EXPECTED_PROCESSES canonical + $([[ "$MECHANISM" == "1" ]] && echo "$EXPECTED_MECH_PROCESSES" || echo 0) mechanism"
echo "    primary:   $PAIRED_MD"
echo "    mechanism: $OUT/MECHANISM.md"
