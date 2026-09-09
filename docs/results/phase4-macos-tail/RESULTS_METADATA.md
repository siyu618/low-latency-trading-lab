# Experiment 01, Phase 4 — canonical tail-latency dataset (Apple M3 Max)

This directory holds the **one canonical Phase 4 result dataset** for the
fixed-batch tail-latency / jitter benchmark, measured on the Apple M3 Max dev
machine (`phase4-macos-tail`). It was produced with the hardened Phase 4.1
tooling: **one benchmark invocation per cell**, a trailing partial batch
recorded but excluded from every distribution metric, and each cell's
`summary.txt` independently re-computed from that cell's own `raw_samples.csv`
by `scripts/verify-tail-summary.sh` (all 13 metrics, 0 failures — see
`verify-all.log`). No numbers were invented or adjusted after measurement.

The earlier, pre-Phase-4.1 cells from buggy tooling live under the sibling
`docs/results/phase4-macos-tail-pre4.1-invalid/` (two independent runs per cell;
trailing partial batch leaked into the summary). They are INVALID and are
retained only as a labeled historical artifact. This directory is the canonical
replacement and is what the README and `PHASE4_ANALYSIS.md` cite.

## Layout

```
phase4-macos-tail/
├── README.md            # dataset overview + per-cell verification
├── RESULTS_METADATA.md  # this file: host, toolchain, run parameters
├── PHASE4_ANALYSIS.md   # analysis written from THIS dataset only
├── calibration.txt      # timer/harness calibration (1,000,000 samples each group)
├── verify-all.log       # raw<->summary verification output, all six cells (PASS)
└── <cell>/              # one directory per measured cell
    ├── command.txt      # effective benchmark invocation (bash %q serialization)
    ├── host.txt         # host/chip/toolchain/git-state metadata at measurement time
    ├── summary.txt      # per-run summary (distribution + derived ratios)
    └── raw_samples.csv  # per-batch raw durations (full + trailing partial row)
```

## Cells

Each cell is one process / one stream / one distribution:
`map_A_1000`, `map_A_1000000`, `map_C_1000000`, `map_E_1000000`,
`flat_A_1000000`, `flat_C_1000000`.

Naming: `<impl>_<workload>_<levels>` where `levels` is the configured scale
`N` (starting live levels per side, domain `[1, 2N]` ticks).

| Cell | Impl | Workload | N (levels/side) |
|------|------|----------|-----------------|
| map_A_1000 | std::map | A update-only | 1,000 |
| map_A_1000000 | std::map | A update-only | 1,000,000 |
| map_C_1000000 | std::map | C frequent best deletion | 1,000,000 |
| map_E_1000000 | std::map | E uniformly random | 1,000,000 |
| flat_A_1000000 | flat | A update-only | 1,000,000 |
| flat_C_1000000 | flat | C frequent best deletion | 1,000,000 |

## Environment (measured 2026-09-09)

| | |
|---|---|
| Machine | Apple MacBook Pro, model identifier `Mac15,10` |
| CPU | Apple M3 Max, `arm64` |
| OS | macOS 14.2.1 (Build 23C71) |
| Compiler | Apple clang 15.0.0 (`clang-1500.3.9.4`), target `arm64-apple-darwin23.2.0` |
| Build | CMake; benchmark always compiled `-O3 -DNDEBUG` |
| Arch flags | **none** — compiler default (no `-march=native` / `-mcpu=…`) |
| Standard | C++20 |
| Git commit | `a0cd5db2f036a404be2ef7d1dafb3f5496053d03` |
| Git tree | clean at measurement time (per-cell `host.txt`) |

## Run parameters (all cells identical)

| | |
|---|---|
| Update count | 10,000,000 steady `apply()` updates |
| Batch size | 512 updates per timed batch |
| Seed | Phase 2 default → resolved seed `407715774446` (also in each summary) |
| Percentile def | nearest-rank over **full-batch** durations: rank = ceil(q·N_full), observed sample at that 1-based rank (see `percentile_definition` in each summary) |
| Units | **batch-normalized ns/update** = batch wall duration / 512 |

Batch shape: 10,000,000 / 512 = 19,531 full batches + a trailing partial batch
of 128 updates. The partial batch is recorded in `raw_samples.csv`
(`batch_operations=128`) and is **excluded** from every distribution metric;
each summary reports `total_samples=19532`, `distribution_samples=19531`,
`partial_batch_present=1`, `partial_batch_ops=128`.

## Calibration

`calibration.txt` at the dataset root was recorded against the same clean
build/commit (1,000,000 samples per group). Two groups are reported **separately
and are never auto-subtracted** from the latency samples:

- `clock_pair_*` — two `steady_clock` reads separated by a compiler barrier:
  the pure timer floor of one clock-read pair.
- `empty_batch_harness_*` — clock-read, 512 empty iterations with a barrier +
  sink, clock-read: the whole batch timing skeleton with **no book work**.

Calibration medians (median / p99 / max, ns):

| Group | median | p99 | max |
|---|---|---|---|
| clock_pair | 0.000 | 42.000 | 8,375.000 |
| empty_batch_harness | 125.000 | 208.000 | 16,875.000 |

The empty-harness median (125 ns over a 512-update batch ≈ 0.24 ns/update
normalized) is negligible against every cell's p50 (≥ 3.9 ns/update), so no
correction is applied. The two groups are not subtracted from any reported
number.

## Honesty rule

Every number in this tree comes from the six runs recorded here, re-verified
summary-from-raw (`verify-all.log`), plus the calibration runs above. Nothing
was edited post-hoc. Occupancy drift between workloads (`final_level_count`
differing across cells — e.g. map/flat C end at 1,999,983 and map E at
1,994,440) is an expected property of the generated streams, not an error: each
cell reports its own `final_synced=1` and `final_seq == final_seq_expected`.
