# orderbook-bitmap-optimization — Experiment 01 Optimization Study dataset

**Status: COMPLETE.** Real, committed measurements backing the analysis in
[`docs/ORDERBOOK_BITMAP_OPTIMIZATION.md`](../../ORDERBOOK_BITMAP_OPTIMIZATION.md).
All rows in that document come from the raw files in this tree; nothing is
illustrative. This is a post-Phase-4 internal study of a
`BitsetFlatOrderBook` candidate (hierarchical-occupancy bitmap) against the
frozen `FlatOrderBook`; no frozen implementation, workload generator, or Phase
2/4 canonical result was modified.

**Host for every run:** the same Apple M3 Max / macOS 14.2.1 (23C71) / Apple
clang 15.0.0 dev machine that produced the Phase 2 dataset
(`../phase2-m3max/`), Release `-O3 -DNDEBUG`, no arch flag. See `host.txt`
(also records the git tree state at capture time and the date).

## Contents

| Path | What it holds | Backs § |
|---|---|---|
| `steady-throughput-1M/` | flat-vs-bitmap steady `apply()` ns/update on the frozen A–E workloads at 1M levels | doc §3.2 |
| `gap-sweep/` | controlled best-delete gap ladder sweep (`g` = 1,2,…,1024), flat vs bitmap | doc §3.3 |
| `gaps-analysis/` | per-workload best-delete re-scan-distance distribution of the real A–E streams | doc §3.5 |
| `memory/` | quantity-array vs occupancy-hierarchy byte accounting | doc §3.6 |
| `host.txt` | machine / OS / compiler / git / date provenance | doc §4 |

### `steady-throughput-1M/`

- `raw/{flat,bits}_{A..E}.log` — five per-process rounds per cell (best-of-3
  inside each process; Phase-2 convention, one impl per process). Workload B
  rows use 500k updates to bound wall time (B's *stream generation* degenerates
  at near-full density — an off-clock, implementation-neutral artifact); A/C/D/E
  use 2M.
- `medians.csv` — median over the 5 rounds of per-process best ns/update (the
  canonical cross-process comparison).
- `inproc.csv` — the interleaved in-one-process read (`--inproc`), a
  drift-free flat-vs-bitmap delta that shares one clock state.
- `flat.csv` / `bits.csv` — single-round per-impl samples (reproduce the frozen
  Phase-2 flat_A_1M ≈ 4.327 ns / flat_C_1M ≈ 5.912 ns references).
- `run.sh`, `command.txt` — exact reproduction.

### `gap-sweep/`

Ladder book whose every timed best-delete rescans exactly `g` slots; 128
interleaved fresh blocks per (impl, gap); K = 4096 best-deletes per block.
`run.log` rows: `impl,gap_ticks,ns_per_delete_best,ns_per_delete_mean,p50,p99,max`.
Measured crossover gap ∈ (4, 8]; flat ≈ 0.26 ns per rescanned slot at g ≥ 64.

### `gaps-analysis/`

`orderbook_bitmap_bench --gaps` output for one workload/scale each: how many of
the stream's deletes hit the current best, and the cumulative distribution of
the re-scan distances those force. Only C deletes the best in volume
(450,467 @1M, 100 % distance ≤ 1); D does 18 (all ≤ 1); A/B/E delete the best
zero times. The frozen workloads never reach the bitmap's large-gap regime.

### `memory/`

Per-scale accounting from the book's own helpers (`quantity_bytes()` vs
`occupancy_bytes()` / `hierarchy_words()`). At 1M: quantity arrays 32,000,000 B
vs occupancy 507,952 B = 1.59 %.

## Reproduce (from the repo root)

```sh
cmake -S . -B build-item14 -DCMAKE_BUILD_TYPE=Release && cmake --build build-item14
bash docs/results/orderbook-bitmap-optimization/steady-throughput-1M/run.sh
./build-item14/orderbook_gap_bench --impl=both --blocks=128 --deletes=4096
./build-item14/orderbook_bitmap_bench both C 1000000 updates=1000000 --gaps
./build-item14/orderbook_bitmap_bench both 1000000 --memory
```

## Honesty rule

Every number here is from a real run on the host recorded in `host.txt`, with
the exact command captured next to it (`command.txt` per subdirectory) and the
raw tool output preserved under `raw/` / `run.log`. No value was fabricated,
edited, or retro-fitted to a hypothesis; where the two measurement methods
disagree (workloads C and E) the analysis in
`docs/ORDERBOOK_BITMAP_OPTIMIZATION.md` says so instead of averaging them away.
