#!/usr/bin/env bash
# Experiment 02 Phase 4 — canonical SPSC TAIL LATENCY / JITTER runner.
#
# Runs the complete 3 x 3 matrix of the Phase-4 benchmark:
#
#   message_bytes x capacity
#   16,32,64      x 1024,4096,65536   = 9 cells
#
# over FOUR sessions, in forward / reverse / forward / reverse traversal order.
#
# ---------------------------------------------------------------------------
# WHAT THIS DESIGN DOES AND DOES NOT DO
# ---------------------------------------------------------------------------
# The four sessions with two traversal directions BALANCE the temporal position
# of each cell: no cell is always measured first, and none is always measured
# last. That is the whole claim. It is NOT an A/B/BA crossover of two treatments
# — there is only ONE queue in this matrix — and it must NOT be described as
# eliminating scheduler variation, core migration or DVFS drift. Those are not
# controlled here; the ordering only prevents them from being confounded with
# cell identity.
#
# ONE (cell, session) PER PROCESS, by design. A process runs the excluded warm-up
# and then the measured repetitions for exactly one cell, so no two cells share
# an address space, a warmed-up cache state or a process lifetime.
#
# ---------------------------------------------------------------------------
# THE MEASURED QUANTITY
# ---------------------------------------------------------------------------
# producer_ready -> consumer_received, stamped immediately before the producer's
# try_push retry loop and immediately after a successful try_pop. It INCLUDES
# the producer's retry/backpressure wait, the queue's release/acquire
# synchronization, the payload copy, the consumer's empty-retry loop and both
# clock reads. It is NOT "pure queue residence time" and must never be called
# that.
#
# ---------------------------------------------------------------------------
# THE QUEUE
# ---------------------------------------------------------------------------
# SEPARATED-CURSOR BASELINE only: acquire/release publication, separated head and
# tail cache lines, baseline remote cursor loads, NO remote-cursor cache. The
# Phase-3B cached variant, the mutex queue and the Phase-3A same-line layout are
# deliberately absent, so every number here is attributable to one queue.
#
# Absolute Phase-4 values must NOT be compared with Phase-2 or Phase-3 values:
# the message shapes are 16/32/64 (not 8/32/64) and the measured quantity is a
# latency distribution, not an end-to-end rate.
#
# Usage:
#   scripts/spsc-tail-latency.sh              # full canonical run
#   FORCE=1 scripts/spsc-tail-latency.sh      # overwrite an existing dataset
#   SMOKE=1 scripts/spsc-tail-latency.sh      # tiny run, non-canonical, into OUT
set -euo pipefail

cd "$(dirname "$0")/.."

BIN_NAME="spsc_tail_latency_bench"
FORCE="${FORCE:-0}"
BUILDDIR="${BUILDDIR:-build-spsc-tail}"
ARCH_FLAGS="${BENCH_ARCH_FLAGS:-}"
SMOKE="${SMOKE:-0}"

# The defaults are chosen from the mode FIRST. Setting them in one place and
# then trying to override them in the smoke branch does not work: `X="${X:-d}"`
# leaves X non-empty, so a later `X="${X:-smoke}"` is dead code — which is how
# an earlier version of this script wrote a smoke run into the CANONICAL output
# directory. One branch, one set of defaults, no override that cannot fire.
if [[ "$SMOKE" == "1" ]]; then
    # Non-canonical: same shape and same output LAYOUT, tiny counts, so the whole
    # pipeline including the verifier can be exercised before the real run. It
    # writes to its own directory and never to the canonical one.
    MESSAGES="${MESSAGES:-200000}"
    REPS="${REPS:-3}"
    WARMUP="${WARMUP:-1}"
    SETTLING="${SETTLING:-2000}"
    INTERVAL="${INTERVAL:-1021}"
    CALIB_REPS="${CALIB_REPS:-20000}"
    OUT="${OUT:-build-spsc-tail/smoke-spsc-tail-latency}"
else
    MESSAGES="${MESSAGES:-10000000}"
    REPS="${REPS:-5}"
    WARMUP="${WARMUP:-1}"
    SETTLING="${SETTLING:-100000}"
    INTERVAL="${INTERVAL:-1021}"
    CALIB_REPS="${CALIB_REPS:-200000}"
    OUT="${OUT:-docs/results/spsc-tail-latency}"
fi

BENCH_REL="$BUILDDIR/$BIN_NAME"

# The balanced design is defined for exactly four sessions in this traversal
# order. Anything else silently loses the balance guarantee, so it is not
# offered as an option.
SESSIONS=4
CELLS_PER_SESSION=9
EXPECTED_PROCESSES=$(( CELLS_PER_SESSION * SESSIONS ))   # 36
EXPECTED_MEASURED_REPETITIONS=$(( EXPECTED_PROCESSES * REPS ))  # 180 canonical

# Canonical cells in forward traversal order: bytes then capacity, each
# ascending. bash 3.2 (macOS) has no associative arrays, so this stays a flat
# list of "bytes capacity" pairs.
PAIRS=(
    "16 1024" "16 4096" "16 65536"
    "32 1024" "32 4096" "32 65536"
    "64 1024" "64 4096" "64 65536"
)

if [[ "$REPS" -lt 1 ]]; then echo "FATAL: REPS must be >= 1 (got $REPS)" >&2; exit 1; fi
if [[ "$WARMUP" -lt 0 ]]; then echo "FATAL: WARMUP must be >= 0 (got $WARMUP)" >&2; exit 1; fi
if [[ "$SETTLING" -ge "$MESSAGES" ]]; then
    echo "FATAL: SETTLING ($SETTLING) must be smaller than MESSAGES ($MESSAGES)" >&2; exit 1
fi
if [[ $(( INTERVAL % 2 )) -eq 0 || "$INTERVAL" -le 0 ]]; then
    echo "FATAL: INTERVAL must be a positive ODD number (got $INTERVAL); an odd" >&2
    echo "       interval is coprime with every power-of-two capacity." >&2
    exit 1
fi

# Session 1 forward, 2 reverse, 3 forward, 4 reverse.
session_traversal() {
    case "$1" in
        1|3) echo forward ;;
        2|4) echo reverse ;;
        *)   echo "FATAL: no traversal defined for session $1" >&2; exit 1 ;;
    esac
}

EXPECTED_SAMPLES=$(( (MESSAGES - SETTLING) / INTERVAL ))

# ---------------------------------------------------------------------------
# Host load pre-flight
# ---------------------------------------------------------------------------
# This is a latency measurement on two unpinned spinning threads. If the machine
# is saturated, the distribution reflects the host's scheduler rather than the
# queue: an earlier run of this script, executed while sanitizer builds and
# sweeps ran beside it, put 95% of the whole experiment's measured wall time into
# 8 of its 180 repetitions and moved p50 by three orders of magnitude in them —
# while passing every correctness check the benchmark can perform on itself. See
# docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/.
#
# BE CLEAR ABOUT WHAT THIS CHECK IS AND IS NOT. The default is the LOGICAL CPU
# COUNT, so it fires only when the host is saturated — it deliberately permits
# ordinary desktop background activity (an IDE, a browser, a terminal), because
# that is the ambient condition this repo's frozen Phase 2/3A/3B datasets were
# measured under, and a threshold that forbade it would make this runner
# unusable on the machine it was written for.
#
# It is a coarse pre-flight, NOT the authoritative guard. It would NOT have
# caught the incident above: that run started at a load average of 6.10 on 16
# CPUs, below any sane threshold, and the load arrived *during* the run. The
# guard that actually caught it is the ns_per_message consistency check in
# scripts/verify-spsc-tail-summary.py, which reads every repetition afterwards
# and refuses the dataset if any repetition ran far slower than its own cell's
# median. This check exists to save you a wasted run; that one exists to stop a
# bad dataset being published. Set MAX_LOADAVG=0 to disable this one; there is no
# switch for the other, and there should not be.
MAX_LOADAVG="${MAX_LOADAVG:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

read_loadavg_1min() {
    # macOS: sysctl prints "{ 1.23 4.56 7.89 }". Linux: /proc/loadavg.
    if [[ -r /proc/loadavg ]]; then
        cut -d' ' -f1 /proc/loadavg
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n vm.loadavg 2>/dev/null | tr -d '{}' | awk '{print $1}'
    else
        echo "unknown"
    fi
}

LOADAVG_START="$(read_loadavg_1min)"
UPTIME_START="$(uptime | sed 's/.*load averages: //')"

if [[ "$MAX_LOADAVG" != "0" ]]; then
    if [[ "$LOADAVG_START" == "unknown" ]]; then
        echo "WARNING: cannot read the 1-minute load average on this host; the" >&2
        echo "         pre-flight load check is being SKIPPED. Verify the host was" >&2
        echo "         otherwise idle before citing this dataset." >&2
    elif awk -v l="$LOADAVG_START" -v m="$MAX_LOADAVG" 'BEGIN{exit !(l > m)}'; then
        cat >&2 <<EOF
FATAL: 1-minute load average is $LOADAVG_START but MAX_LOADAVG=$MAX_LOADAVG.

Refusing to run. This is a latency measurement on two unpinned spinning
threads; on a saturated host the measured distribution reflects the host's
scheduler, not the queue. Wait for the machine to quiesce — including build
jobs, test runs and anything else you started — then re-run.

Note that the load average is only a pre-flight. It cannot see load that
arrives AFTER it is read, and it cannot see a host that was busy earlier. The
authoritative check runs at the end, over every repetition, in
scripts/verify-spsc-tail-summary.py.

To override anyway: MAX_LOADAVG=0 $0
Any dataset produced under that override must be labelled as
load-uncontrolled, because the load average is then unrecorded and unenforced.
EOF
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Guard against clobbering an existing dataset
# ---------------------------------------------------------------------------
if [[ -e "$OUT" && "$FORCE" != "1" && "$SMOKE" != "1" ]]; then
    echo "FATAL: $OUT already exists. Re-run with FORCE=1 to overwrite." >&2
    exit 1
fi
# Always start from an empty tree: a leftover file from an earlier run would be
# picked up by the verifier as a live result.
rm -rf "$OUT"
mkdir -p "$OUT/raw" "$OUT/summaries" "$OUT/calibration" "$OUT/stderr"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
echo "==> Configuring and building $BIN_NAME (Release, forced -O3 -DNDEBUG)"
cmake -S . -B "$BUILDDIR" -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS="$ARCH_FLAGS" >/dev/null
cmake --build "$BUILDDIR" --target "$BIN_NAME" -j >/dev/null
BENCH="$BUILDDIR/$BIN_NAME"
if [[ ! -x "$BENCH" ]]; then echo "FATAL: $BENCH is not executable" >&2; exit 1; fi

echo
echo "==> Phase 4 TAIL LATENCY over the SEPARATED-CURSOR BASELINE queue"
echo "    output           : $OUT"
echo "    load avg (1 min) : $LOADAVG_START (pre-flight limit $MAX_LOADAVG; 0 = disabled)"
echo "    cells            : 3 message sizes x 3 capacities = $CELLS_PER_SESSION"
echo "    sessions         : $SESSIONS (forward / reverse / forward / reverse)"
echo "    processes        : $EXPECTED_PROCESSES"
echo "    messages/process : $MESSAGES (settling prefix $SETTLING)"
echo "    measured reps    : $REPS (+$WARMUP excluded warm-up)"
if [[ "$SMOKE" == "1" ]]; then
    echo "    *** SMOKE MODE: NOT the canonical dataset ***"
fi
echo

# ---------------------------------------------------------------------------
# command.txt — every effective invocation, in the order it ran
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 4 — SPSC tail latency / jitter: exact invocations."
    echo "#"
    echo "# repo HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo "# binary: $BENCH_REL"
    echo "# messages=$MESSAGES settling_prefix=$SETTLING sample_interval=$INTERVAL"
    echo "# reps=$REPS warmup=$WARMUP sessions=$SESSIONS"
    echo "# expected_samples_per_repetition=$EXPECTED_SAMPLES"
    echo "#"
    echo "# THE MEASURED QUANTITY is producer_ready -> consumer_received: stamped"
    echo "# immediately before the producer's try_push retry loop and immediately"
    echo "# after a successful try_pop, before any validation. It INCLUDES the"
    echo "# producer's retry/backpressure wait, the queue's release/acquire"
    echo "# synchronization, the payload copy, the consumer's empty-retry loop and"
    echo "# both clock reads. It is NOT pure queue residence time and NOT a"
    echo "# per-call queue cost."
    echo "#"
    echo "# THE QUEUE is the SEPARATED-CURSOR BASELINE only. The Phase-3B cached"
    echo "# variant, the mutex queue and the Phase-3A same-line layout are NOT in"
    echo "# this matrix. Phase-4 absolute values must NOT be compared with Phase-2"
    echo "# or Phase-3 absolute values: the message shapes are 16/32/64, not 8/32/64."
    echo "#"
    echo "# THE SESSION ORDER balances temporal position (forward, reverse, forward,"
    echo "# reverse). It does NOT eliminate scheduler variation, core migration or"
    echo "# DVFS drift, and must not be described as doing so."
    echo "#"
    echo "# --sample-interval=$INTERVAL is deliberately ODD, so it is coprime with"
    echo "# every power-of-two capacity and the sequence-keyed sparse sample"
    echo "# rotates through all ring positions. It is not 1024."
    echo "#"
    echo "# INSTRUMENTATION IS SPARSE. The producer stamps a timestamp into the"
    echo "# message itself for the sampled sequences only, and the consumer reads the"
    echo "# clock on receipt of those same sequences — both sides evaluate the same"
    echo "# sequence-keyed schedule independently. $EXPECTED_SAMPLES clock reads per"
    echo "# thread per repetition, NOT $MESSAGES. The summary records the counts"
    echo "# actually taken and the repetition FAILS if either differs. There is no"
    echo "# timestamp side array: the stamp travels producer -> SPSC message ->"
    echo "# consumer."
    echo "#"
    echo "# ONE (cell, session) PER PROCESS: no two cells share an address space."
} >"$OUT/command.txt"

# ---------------------------------------------------------------------------
# The canonical run
# ---------------------------------------------------------------------------
for session in $(seq 1 "$SESSIONS"); do
    traversal="$(session_traversal "$session")"

    {
        echo
        echo "# ---- session $session/$SESSIONS | $traversal traversal ----"
    } >>"$OUT/command.txt"

    echo
    echo "==> Session $session/$SESSIONS — $traversal traversal"

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

        process_index=$(( process_index + 1 ))
        cell_id="b${bytes}_c${cap}"
        raw="$OUT/raw/${cell_id}_s${session}.csv"
        summary="$OUT/summaries/${cell_id}_s${session}.csv"
        calib="$OUT/calibration/${cell_id}_s${session}.csv"

        echo "    [$process_index/$EXPECTED_PROCESSES] s$session $cell_id"

        # The invocation is built as an array and serialized with %q quoting
        # (bash 3.2 has no ${arr[@]@Q}), so command.txt records exactly what ran,
        # in the order it ran.
        cmd=("$BENCH_REL" "--message-bytes=$bytes" "--capacity=$cap"
             "--messages=$MESSAGES" "--reps=$REPS" "--warmup=$WARMUP"
             "--settling=$SETTLING" "--sample-interval=$INTERVAL"
             "--session=$session" "--calibration-reps=$CALIB_REPS"
             "--raw-out=$raw" "--summary-out=$summary"
             "--calibration-out=$calib")
        for a in "${cmd[@]}"; do printf '%q ' "$a"; done >>"$OUT/command.txt"
        printf '\n' >>"$OUT/command.txt"

        # A failing cell aborts the run: set -e propagates the benchmark's
        # non-zero exit, and nothing downstream publishes the cell.
        #
        #   exit 2  a malformed invocation was refused
        #   exit 3  the cursor layout on the measured object was not the
        #           verified Phase-3A separated placement
        #   exit 4  a repetition failed its own invariants (delivery, sequence,
        #           payload validation, timestamp ordering, or the observed
        #           sample count differing from the derived one)
        "$BENCH" --message-bytes="$bytes" --capacity="$cap" \
                 --messages="$MESSAGES" --reps="$REPS" --warmup="$WARMUP" \
                 --settling="$SETTLING" --sample-interval="$INTERVAL" \
                 --session="$session" --calibration-reps="$CALIB_REPS" \
                 --raw-out="$raw" --summary-out="$summary" \
                 --calibration-out="$calib" \
                 >/dev/null 2>"$OUT/stderr/${cell_id}_s${session}.txt"
    done
done

LOADAVG_END="$(read_loadavg_1min)"

echo
echo "==> All $EXPECTED_PROCESSES processes completed and validated"
echo "    load avg (1 min) at end: $LOADAVG_END"

# ---------------------------------------------------------------------------
# run_order.txt — the execution order parsed back OUT of command.txt, so the
# record and the order cannot disagree.
# ---------------------------------------------------------------------------
grep -E '^.*spsc_tail_latency_bench ' "$OUT/command.txt" \
    | sed 's/^[^ ]* //' > "$OUT/run_order.txt" || true

# ---------------------------------------------------------------------------
# SESSIONS.md — the session design, stated plainly
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 4 — session design"
    echo
    echo "| session | traversal | first cell | last cell | processes |"
    echo "|---|---|---|---|---|"
    for session in $(seq 1 "$SESSIONS"); do
        traversal="$(session_traversal "$session")"
        if [[ "$traversal" == "reverse" ]]; then
            first="64 B / 65536"; last="16 B / 1024"
        else
            first="16 B / 1024"; last="64 B / 65536"
        fi
        echo "| $session | $traversal | $first | $last | $CELLS_PER_SESSION |"
    done
    echo
    echo "Every cell appears once per session, so each is measured **4 times** as a"
    echo "process: twice early in a session and twice late, in two forward and two"
    echo "reverse passes."
    echo
    echo "**What the ordering buys:** it prevents a cell's temporal position inside a"
    echo "session from being confounded with the cell's identity. Every cell is"
    echo "measured both first and last across the four sessions."
    echo
    echo "**What it does NOT buy:** it does not eliminate scheduler variation, core"
    echo "migration, DVFS or thermal drift, and it does not make the sessions"
    echo "independent of each other. This is a single-host, non-pinned measurement."
    echo "No claim of a controlled thermal or frequency regime is made anywhere in"
    echo "this dataset."
    echo
    echo "**AB/BA is not applicable.** There is one queue in this matrix, so there is"
    echo "no treatment pair to counterbalance. The two traversal directions balance"
    echo "cell POSITION, and nothing more is claimed for them."
} >"$OUT/SESSIONS.md"

# ---------------------------------------------------------------------------
# HOST.md — host and toolchain metadata
# ---------------------------------------------------------------------------
{
    echo "# Experiment 02 Phase 4 — host and toolchain metadata"
    echo
    echo "Measured with \`$BIN_NAME\` built from repo HEAD"
    echo "\`$(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')\`."
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
    echo "| warnings | \`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror\` |"
    echo "| extra arch flags | \`${ARCH_FLAGS}\` (empty = compiler default) |"
    echo "| clock | \`std::chrono::steady_clock\` only; never \`system_clock\` |"
    echo "| load average (1 min) at start | \`$LOADAVG_START\` |"
    echo "| load average (1 min) at end | \`$LOADAVG_END\` |"
    echo "| pre-flight load limit | \`$MAX_LOADAVG\` (0 = check disabled) |"
    echo "| uptime at start | \`$UPTIME_START\` |"
    echo
    echo "Thread placement is NOT controlled: no affinity, no pinning and no thread"
    echo "priority is set anywhere in this experiment. macOS may migrate either"
    echo "thread mid-run, and the two threads may land on different core types. No"
    echo "measurement in this dataset observes which cores were used, so no result"
    echo "here may be attributed to a core type."
    echo
    echo "## Reported by the measured processes themselves"
    echo
    echo "\`\`\`"
    grep -h 'layout=' "$OUT"/stderr/*.txt 2>/dev/null | sed 's/.*layout=//' | sort -u || true
    echo "\`\`\`"
    echo
    echo "## Exact sources that produced this dataset"
    echo
    echo "A commit hash alone does not describe a working tree with uncommitted"
    echo "changes, so the digest of each source file is recorded here. The digest is"
    echo "of the file ON DISK at run time, which is what the binary was built from."
    echo
    echo '```'
    echo "git HEAD: $(git rev-parse HEAD 2>/dev/null || echo 'not a git repository')"
    echo
    echo "working tree at run time (git status --short):"
    git status --short 2>/dev/null || echo "  (not a git repository)"
    echo
    for f in benchmark/spsc_tail_latency_bench.cpp benchmark/spsc_tail_harness.h \
             tests/spsc_tail_latency_tests.cpp scripts/spsc-tail-latency.sh \
             scripts/verify-spsc-tail-summary.py include/spsc_remote_cursor_ring_buffer.h; do
        if [[ -f "$f" ]]; then
            printf 'sha256  %s  %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "$f"
        else
            printf 'sha256  %s  %s\n' "MISSING" "$f"
        fi
    done
    echo '```'
} >"$OUT/HOST.md"

# ---------------------------------------------------------------------------
# Verify: recompute every repetition's statistics from its raw samples
# ---------------------------------------------------------------------------
echo
echo "==> Verifying every raw repetition against its summary"
#
# The report is SAVED as invariants.txt, not merely printed. Two things depend
# on the artifact rather than on the console: the analyzer refuses to derive any
# table unless invariants.txt records a passing verification, and a reader who
# wants to know what was checked needs the record next to the data. `PIPESTATUS`
# is read instead of `$?` so a failed verification still fails the run — a pipe
# to `tee` would otherwise mask it.
if command -v python3 >/dev/null 2>&1; then
    python3 scripts/verify-spsc-tail-summary.py "$OUT" 2>&1 \
        | tee "$OUT/invariants.txt"
    VERIFY_RC="${PIPESTATUS[0]}"
    if [[ "$VERIFY_RC" != "0" ]]; then
        echo "FATAL: the raw->summary verification FAILED (exit $VERIFY_RC)." >&2
        echo "       The dataset is NOT verified and must not be cited." >&2
        exit 1
    fi
else
    echo "FATAL: python3 not found; the raw->summary verification could not run." >&2
    echo "       The dataset is NOT verified and must not be cited." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Derive the cross-repetition tables the docs cite.
#
# Run HERE, immediately after the verification gate, so a dataset and its
# derived tables are never produced by different steps and can never drift
# apart. The analyzer refuses to run without a passing invariants.txt, so this
# cannot derive a table from unverified data.
# ---------------------------------------------------------------------------
echo
echo "==> Deriving the cross-repetition tables (cell-session-blocked is PRIMARY)"
if command -v python3 >/dev/null 2>&1; then
    python3 scripts/analyze-spsc-tail.py "$OUT" || {
        echo "FATAL: the derived tables could not be produced." >&2
        exit 1
    }
else
    echo "FATAL: python3 not found; the derived tables could not be produced." >&2
    exit 1
fi

echo
echo "==> Phase 4 dataset complete: $OUT"
echo "    processes                : $EXPECTED_PROCESSES"
echo "    measured repetition dists: $EXPECTED_MEASURED_REPETITIONS"
echo "    sampled latencies        : $(( EXPECTED_MEASURED_REPETITIONS * EXPECTED_SAMPLES ))"
echo "    clock reads per thread   : $(( EXPECTED_MEASURED_REPETITIONS * EXPECTED_SAMPLES ))"
echo "                               (sparse: NOT $(( EXPECTED_MEASURED_REPETITIONS * MESSAGES )))"
echo "    verification record      : $OUT/invariants.txt"
echo "    PRIMARY cell summary     : $OUT/CELL_SESSION_BLOCKED.csv"
