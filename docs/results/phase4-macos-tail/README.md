# Phase 4 tail-latency dataset — canonical (Apple M3 Max)

Canonical Phase 4 tail-latency / jitter dataset for Experiment 01, measured on
the Apple M3 Max dev machine with the **hardened Phase 4.1 tooling**: one
invocation per cell, trailing partial batch excluded from all distribution
metrics, and each summary re-verified from its own raw samples. See
[`RESULTS_METADATA.md`](RESULTS_METADATA.md) for the environment and run
parameters, and [`PHASE4_ANALYSIS.md`](PHASE4_ANALYSIS.md) for the analysis
written from this dataset.

> This directory supersedes `../phase4-macos-tail-pre4.1-invalid/`, which holds
> the INVALID pre-Phase-4.1 cells (retained only as a labeled historical
> artifact). Nothing in this canonical tree comes from that older tooling.

## What the numbers are

Each cell is one `orderbook_tail_bench` process replaying one deterministic
10,000,000-update stream in **512-update batches**. The reported metric is the
**batch-normalized ns/update** — a batch's wall duration divided by 512 — so a
value like `3.906` means one 512-update batch took ~2.00 µs. Percentiles are
nearest-rank over the 19,531 **full** batches; the trailing partial batch (128
ops) is recorded but excluded from every distribution metric.

All numbers below are batch-normalized ns/update. Mean / p50 / p90 / p99 /
p99.9 / max, plus derived ratios p99/p50, p99.9/p50, max/p50 from the same raw
basis. Updates = 10,000,000; batch = 512; seed = 407715774446.

| Cell | mean | p50 | p90 | p99 | p99.9 | max | p99/p50 | p99.9/p50 | max/p50 |
|------|-----:|----:|----:|----:|------:|----:|--------:|----------:|--------:|
| map_A_1000 | 34.963 | 34.424 | 36.297 | 44.922 | 126.709 | 219.564 | 1.30 | 3.68 | 6.38 |
| map_A_1000000 | 187.093 | 177.572 | 214.520 | 328.207 | 456.299 | 644.367 | 1.85 | 2.57 | 3.63 |
| map_C_1000000 | 74.310 | 73.730 | 78.043 | 91.146 | 106.527 | 255.371 | 1.24 | 1.44 | 3.46 |
| map_E_1000000 | 484.410 | 467.367 | 632.080 | 897.543 | 1330.566 | 3016.113 | 1.92 | 2.85 | 6.45 |
| flat_A_1000000 | 3.912 | 3.906 | 4.068 | 4.314 | 6.021 | 16.275 | 1.10 | 1.54 | 4.17 |
| flat_C_1000000 | 6.106 | 6.023 | 6.268 | 7.730 | 18.066 | 70.230 | 1.28 | 3.00 | 11.66 |

Each cell directory holds the full-resolution numbers: `summary.txt`
(`%.6f`), `raw_samples.csv` (per-batch durations, ~450–525 KB), `command.txt`
and `host.txt`.

## Verification

Every cell's `summary.txt` was recomputed **from that cell's own
`raw_samples.csv`** by `scripts/verify-tail-summary.sh` — 13 checks each
(total / distribution / partial shape / mean / min / p50 / p90 / p99 / p99.9 /
max / p99:p50 / p99.9:p50 / max:p50). Full output in
[`verify-all.log`](verify-all.log): **all six cells PASS, 0 failures**.

Per-cell integrity (all six identical except where noted):

- `total_samples=19532`, `distribution_samples=19531` (19,531 full batches),
  `partial_batch_present=1`, `partial_batch_ops=128`
- `final_synced=1` and `final_seq == final_seq_expected` (10002000 for
  map_A_1000; 12000000 for the five 1M-level cells)
- `final_level_count`: 2000 (map_A_1000), 2000000 (map/flat A 1M), 1999983
  (map/flat C 1M), 1994440 (map E 1M). The differences across workloads are the
  expected occupancy drift of each generated stream (C conserves with a small
  transient deficit; E is non-conserving), **not** an error.

Calibration (clock-pair and empty-batch-harness groups, reported separately,
never auto-subtracted): see `calibration.txt` and `RESULTS_METADATA.md`.
