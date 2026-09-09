#!/usr/bin/env bash
# Multi-round gap-crossover hardening (Experiment 01 credibility pass).
#
# Research target: pin the gap at which BitsetFlatOrderBook's hierarchical
# occupancy lookup becomes cheaper than FlatOrderBook's adjacent linear best
# re-scan — and state it only if repeated independent rounds agree.
#
# Rounds are independent runs of the deterministic ladder sweep (--impl=both
# interleaved in one process, blocks=128, K=4096 deletes) over the SAME gap set,
# but the ORDER gaps are swept alternates FORWARD (1,2,4,...,1024) and REVERSE
# (1024,...,1) to expose any order/drift bias. 5 rounds each direction.
#
# A separate validation (fixed-domain/) re-runs the sweep with --fixed-domain:
# every gap is timed on the domain the largest gap requires, so the next-best
# distance (g) is decoupled from the active memory span that the default ladder
# couples to g. 3 rounds each direction (validation only).
#
# analysis.py merges the per-round files into, per gap: the median-over-rounds
# p50 for flat and bits, the delta, and how often the delta sign agrees across
# rounds — the sign-consistency a crossover claim needs.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$R" rev-parse --show-toplevel)"
cd "$ROOT"
BIN=./build/orderbook_gap_bench
FWD="1,2,4,8,16,32,64,128,256,512,1024"
REV="1024,512,256,128,64,32,16,8,4,2,1"
mkdir -p "$R/var-domain" "$R/fixed-domain"

# provenance header
{
  echo "# gap-crossover hardening runs ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
  echo "# host: $(uname -m) $(sw_vers -productVersion 2>/dev/null)"
  echo "# git: $(git -C "$ROOT" rev-parse HEAD) $(git -C "$ROOT" status --porcelain | wc -l | tr -d ' ') dirty-file(s)"
  echo "# effective: --impl=both --blocks=128 --deletes=4096 (K=4096 ask levels, floor survives)"
} > "$R/command.txt"

echo "gap-crossover: variable-domain forward rounds (5)"
for r in 1 2 3 4 5; do
  $BIN --impl=both --blocks=128 --deletes=4096 --gaps="$FWD" > "$R/var-domain/fwd-$r.log" 2>&1
  echo "  fwd round $r/5 done"
done
echo "gap-crossover: variable-domain reverse rounds (5)"
for r in 1 2 3 4 5; do
  $BIN --impl=both --blocks=128 --deletes=4096 --gaps="$REV" > "$R/var-domain/rev-$r.log" 2>&1
  echo "  rev round $r/5 done"
done
echo "gap-crossover: fixed-domain forward rounds (3)"
for r in 1 2 3; do
  $BIN --impl=both --blocks=128 --deletes=4096 --gaps="$FWD" --fixed-domain > "$R/fixed-domain/fwd-$r.log" 2>&1
  echo "  fixed fwd round $r/3 done"
done
echo "gap-crossover: fixed-domain reverse rounds (3)"
for r in 1 2 3; do
  $BIN --impl=both --blocks=128 --deletes=4096 --gaps="$REV" --fixed-domain > "$R/fixed-domain/rev-$r.log" 2>&1
  echo "  fixed rev round $r/3 done"
done
echo "gap-crossover: all rounds complete"
