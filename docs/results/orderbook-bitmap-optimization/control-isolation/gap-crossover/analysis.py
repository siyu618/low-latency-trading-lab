#!/usr/bin/env python3
"""Merge multi-round gap-crossover sweeps into per-gap medians + sign counts.

Each round file is one full sweep of a deterministic ladder (--impl=both,
blocks=128, K=4096 best-deletes) that prints one CSV row per (impl, gap):

  impl,gap_ticks,ns_per_delete_best,ns_per_delete_mean,p50,p99,max,blocks

p50 (column 5) is the median over the 128 block samples inside that round.

For every gap we collect the p50 each round reported for flat and for bits and
emit:
  * median-over-rounds p50 for each impl,
  * bits - flat delta (ns and % of flat),
  * sign consistency: in how many of the N rounds bits_p50 < flat_p50 at this
    exact gap — a crossover claim must survive across independent rounds.
The crossover gap is the smallest g whose MEDIAN p50 has bits < flat, alongside
the observed distribution of the per-round crossover gap (min/median/max).
"""
import glob
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))


def parse_round(fn):
    """Return {gap: {'flat': p50, 'bits': p50}} from one sweep file."""
    per = {}
    with open(fn) as fh:
        for line in fh:
            m = re.match(r"^(flat|bits),(\d+),", line)
            if not m:
                continue
            impl, gap = m.group(1), int(m.group(2))
            fields = line.rstrip("\n").split(",")
            per.setdefault(gap, {})[impl] = float(fields[4])  # p50 column
    return per


def analyze(name, dir_path):
    files = sorted(glob.glob(os.path.join(dir_path, "*.log")))
    if not files:
        print(f"(no round files under {dir_path})")
        return
    # direction label (fwd/rev) from filenames like fwd-1.log
    by_dir = {}
    for fn in files:
        by_dir.setdefault(os.path.basename(fn).rsplit("-", 1)[0], []).append(fn)

    datas = [parse_round(fn) for fn in files]  # parse each round once
    gaps = set()
    samples = {}  # (impl, gap) -> [p50 across every round]
    round_cross = []  # per-round crossover gap (smallest g with bits < flat)
    for data in datas:
        for gap, d in data.items():
            gaps.add(gap)
            for impl in ("flat", "bits"):
                samples.setdefault((impl, gap), []).append(d[impl])
        cand = [g for g in sorted(data) if data[g]["bits"] < data[g]["flat"]]
        round_cross.append(min(cand) if cand else None)

    total_rounds = len(datas)
    n_by_dir = {k: len(v) for k, v in by_dir.items()}
    print(f"\n===== {name}: {total_rounds} independent rounds "
          f"({', '.join(f'{k}={v}' for k, v in n_by_dir.items())}) =====")
    print("gap,flat_p50_med,bits_p50_med,bits_minus_flat_ns,bits_minus_flat_pct,"
          "bits_wins_rounds")
    crossover_rows = []
    for gap in sorted(gaps):
        f = statistics.median(samples[("flat", gap)])
        b = statistics.median(samples[("bits", gap)])
        # direct sign count over the rounds that contain this gap
        n_win = sum(1 for data in datas
                    if gap in data and data[gap]["bits"] < data[gap]["flat"])
        print(f"{gap},{f:.3f},{b:.3f},{b - f:.3f},{(b - f) / f * 100.0:.1f},"
              f"{n_win}")
        if b < f:
            crossover_rows.append(gap)
    print(f"median-p50 crossover gap (smallest g with bits<flat): "
          f"{min(crossover_rows) if crossover_rows else 'none'}")
    if round_cross:
        cross = sorted(c for c in round_cross if c is not None)
        if cross:
            print(f"per-round crossover gap distribution: n={len(cross)} "
                  f"min={min(cross)} median={statistics.median(cross)} "
                  f"max={max(cross)}")


def main():
    analyze("var-domain (default ladder: domain grows with g)",
            os.path.join(ROOT, "var-domain"))
    analyze("fixed-domain (domain pinned to max-gap size)",
            os.path.join(ROOT, "fixed-domain"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
