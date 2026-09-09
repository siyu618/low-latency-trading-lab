# Notes — `map C 1000000` (Phase 4 canonical)

## Run

Canonical Phase 4 cell: full deterministic distribution run,
`--updates 10000000 --batch-size 512`, default Phase 2 seed, one process.
See `command.txt` for the exact command, `host.txt` for run-time metadata.

- 19,531 full 512-update batches form the distribution; the trailing
  `10,000,000 % 512 = 128`-update partial batch was recorded and **excluded**
  from all distribution metrics, per spec §7.
- Final-state validation: `final_synced=1`, `final_level_count=1999983`.
  Workload C is level-conserving with transient deficits, so the ending count
  sits just under 2,000,000 — the exact finite-run value the stream produces
  (not a hard-coded 2N).

## Summary (MEASURED / DERIVED)

batch-normalized ns/update (full 512-update batches, nearest-rank):

| metric | ns/update | ratio vs p50 |
|---|---|---|
| mean | 77.04 | — |
| min | 30.60 | — |
| p50 | 75.60 | 1.00 |
| p90 | 84.39 | 1.12 |
| p99 | 105.14 | 1.39 |
| p99.9 | 213.54 | 2.82 |
| max | 273.84 | 3.62 |

mean / p50 (~75–77 ns/update) sit close to the Phase 2 canonical mean (70.551)
and the Phase 3M anchor (75.644). The Phase 2 "why is map C cheap" finding
**holds in the distribution too**: C at 1M is ~3.7× cheaper than A at 1M
(p50 75.6 vs 267.6) — consistent with the same ordering as the mean-based Phase
2 numbers. C's tail ratios are also the **tightest of the map cells**
(p99 1.39× p50, max 3.62×) — INTERPRETATION: workload C's near-touch best-price
churn keeps the map access localized, so both the median and the tail stay low.

## LIMITATION

Same as `map_A_1000/notes.md`: batch-normalized (not per-update); un-pinned
macOS P/E scheduling inflates p99.9/max; timer-boundary overhead not subtracted
(a 512-update batch here is ~39 µs). The `final_level_count < 2N` is expected
for workload C (level-conserving with transient deficits), not a validation
failure.

## Cross-reference

- Raw samples (source of truth): `raw_samples.csv`
- Full metric summary: `summary.txt`
- Methodology: `docs/profiling/PHASE4_TAIL_LATENCY.md`
- Phase 2 mean baseline: `docs/results/phase2-m3max/`
- Phase 3M same-host traces: `docs/results/phase3-macos-apple-silicon/`
