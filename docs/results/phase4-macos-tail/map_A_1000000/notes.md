# Notes — `map A 1000000` (pre-Phase-4.1 artifact — NOT canonical)

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
  `10,000,000 % 512 = 128`-update partial batch was recorded
  (`raw_samples.csv` final row, `batch_operations=128`) and **excluded** from
  all distribution metrics, per spec §7.
- Final-state validation: `final_synced=1`, `final_level_count=2000000` (= 2N).

## Summary (MEASURED / DERIVED)

batch-normalized ns/update (full 512-update batches, nearest-rank):

| metric | ns/update | ratio vs p50 |
|---|---|---|
| mean | 285.48 | — |
| min | 71.78 | — |
| p50 | 267.58 | 1.00 |
| p90 | 374.84 | 1.40 |
| p99 | 496.91 | 1.86 |
| p99.9 | 637.04 | 2.38 |
| max | 1164.79 | 4.35 |

mean / p50 (~267–285 ns/update) sit above the Phase 2 canonical mean (182.714,
`../phase2-m3max/`). **INTERPRETATION (not a measured cause):** this cell is
dominated by the 1 M-node snapshot load and a long ~10 M-update apply; the
higher absolute level vs the Phase 2 best-of-3 number is consistent with the
run-to-run drift already seen between Phase 2 (best-of-3) and the Phase 3M
single-rep anchors (219.7), not evidence of a methodology difference — the Phase
4 mean is over a different (single long sequential) rep, not a min-of-3.
Tail spread is narrower in *ratio* terms than the small-scale cell (max 4.35×
p50 vs 15.3× at `map A 1000`) — INTERPRETATION: at 1M levels the map is
already uniformly slow, so even a scheduling hiccup is a smaller *multiple* of
an already-high median.

## LIMITATION

Same as `map_A_1000/notes.md`: batch-normalized (not per-update); un-pinned
macOS P/E scheduling inflates p99.9/max; a 512-update batch here is ~140 µs, so
timer-boundary overhead is proportionally even smaller than at small scale, but
still not subtracted. **INTERPRETATION** of absolute levels vs Phase 2 is
qualified by the single-rep vs best-of-3 difference.

## Cross-reference

- Raw samples (source of truth): `raw_samples.csv`
- Full metric summary: `summary.txt`
- Methodology: `docs/profiling/PHASE4_TAIL_LATENCY.md`
- Phase 2 mean baseline: `docs/results/phase2-m3max/`
- Phase 3M same-host traces: `docs/results/phase3-macos-apple-silicon/`
