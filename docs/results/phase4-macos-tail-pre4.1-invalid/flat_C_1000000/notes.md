# Notes — `flat C 1000000` (pre-Phase-4.1 artifact — NOT canonical)

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
- Final-state validation: `final_synced=1`, `final_level_count=1999983`
  (level-conserving workload C, transient deficits — expected).

## Summary (MEASURED / DERIVED)

batch-normalized ns/update (full 512-update batches, nearest-rank):

| metric | ns/update | ratio vs p50 |
|---|---|---|
| mean | 6.23 | — |
| min | 2.69 | — |
| p50 | 6.10 | 1.00 |
| p90 | 6.35 | 1.04 |
| p99 | 9.20 | 1.51 |
| p99.9 | 22.05 | 3.61 |
| max | 59.90 | 9.81 |

mean / p50 (~6.1–6.2 ns/update) sit right at the Phase 2 canonical mean (5.912)
and the Phase 3M anchor (6.697). Flat C's best-price rescan cost over A
(+~1.1–1.2 ns/update at p50, +~1.1 at mean) **reproduces in the distribution**
the Phase 2 mean finding (+~1.5 ns/update canonical). Notably C's **max is much
smaller than flat A's** (59.9 vs 284.6 ns/update) and its tail ratios are
tighter (p99.9 3.6× vs 5.6×, max 9.8× vs 56×) — INTERPRETATION (not a measured
cause): at ~6 ns/update the C batch is only marginally longer than A's, so the
rescan does not obviously add tail events; the smaller max is likely run-to-run
scheduling variation rather than a structural difference, since both flat cells
sit at the mercy of the same OS interruptions.

## LIMITATION

Same as `flat_A_1000000/notes.md`: batch-normalized; un-pinned scheduling; a
512-update batch is only ~3.1 µs, so timer-boundary and scheduling sensitivity
are the dominant tail drivers. `final_level_count < 2N` is expected for
workload C.

## Cross-reference

- Raw samples (source of truth): `raw_samples.csv`
- Full metric summary: `summary.txt`
- Methodology: `docs/profiling/PHASE4_TAIL_LATENCY.md`
- Phase 2 mean baseline: `docs/results/phase2-m3max/`
- Phase 3M same-host traces: `docs/results/phase3-macos-apple-silicon/`
