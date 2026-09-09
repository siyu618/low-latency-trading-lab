# orderbook-bitmap-optimization — Experiment 01 Optimization Study dataset

**Status: COMPLETE (credibility-hardened).** Real, committed measurements backing
the analysis in
[`docs/ORDERBOOK_BITMAP_OPTIMIZATION.md`](../../ORDERBOOK_BITMAP_OPTIMIZATION.md).
All rows in that document come from the raw files in this tree; nothing is
illustrative. This is a post-Phase-4 internal study of a `BitsetFlatOrderBook`
candidate (hierarchical-occupancy bitmap) against the frozen `FlatOrderBook`,
**with a no-bitmap control** (`TransitionAwareFlatOrderBook`) added so the
candidate's two changes — transition-aware control flow and the occupancy
bitmap — are measured separately. No frozen implementation, workload generator,
or Phase 2/4 canonical result was modified.

**Host for every run:** the same Apple M3 Max / macOS 14.2.1 (23C71) / Apple
clang 15.0.0 dev machine that produced the Phase 2 dataset (`../phase2-m3max/`),
Release `-O3 -DNDEBUG`, no arch flag. See `host.txt` (also records the git tree
state at capture time and the date). Control-isolation runs are same-host,
same-build, same-date as the frozen runs; because the bitmap's absolute
per-process values are window-dependent on this host, cross-session comparison
of `bits` rows is NOT meaningful — see the steady command.txt caveat.

## Contents

| Path | What it holds | Backs § |
|---|---|---|
| `steady-throughput-1M/` | **frozen** two-impl (flat/bits) steady dataset, A–E @1M | doc §4.2 harness cross-check; historical flat-vs-bits |
| `gap-sweep/` | **frozen** single forward best-delete gap ladder (flat vs bits) | doc §4.4 historical; superseded for claims by `gap-crossover/` |
| `gaps-analysis/` | per-workload best-delete re-scan-distance distribution of the real A–E streams | doc §4.5 |
| `memory/` | quantity-array vs occupancy-hierarchy byte accounting | doc §4.6 |
| `control-isolation/steady-throughput-1M/` | **three-impl** steady dataset: flat / tuned / bits, per-process 5-round medians + 3-way in-process deltas | doc §4.2 (the isolation result) |
| `control-isolation/gap-crossover/` | **10-round** gap crossover (5 fwd + 5 rev), per-gap median p50 + sign-consistency | doc §4.4 |
| `fixed-domain-gap-validation/` | **6-round** `--fixed-domain` gap crossover (memory-span control) | doc §4.4 |
| `host.txt` | machine / OS / compiler / git / date provenance | doc §6 |

### `steady-throughput-1M/` (frozen, two-impl)

- `raw/{flat,bits}_{A..E}.log` — five per-process rounds per cell (best-of-3
  inside each process; Phase-2 convention, one impl per process). B uses 500k
  updates to bound wall time (B's *stream generation* degenerates at near-full
  density — an off-clock, implementation-neutral artifact); A/C/D/E use 2M.
- `medians.csv` — median over the 5 rounds of per-process best ns/update.
- `inproc.csv` — the interleaved in-one-process two-way read (`--inproc`),
  drift-free flat-vs-bits.
- `flat.csv` / `bits.csv` — one-round per-impl samples.
- `run.sh`, `command.txt` — exact reproduction. (run.sh/command.txt corrected in
  the hardening pass: B's effective update count documented honestly.)

### `control-isolation/steady-throughput-1M/` (three-impl; the isolation result)

- `raw/{flat,tuned,bits}_{A..E}.log` — five per-process rounds per (impl, wl),
  impl start order rotated per round.
- `medians.csv` — 3-impl cross-process medians + flat→tuned / tuned→bits /
  flat→bits per cent.
- `inproc3.csv` — 3-way in-process drift-free deltas (24 blocks, 32 for B).
- `run.sh`, `run_inproc3.sh`, `command.txt` — exact reproduction + the
  measurement-stability caveat (per-process `bits` spans ±40 % round to round on
  this host; in-process deltas are authoritative; robustness = both methods
  agree on sign).

### `control-isolation/gap-crossover/` and `fixed-domain-gap-validation/`

Each gap sweep is the deterministic ladder (K = 4096, 128 fresh blocks/impl/gap)
as repeated independent rounds, forward and reverse, with per-gap median-over-
rounds p50 for flat and bits plus how many rounds the "bits faster" sign held.
Crossover gap: **g = 8** (10/10 rounds variable-domain; 6/6 fixed-domain).
Round logs under `var-domain/` / `rounds/`; merged results in
`analysis-var-domain.txt` / `analysis-fixed-domain.txt`; provenance in the
respective `command.txt`.

### `gaps-analysis/`

`orderbook_bitmap_bench --gaps` output for one workload/scale each: how many of
the stream's deletes hit the current best, and the cumulative distribution of
the re-scan distances those force. Only C deletes the best in volume
(450,467 @1M and a re-verified 900,944 @2M, 100 % distance ≤ 1); D does 18 (all
≤ 1); A/B/E delete the best zero times. The frozen workloads never reach the
bitmap's large-gap regime.

### `memory/`

Per-scale accounting from the book's own helpers. At 1M: quantity arrays
32,000,000 B vs occupancy 507,952 B = 1.59 %.

## Reproduce (from the repo root)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON && cmake --build build
ctest --test-dir build --output-on-failure

# three-impl control-isolation steady data (A/C/D/E @2M, B @500k)
bash docs/results/orderbook-bitmap-optimization/control-isolation/steady-throughput-1M/run.sh
bash docs/results/orderbook-bitmap-optimization/control-isolation/steady-throughput-1M/run_inproc3.sh

# gap crossover: 10 variable-domain + 6 fixed-domain rounds
bash docs/results/orderbook-bitmap-optimization/control-isolation/gap-crossover/run.sh
python3 docs/results/orderbook-bitmap-optimization/control-isolation/gap-crossover/analysis.py

# frozen two-impl steady + a single gap ladder
bash docs/results/orderbook-bitmap-optimization/steady-throughput-1M/run.sh
./build/orderbook_gap_bench --impl=both --blocks=128 --deletes=4096
./build/orderbook_bitmap_bench flat C 1000000 updates=2000000 --gaps
./build/orderbook_bitmap_bench both 1000000 --memory
```

## Honesty rule

Every number here is from a real run on the host recorded in `host.txt`, with
the exact command captured next to it (`command.txt` per subdirectory) and the
raw tool output preserved under `raw/` / `run.log`. No value was fabricated,
edited, or retro-fitted to a hypothesis; where the two measurement methods
disagree (e.g. workload A's bitmap-over-control delta, or the per-process vs
in-process `bits` magnitudes) the analysis in
`docs/ORDERBOOK_BITMAP_OPTIMIZATION.md` says so instead of averaging them away,
and calls a finding robust only when both methods agree on its sign.
