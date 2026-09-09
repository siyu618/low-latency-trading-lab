# Phase 4 — macOS tail-latency results

**Status: populated — six real Phase 4 canonical tail-latency datasets,
committed and reviewed.**

This tree is the committed home for Phase 4 (tail latency / jitter) measurements
made with `orderbook_tail_bench` on the Apple Silicon macOS host. It holds six
real canonical runs from the **same Apple M3 Max / macOS 14.2.1 machine** that
produced the Phase 2 canonical dataset (`../phase2-m3max/`) and the Phase 3M
traces (`../phase3-macos-apple-silicon/`), so those are the same-host baselines
these distributions are read against. Every number is measured by the benchmark;
nothing is invented.

## The dataset

Six cells, each a full deterministic distribution run at the spec's canonical
defaults — `--updates 10000000 --batch-size 512`, default Phase 2 seed, one
process per book — produced 2026-09-09 (UTC+8):

| Cell | mean | p50 | p99 | p99.9 | max | max/p50 |
|---|---|---|---|---|---|---|
| `map_A_1000` | 34.22 | 33.45 | 55.66 | 132.24 | 511.88 | 15.3× |
| `map_A_1000000` | 285.48 | 267.58 | 496.91 | 637.04 | 1164.79 | 4.4× |
| `map_C_1000000` | 77.04 | 75.60 | 105.14 | 213.54 | 273.84 | 3.6× |
| `map_E_1000000` | 615.33 | 550.13 | 1281.98 | 1627.85 | 5077.07 | 9.2× |
| `flat_A_1000000` | 5.13 | 5.04 | 11.96 | 28.48 | 284.59 | 56.4× |
| `flat_C_1000000` | 6.23 | 6.10 | 9.20 | 22.05 | 59.90 | 9.8× |

All values are **batch-normalized ns/update** (full 512-update batches,
nearest-rank percentile). Each cell produced 19,531 full batches for the
distribution + one recorded-and-excluded trailing 128-update partial batch
(`total_samples=19532`, `distribution_samples=19531`). The full analysis is in
`PHASE4_ANALYSIS.md`.

## What is in each cell directory

- `summary.txt` — the benchmark's key:value summary (MEASURED distribution +
  DERIVED percentiles/jitter ratios + final-state validation).
- `raw_samples.csv` — the raw batch samples (source of truth), one row per batch.
- `command.txt` — the exact reproduction command.
- `host.txt` — machine/tool/build metadata at run time.
- `notes.md` — per-cell interpretation (labeled INTERPRETATION) and LIMITATION.

## Provenance / honesty

- `host.txt` per cell records commit `34c0f17` with `tree: DIRTY` — the only
  dirty change at run time was the `scripts/tail-bench.sh` exec-bit fix
  (100644→100755), which is committed together with this dataset; no benchmark
  or binary semantics changed between the run and the commit.
- Final-state validation passed in every cell (`final_synced=1`; `final_seq` =
  `2N + updates`). Ending level counts < 2N for workloads C and E are the
  workloads' expected level drift, not validation failures (see per-cell notes).
- Every claim in `notes.md` / `PHASE4_ANALYSIS.md` is labeled
  `MEASURED` / `DERIVED` / `INTERPRETATION` / `LIMITATION`.
