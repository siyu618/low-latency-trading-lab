# Notes — `flat A 1000000` (pre-Phase-4.1 artifact — NOT canonical)

> ⚠️ Produced by the pre-fix buggy tooling: this cell's `summary.txt` and
> `raw_samples.csv` came from TWO independent benchmark runs, and the summary's
> distribution included the trailing 128-update partial batch. The numbers in
> this file are INVALID — do not cite. See `./README.md` in this directory; the
> cell must be re-measured with the hardened tooling.

## Run

Pre-fix Phase 4 cell (NOT canonical): full deterministic distribution run,
`--updates 10000000 --batch-size 512`, default Phase 2 seed, one process.
See `command.txt` for the exact command, `host.txt` for run-time metadata.

- 19,531 full 512-update batches form the distribution; the trailing
  `10,000,000 % 512 = 128`-update partial batch was recorded and **excluded**
  from all distribution metrics, per spec §7.
- Final-state validation: `final_synced=1`, `final_level_count=2000000` (= 2N).

## Summary (MEASURED / DERIVED)

batch-normalized ns/update (full 512-update batches, nearest-rank):

| metric | ns/update | ratio vs p50 |
|---|---|---|
| mean | 5.13 | — |
| min | 2.69 | — |
| p50 | 5.04 | 1.00 |
| p90 | 6.27 | 1.24 |
| p99 | 11.96 | 2.37 |
| p99.9 | 28.48 | 5.65 |
| max | 284.59 | 56.41 |

mean / p50 (~5.0–5.1 ns/update) sit right at the Phase 2 canonical mean (4.327)
and the Phase 3M anchor (5.075). The **mean is extremely tight** (p90 only 1.24×
p50), but the tail is the **widest in ratio terms in the whole dataset**
(p99.9 5.65× p50, max 284.59 ns/update ≈ 56× p50). **INTERPRETATION (not a
measured cause):** flat A's steady-state store is so fast that virtually every
slow batch is a *transient interruption* — a scheduler tick, cache/thermal
event — rather than a property of the store; a single context switch inside a
512-update batch at 5 ns/update (~2.6 µs per batch) dwarfs the work. The
distribution's mass is a very tight ~5 ns core with a spiky, interruption-driven
tail.

## LIMITATION

Same as `map_A_1000/notes.md`: batch-normalized (not per-update); un-pinned
macOS P/E scheduling. Here the effect is **largest**: a 512-update batch is only
~2.6 µs, so the timer boundary is not negligible relative to it and the
scheduling sensitivity of max (56× p50) is extreme. The flat cells are also the
cells where batch-normalization most dilutes any true per-update spike (512 ops
at 5 ns each).

## Cross-reference

- Raw samples (source of truth): `raw_samples.csv`
- Full metric summary: `summary.txt`
- Methodology: `docs/profiling/PHASE4_TAIL_LATENCY.md`
- Phase 2 mean baseline: `docs/results/phase2-m3max/`
- Phase 3M same-host traces: `docs/results/phase3-macos-apple-silicon/`
