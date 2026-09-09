#!/usr/bin/env bash
# Control-isolation steady-state comparison (Experiment 01 credibility pass).
# THREE implementations, ONE impl per process (Phase-2 convention), rotating
# which impl goes first across rounds so process-level turbo/frequency drift is
# sampled rather than systematized. Per (impl,wl) the MEDIAN over rounds of the
# per-process best_ns_per_update is the reported number (best-of-reps inside
# each process, median-of-rounds across processes).
#
# Isolation framing (see docs/ORDERBOOK_BITMAP_OPTIMIZATION.md):
#   flat  vs tuned  -> transition-aware CONTROL FLOW alone (no bitmap anywhere;
#                       TransitionAwareFlatOrderBook = FlatOrderBook + that one
#                       restructured positive path)
#   tuned vs bits   -> occupancy BITMAP alone (same contiguous qty rep, same
#                      linear best re-scan on delete, same transition-aware
#                      positive path)
#   flat  vs bits   -> END-TO-END candidate delta (control flow + bitmap);
#                      MUST NOT be read as "the bitmap".
#
# EFFECTIVE timed-block update count at scale 1M:
#   A = 2000000   B = 500000   C = 2000000   D = 2000000   E = 2000000
# (B at 500k bounds wall time only: B's off-clock stream generation degenerates
# at near-full density. Same rationale as the frozen Phase-2/steady runs.)
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$R" rev-parse --show-toplevel)"
cd "$ROOT"
mkdir -p "$R/raw"
WL=(A B C D E)
ROUNDS=5

# median of the numeric values passed on stdin (one per line)
median() {
  sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}

for wl in "${WL[@]}"; do
  updates=2000000
  if [ "$wl" = "B" ]; then updates=500000; fi
  : > "$R/raw/flat_${wl}.log"
  : > "$R/raw/tuned_${wl}.log"
  : > "$R/raw/bits_${wl}.log"
  for ((r=1; r<=ROUNDS; r++)); do
    # rotate which impl goes first: r1 flat,tuned,bits / r2 tuned,bits,flat / r3 bits,flat,tuned / ...
    case $(( (r-1) % 3 )) in
      0) ORD=(flat tuned bits) ;;
      1) ORD=(tuned bits flat) ;;
      2) ORD=(bits flat tuned) ;;
    esac
    for impl in "${ORD[@]}"; do
      ./build/orderbook_bitmap_bench "$impl" "$wl" 1000000 \
          updates="$updates" reps=3 \
        2>/dev/null | grep -E "^${impl},${wl}," >> "$R/raw/${impl}_${wl}.log"
    done
  done
done

# Build medians.csv, then echo it for the run log.
: > "$R/medians.csv"
{
  echo "# medians of per-process best_ns_per_update over ${ROUNDS} rounds, N=1000000; 3 impls"
  echo "# flat -> tuned = transition-aware control flow alone (no bitmap);"
  echo "# tuned -> bits = occupancy bitmap alone; flat -> bits = end-to-end (NOT the bitmap alone)"
  echo "wl,updates,flat_ns,tuned_ns,bits_ns,flat_to_tuned_pct,tuned_to_bits_pct,flat_to_bits_pct"
  for wl in "${WL[@]}"; do
    updates=2000000
    if [ "$wl" = "B" ]; then updates=500000; fi
    f=$(awk -F, '$6>0{print $6}' "$R/raw/flat_${wl}.log" | median)
    t=$(awk -F, '$6>0{print $6}' "$R/raw/tuned_${wl}.log" | median)
    b=$(awk -F, '$6>0{print $6}' "$R/raw/bits_${wl}.log" | median)
    ft=$(awk -v f="$f" -v t="$t" 'BEGIN{if(f>0)printf "%.1f",100*(t-f)/f; else print "nan"}')
    tb=$(awk -v t="$t" -v b="$b" 'BEGIN{if(t>0)printf "%.1f",100*(b-t)/t; else print "nan"}')
    fb=$(awk -v f="$f" -v b="$b" 'BEGIN{if(f>0)printf "%.1f",100*(b-f)/f; else print "nan"}')
    printf "%s,%s,%.3f,%.3f,%.3f,%s,%s,%s\n" "$wl" "$updates" "$f" "$t" "$b" "$ft" "$tb" "$fb" >> "$R/medians.csv"
  done
} > /dev/null
echo "# control-isolation steady @1M: medians of per-process best_ns_per_update"
echo "# over ${ROUNDS} rounds (best-of-3 reps inside each process); 3 impls"
echo "# flat -> tuned = control flow alone; tuned -> bits = bitmap alone;"
echo "# flat -> bits = end-to-end (MUST NOT be read as the bitmap)"
cat "$R/medians.csv"
echo "# medians.csv written to $R/medians.csv"
