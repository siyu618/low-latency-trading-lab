#!/usr/bin/env bash
# Experiment 02 Phase 2 — canonical throughput runner.
#
# Runs the complete 2 x 3 x 3 matrix of the deterministic two-thread
# end-to-end message-transfer benchmark:
#
#   impl      x message_bytes x capacity
#   mutex,spsc x 8,32,64      x 1024,4096,65536     = 18 cells
#
# ONE IMPLEMENTATION PER PROCESS. Every cell is a SEPARATE invocation of
# spsc_throughput_bench choosing exactly one --impl; the mutex and SPSC queues
# are never timed inside one interval, one address space, or one warmed-up
# process state. That separation is the whole point of the matrix, so this
# script never passes --impl=both (the benchmark rejects it).
#
# MEASUREMENT DISCIPLINE (see docs/SPSC_THROUGHPUT.md):
#   * Release build, -O3 -DNDEBUG, forced by the CMake target itself.
#   * Thread creation, queue allocation and the expected-checksum pre-pass all
#     happen outside the timed interval; the consumer records t1 itself.
#   * ns_per_message is END-TO-END elapsed / messages DELIVERED. It is NOT a
#     per-call latency and NOT a one-way handoff time.
#   * An untimed, unreported warm-up precedes the measured repetitions. All
#     measured repetitions are kept — this script never drops a slow run.
#   * Every cell must self-validate (all N delivered, sequence exact, checksum
#     matching an independently precomputed stream). A cell that fails is NOT
#     published: the benchmark exits non-zero and this script stops there.
#
# PRESERVE-BEFORE-SUMMARIZE. Every raw per-repetition CSV is written first,
# untouched. Only after every cell has finished does the script derive the
# matrix summary, and it then RE-VERIFIES each cell's median/min/max against the
# raw CSV it claims to summarize — a summary that does not match its own raw
# data fails the run loudly. No number here is ever the output of a second,
# independent run.
#
# RAW OUTPUTS ARE NEVER FABRICATED OR EDITED. This script only invokes the
# benchmark and records what the machine measured. If the host is not the
# canonical development host, the results are still real — they are simply not
# the canonical M3 Max dataset, and HOST.md records what actually ran.
#
# Usage:
#   scripts/spsc-throughput.sh                 # canonical matrix
#   FORCE=1 scripts/spsc-throughput.sh         # overwrite an existing results dir
#   MESSAGES=200000 REPS=3 scripts/spsc-throughput.sh   # smoke-sized matrix
#
# WHY MULTIPLE SESSIONS: a two-thread measurement on an unpinned, unpadded
# baseline is sensitive to where the OS puts the two threads and where the
# allocator puts the queue, and that placement is fixed for the life of a
# process. A single process therefore samples ONE placement, and this host has
# been observed to hand out placements that differ by ~3x for byte-identical
# machine code. SESSIONS independent processes per cell sample several
# placements; their repetitions are POOLED (never filtered) so the reported
# median and spread describe the distribution the machine actually produced
# rather than one lucky or unlucky launch. Session 1 is the canonical session.
#
# Env overrides:
#   MESSAGES  messages per repetition (default 10000000)
#   REPS      measured repetitions per cell per session (default 5, all kept)
#   SESSIONS  independent processes per cell (default 3, all kept)
#   WARMUP    untimed warm-up repetitions per process (default 1)
#   OUT       results directory (default docs/results/spsc-throughput)
#   FORCE     1 = allow writing into a non-empty OUT directory
#   BUILDDIR  build directory (default build-spsc-perf)

set -euo pipefail
cd "$(dirname "$0")/.."

REPO_ROOT="$(pwd)"
BIN_NAME="spsc_throughput_bench"

MESSAGES="${MESSAGES:-10000000}"
REPS="${REPS:-5}"
SESSIONS="${SESSIONS:-3}"
WARMUP="${WARMUP:-1}"
OUT="${OUT:-docs/results/spsc-throughput}"
FORCE="${FORCE:-0}"
BUILDDIR="${BUILDDIR:-build-spsc-perf}"

# Recorded in command.txt, so it must reflect the directory actually used.
BENCH_REL="$BUILDDIR/$BIN_NAME"

if [[ "$SESSIONS" -lt 1 ]]; then
    echo "FATAL: SESSIONS must be >= 1 (got $SESSIONS)" >&2
    exit 1
fi

# Canonical matrix, in run order. bash 3.2 (macOS) has no associative arrays, so
# this stays a flat list of "impl bytes capacity" triples.
CELLS=()
for impl in mutex spsc; do
    for bytes in 8 32 64; do
        for cap in 1024 4096 65536; do
            CELLS+=("$impl $bytes $cap")
        done
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
# Leg 1 — run every cell, preserving raw output. Nothing is summarized yet.
# ---------------------------------------------------------------------------
TOTAL_RUNS=$(( ${#CELLS[@]} * SESSIONS ))
echo
echo "==> Running the canonical matrix: ${#CELLS[@]} cells x $SESSIONS session(s)"
echo "    = $TOTAL_RUNS processes, ONE implementation per process"
echo "    messages=$MESSAGES reps=$REPS warmup=$WARMUP"

: >"$OUT/command.txt"
{
    echo "# Experiment 02 Phase 2 canonical run — exact effective invocations"
    echo "# UTC:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# local:     $(date +%Y-%m-%dT%H:%M:%S%z)"
    echo "# repo HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "# messages=$MESSAGES reps=$REPS warmup=$WARMUP sessions=$SESSIONS"
    echo "# each line below is a SEPARATE process with exactly one implementation"
    echo
} >>"$OUT/command.txt"

for session in $(seq 1 "$SESSIONS"); do
    cell_index=0
    for cell in "${CELLS[@]}"; do
        # shellcheck disable=SC2086
        set -- $cell
        impl="$1"; bytes="$2"; cap="$3"
        cell_index=$((cell_index + 1))
        cell_id="${impl}_b${bytes}_c${cap}"
        raw="$OUT/raw/${cell_id}_s${session}.csv"
        summary="$OUT/summaries/${cell_id}_s${session}.txt"

        echo "    [session $session/$SESSIONS] [$cell_index/${#CELLS[@]}] $cell_id"

        # Build the invocation as an array and serialize it with %q quoting
        # (bash 3.2 has no ${arr[@]@Q}), so command.txt records exactly what ran.
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
echo "==> All $TOTAL_RUNS processes completed and validated (raw output preserved)"

# ---------------------------------------------------------------------------
# Leg 2 — verify every summary against its own raw CSV, then summarize.
# ---------------------------------------------------------------------------
echo
echo "==> Verifying each summary against the raw CSV it summarizes"

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
    for cell in "${CELLS[@]}"; do
        # shellcheck disable=SC2086
        set -- $cell
        cell_id="${1}_b${2}_c${3}"
        verify_cell "$OUT/raw/${cell_id}_s${session}.csv" \
                    "$OUT/summaries/${cell_id}_s${session}.txt" \
                    "${cell_id}_s${session}"
    done
done
echo "    all $TOTAL_RUNS summaries match their own raw repetition data"

# ---- pooled per-cell statistics (no new measurement happens here) ----------
# Pool EVERY measured repetition of EVERY session for a cell and recompute the
# median/min/max over the pool. Nothing is dropped and no session is preferred;
# pooled_reps is printed so a cell that lost a session cannot pass unnoticed.
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
    echo "# each of which ran exactly one implementation."
    echo "# message_count=$MESSAGES reps_per_session=$REPS sessions=$SESSIONS warmup_reps=$WARMUP"
    echo "# UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "impl,message_bytes,capacity,message_count,pooled_reps,cell,median_ns_per_message,min_ns_per_message,max_ns_per_message,spread_pct,median_messages_per_second,producer_full_retries_total,consumer_empty_retries_total,correctness"
    for cell in "${CELLS[@]}"; do
        # shellcheck disable=SC2086
        set -- $cell
        pool_cell "${1}_b${2}_c${3}"
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
    echo "- message_count: $MESSAGES per repetition"
    echo "- measured repetitions: $REPS per session x $SESSIONS independent processes"
    echo "- warm-up (excluded): $WARMUP per process"
    echo "- UTC: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "| impl | bytes | capacity | pooled reps | median ns/msg | min | max | spread | median msg/s | full retries | empty retries |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|"
    for cell in "${CELLS[@]}"; do
        # shellcheck disable=SC2086
        set -- $cell
        pool_cell "${1}_b${2}_c${3}" | awk -F, '{
            printf "| %s | %s | %s | %s | %s | %s | %s | %s%% | %s | %s | %s |\n",
                   $1, $2, $3, $5, $7, $8, $9, $10, $11, $12, $13 }'
    done
} >"$MATRIX_MD"

# ---- per-session medians, so placement-to-placement spread is visible ------
SESSION_MD="$OUT/SESSIONS.md"
{
    echo "# Experiment 02 Phase 2 — per-session medians"
    echo
    echo "Each session is a separate process. Session-to-session differences show how"
    echo "much of a cell's spread is process-placement (not repetition) variance."
    echo "All values are ns per message, END-TO-END."
    echo
    printf '%s' "| impl | bytes | capacity |"
    for session in $(seq 1 "$SESSIONS"); do printf " session %s |" "$session"; done
    printf '%s\n' " pooled |"
    # printf would read a leading '-' in the format as an option, so pass the
    # separator row as an argument instead.
    printf '%s' "|---|---|---|"
    for session in $(seq 1 "$SESSIONS"); do printf '%s' "---|---"; done
    printf '%s\n' "---|"
    for cell in "${CELLS[@]}"; do
        # shellcheck disable=SC2086
        set -- $cell
        cell_id="${1}_b${2}_c${3}"
        printf "| %s | %s | %s |" "$1" "$2" "$3"
        for session in $(seq 1 "$SESSIONS"); do
            v="$(grep '^median_ns_per_message=' "$OUT/summaries/${cell_id}_s${session}.txt" | cut -d= -f2)"
            printf " %s |" "$v"
        done
        printf " %s |\n" "$(pool_cell "$cell_id" | awk -F, '{print $7}')"
    done
} >"$SESSION_MD"

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
echo "    raw per-repetition data : $OUT/raw/          ($TOTAL_RUNS files)"
echo "    per-cell summaries      : $OUT/summaries/    ($TOTAL_RUNS files)"
echo "    benchmark stderr        : $OUT/stderr/"
echo "    matrix (derived)        : $MATRIX"
echo "    matrix (human)          : $MATRIX_MD"
echo "    per-session medians     : $SESSION_MD"
echo "    exact commands          : $OUT/command.txt"
echo "    host/toolchain          : $OUT/HOST.md"
echo
echo "    Interpretation belongs in docs/SPSC_THROUGHPUT.md — not in this script."
