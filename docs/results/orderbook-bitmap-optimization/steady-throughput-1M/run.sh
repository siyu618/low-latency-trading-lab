#!/usr/bin/env bash
# Canonical steady-state comparison, one impl per process (Phase-2 convention),
# alternating flat/bits ACROSS rounds so process-level turbo/frequency drift is
# sampled rather than systematized. Per (impl,wl) the MEDIAN over rounds of the
# per-process best_ns_per_update is the reported number (best-of-reps inside
# each process, median-of-rounds across processes).
#
# Run from the repo root:  bash docs/results/.../run.sh
#
# EFFECTIVE timed-block update count at scale 1M (what the timed apply loop and
# the rows below actually replay):
#   A = 2000000   B = 500000   C = 2000000   D = 2000000   E = 2000000
# A and C stay at 2M so they reproduce the frozen Phase-2 reference
# flat_A_1M=4.327 / flat_C_1M=5.912 (harness cross-check). B uses 500000 updates
# ONLY to bound wall time: workload B's off-clock STREAM GENERATION degenerates
# at near-full density (rnd_absent retries 4096 draws per add that almost never
# finds an absent level), ~31 s per 2M-update process, independent of the
# implementation and untimed. The timed apply loop is unaffected; B's
# flat-vs-bits delta is still measured on identical streams at identical
# updates. (The standalone single-sample flat.csv / bits.csv files are separate
# 2M-update reference reproductions even for B; see command.txt.)
set -euo pipefail
R="$(cd "$(dirname "$0")" && pwd)"
cd "$R/../../../.." || exit 2
mkdir -p "$R/raw"
WL=(A B C D E)
ROUNDS=5

for wl in "${WL[@]}"; do
  # updates per workload at scale 1M (bash-3.2 portable; no associative arrays).
  updates=2000000
  if [ "$wl" = "B" ]; then updates=500000; fi
  : > "$R/raw/flat_${wl}.log"
  : > "$R/raw/bits_${wl}.log"
  for ((r=1; r<=ROUNDS; r++)); do
    # alternate which impl goes first each round
    if [ $((r % 2)) -eq 1 ]; then ORD=(flat bits); else ORD=(bits flat); fi
    for impl in "${ORD[@]}"; do
      ./build/orderbook_bitmap_bench "$impl" "$wl" 1000000 \
          updates="$updates" reps=3 \
        2>/dev/null | grep -E "^${impl},${wl}," >> "$R/raw/${impl}_${wl}.log"
    done
  done
done
echo "# medians (ns/update, best-of-reps inside process, median over ${ROUNDS} rounds)"
printf "wl,flat_median_ns,bits_median_ns,delta_ns,delta_pct\n"
for wl in "${WL[@]}"; do
  fm=$(sort -t, -k6,6g "$R/raw/flat_${wl}.log" | awk -F, 'NR==3{print $6}')
  bm=$(sort -t, -k6,6g "$R/raw/bits_${wl}.log" | awk -F, 'NR==3{print $6}')
  d=$(awk -v f="$fm" -v b="$bm" 'BEGIN{printf "%.3f", b-f}')
  p=$(awk -v f="$fm" -v b="$bm" 'BEGIN{if(f>0) printf "%.1f", 100*(b-f)/f; else print "nan"}')
  printf "%s,%.3f,%.3f,%s,%s\n" "$wl" "$fm" "$bm" "$d" "$p"
done
