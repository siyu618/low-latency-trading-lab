# Notes — `map A 1000` (Phase 4 canonical)

## Run

Canonical Phase 4 cell: full deterministic distribution run,
`--updates 10000000 --batch-size 512`, default Phase 2 seed, one process.
See `command.txt` for the exact command, `host.txt` for run-time metadata.

- 19,531 full 512-update batches form the distribution; the trailing
  `10,000,000 % 512 = 128`-update partial batch was recorded
  (`raw_samples.csv` final row, `batch_operations=128`) and **excluded** from
  all distribution metrics, per spec §7 (see `summary.txt`
  `partial_batch_present=1`, `partial_batch_ops=128`,
  `partial_ns_per_update=41.34`).
- Final-state validation: `final_synced=1`, `final_level_count=2000` (= 2N).
  All 10 M updates applied; the book's post-stream state is the true stream end.

## Summary (MEASURED / DERIVED)

batch-normalized ns/update (full 512-update batches, nearest-rank):

| metric | ns/update | ratio vs p50 |
|---|---|---|
| mean | 34.22 | — |
| min | 10.34 | — |
| p50 | 33.45 | 1.00 |
| p90 | 35.64 | 1.07 |
| p99 | 55.66 | 1.66 |
| p99.9 | 132.24 | 3.95 |
| max | 511.88 | 15.30 |

mean / p50 (~33–34 ns/update) sit right at the Phase 2 canonical mean
(32.456, `../phase2-m3max/`) and the same-host Phase 3M anchor (34.873,
`../phase3-macos-apple-silicon/`). The tail shows real jitter: p99 is 1.66× p50,
p99.9 ~4× p50, and the single worst batch ran 15.3× the median.

## LIMITATION (applies to every cell in this dataset)

- **batch-normalized, not per-update:** `elapsed/512` is a batch average; a
  single slow update inside a batch is diluted by its neighbours, so the true
  per-update tail is at least as wide as what is shown.
- **MacOS P/E-core scheduling is un-pinned**; background activity can lift
  p99.9/max. A single context switch inside a 512-update batch appears as one
  large sample, so **max is especially scheduling-sensitive** and is not
  attributed to order-book code (Phase 3M profiling is where attribution would
  be tested).
- **Timer-boundary overhead is not subtracted** (see PHASE4_TAIL_LATENCY.md);
  at ~33 ns/update a 512-update batch is ~17 µs, so boundary cost is small but
  nonzero.

## Cross-reference

- Raw samples (source of truth): `raw_samples.csv`
- Full metric summary: `summary.txt`
- Methodology: `docs/profiling/PHASE4_TAIL_LATENCY.md`
- Phase 2 mean baseline: `docs/results/phase2-m3max/`
- Phase 3M same-host traces: `docs/results/phase3-macos-apple-silicon/`
