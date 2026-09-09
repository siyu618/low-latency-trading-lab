#!/usr/bin/env bash
# Three-way drift-free in-process DELTA (flat / tuned / bits in ONE process,
# rotating start order per block) for the control-isolation study.
#
# Same block convention as the legacy two-way inproc.csv: 24 blocks per cell for
# A/C/D/E at 2M updates, 32 for B at 500k. impl=all selects flat+tuned+bits so
# the control-flow (flat vs tuned) and bitmap (tuned vs bits) deltas share one
# clock state with the end-to-end flat-vs-bits delta.
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$R" rev-parse --show-toplevel)"
cd "$ROOT"
: > "$R/inproc3.csv"
for wl in A B C D E; do
  updates=2000000; blocks=24
  if [ "$wl" = "B" ]; then updates=500000; blocks=32; fi
  echo "# ---- workload $wl (updates=${updates}, inproc3 blocks=${blocks}) ----" >> "$R/inproc3.csv"
  ./build/orderbook_bitmap_bench all "$wl" 1000000 updates="$updates" --inproc="$blocks" >> "$R/inproc3.csv" 2>/dev/null
done
echo "inproc3.csv written to $R/inproc3.csv"
