#!/usr/bin/env bash
# Phase 3M (macOS / Apple Silicon) Instruments automation via xctrace.
#
# xctrace is the CLI for Instruments and ships only with a FULL Xcode install.
# This host is Command-Line-Tools only, so on THIS machine the script stops
# with a clear instruction instead of guessing at template names. On a machine
# with Xcode installed it records the benchmark cell under the requested
# template into a .trace bundle and prints how to open it.
#
# Usage:
#   scripts/phase3m-instruments.sh [--template NAME] <impl> <workload> <scale> [updates=N] [reps=N] [--signposts] [--dry-run]
#     --template NAME  Instruments template name (default: 'Time Profiler').
#                      Templates are DETECTED, not assumed — see below.
#     impl         map | flat
#     workload     A | B | C | D | E
#     scale        1000 | 10000 | 100000 | 1000000
#     updates=N    steady ops per timed block     (default 2000000)
#     reps=N       timed blocks per cell          (default 1 — see MACOS_INSTRUMENTS.md)
#     --signposts  set LLOB_SIGNPOSTS=1 so the 'llob.apply.block' interval
#                  appears in the recording (see order_book_bench.cpp)
#     --dry-run    print the exact commands without running xctrace (default off)
#
# Template detection: xctrace requires the FULL template NAME. Rather than
# depend on undocumented abbreviations, this script asks xctrace to LIST the
# installed templates and matches the requested name exactly (Time Profiler,
# CPU Counters, ...). If the template is not found it fails with the list of
# available templates and a clear next step — it never invents a template name.
#
# xctrace writes a .trace bundle to the given output dir; it is opaque (not
# text). Open it with Instruments, or `xctrace export` to pull data out.

set -euo pipefail

cd "$(dirname "$0")/.."          # project root

# ---- defaults ---------------------------------------------------------------
TEMPLATE="Time Profiler"
SIGNPOSTS=0
DRYRUN=0
IMPL=""; WORKLOAD=""; SCALE=""
UPDATES=2000000
REPS=1

for a in "$@"; do
    case "$a" in
        --template=*)  TEMPLATE="${a#--template=}" ;;
        updates=*)     UPDATES="${a#updates=}" ;;
        reps=*)        REPS="${a#reps=}" ;;
        --signposts)   SIGNPOSTS=1 ;;
        --dry-run)     DRYRUN=1 ;;
        -h|--help)
            sed -n '1,45p' "$0" | grep -E '^#( |$)' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        --template)    echo "use --template=NAME (with '=')" >&2; exit 2 ;;
        *)  # positional impl workload scale
            if [[ -z "$IMPL" ]]; then IMPL="$a";
            elif [[ -z "$WORKLOAD" ]]; then WORKLOAD="$a";
            elif [[ -z "$SCALE" ]]; then SCALE="$a";
            else echo "unexpected argument: $a" >&2; exit 2; fi ;;
    esac
done

if [[ -z "$IMPL" || -z "$WORKLOAD" || -z "$SCALE" ]]; then
    echo "usage: scripts/phase3m-instruments.sh [--template=NAME] <impl> <workload> <scale> [updates=N] [reps=N] [--signposts] [--dry-run]" >&2
    exit 2
fi
case "$IMPL" in map|flat) ;; *) echo "impl must be map|flat" >&2; exit 2 ;; esac
case "$WORKLOAD" in A|B|C|D|E) ;; *) echo "workload must be A|B|C|D|E" >&2; exit 2 ;; esac

# ---- gate: xctrace requires full Xcode --------------------------------------
if ! command -v xcrun >/dev/null 2>&1 || ! xcrun --find xctrace >/dev/null 2>&1; then
    echo "error: xctrace is not available on this host." >&2
    echo "  xctrace ships with FULL Xcode (Instruments). This host is Command Line" >&2
    echo "  Tools only, so it cannot record Instruments traces." >&2
    echo "  Install Xcode from the Mac App Store (or point xcode-select at an Xcode" >&2
    echo "  install), then re-run. The tooling itself (scripts + docs) is ready;" >&2
    echo "  a real trace belongs under docs/results/phase3-macos-apple-silicon/." >&2
    exit 3
fi

# ---- detect the requested template (never assume) ---------------------------
avail="$(xcrun xctrace list templates 2>/dev/null || true)"
if ! grep -Fqi -- "$TEMPLATE" <<<"$avail"; then
    echo "error: Instruments template '$TEMPLATE' not found." >&2
    echo "  Available templates on this host:" >&2
    echo "$avail" | sed 's/^/    /' >&2
    echo "  Pick an exact name above (e.g. --template='Time Profiler')." >&2
    exit 3
fi

# ---- build (fresh dir, Release, no stale arch flags) ------------------------
if [[ "$DRYRUN" -eq 1 ]]; then
    echo "==> DRY-RUN: no build, no recording. Exact commands:"
    echo "  rm -rf build-perf && cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS= && cmake --build build-perf"
else
    echo "==> Configuring (Release, fresh build-perf dir)"
    rm -rf build-perf
    cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS= >/dev/null
    cmake --build build-perf >/dev/null
fi

BIN=./build-perf/orderbook_bench
mkdir -p results
OUT="results/macos_${IMPL}_${WORKLOAD}_${SCALE}_$(date +%Y%m%d-%H%M%S).trace"
ENVV=()
[[ "$SIGNPOSTS" -eq 1 ]] && ENVV=(env LLOB_SIGNPOSTS=1)

echo "==> recording: template='$TEMPLATE'  cell=$IMPL $WORKLOAD $SCALE  updates=$UPDATES reps=$REPS  signposts=$SIGNPOSTS"
if [[ "$DRYRUN" -eq 1 ]]; then
    echo "  ${ENVV[@]+"${ENVV[@]}"} xcrun xctrace record --template '$TEMPLATE' --output '$OUT' \\"
    echo "      --launch -- $BIN $IMPL $WORKLOAD $SCALE updates=$UPDATES reps=$REPS"
    echo "  open '$OUT'"
    echo "Run without --dry-run to actually record (full Xcode required)."
    exit 0
fi

# Record. xctrace --launch runs the target under the template until it exits.
# LLOB_SIGNPOSTS is exported so the bench emits its interval for the trace.
${ENVV[@]+"${ENVV[@]}"} \
    xcrun xctrace record \
        --template "$TEMPLATE" \
        --output "$OUT" \
        --launch -- "$BIN" "$IMPL" "$WORKLOAD" "$SCALE" \
        "updates=$UPDATES" "reps=$REPS"
echo "==> recorded: $OUT"
echo "    open with: open '$OUT'"
echo "    In Instruments, select the 'llob.apply.block' signpost interval (Time Profiler"
echo "    > the process > the interval) to scope the trace to the timed apply() block."
echo "    Then commit the inspected result under docs/results/phase3-macos-apple-silicon/."
