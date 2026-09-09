# Notes — `map E 1000000` (Phase 4 canonical)

## Run

Canonical Phase 4 cell: full deterministic distribution run,
`--updates 10000000 --batch-size 512`, default Phase 2 seed, one process.
See `command.txt` for the exact command, `host.txt` for run-time metadata.

- 19,531 full 512-update batches form the distribution; the trailing
  `10,000,000 % 512 = 128`-update partial batch was recorded and **excluded**
  from all distribution metrics, per spec §7.
- Final-state validation: `final_synced=1`, `final_level_count=1994440`.
  Workload E does not conserve levels and drifts below N over a finite run; the
  exact ending value is what the deterministic stream produces, not an error.

## Summary (MEASURED / DERIVED)

batch-normalized ns/update (full 512-update batches, nearest-rank):

| metric | ns/update | ratio vs p50 |
|---|---|---|
| mean | 615.33 | — |
| min | 258.14 | — |
| p50 | 550.13 | 1.00 |
| p90 | 915.69 | 1.66 |
| p99 | 1281.98 | 2.33 |
| p99.9 | 1627.85 | 2.96 |
| max | 5077.07 | 9.23 |

mean / p50 (~550–615 ns/update) sit above the Phase 2 canonical mean (383.549).
E is the **slowest and widest map cell**: p50 is ~7× map-C's and ~2× map-A's at
1M, and it is the only map cell whose mean (615) sits well above its median
(550) — the distribution is right-skewed by a heavy tail (p90 already 1.66× p50;
max 5077 ns ≈ 5 µs is a full context-switch-scale interruption). **INTERPRETATION
(not a measured cause):** workload E's uniform add/delete across the whole
2M-tick tree is the map's worst case in the mean (Phase 2) and stays worst in
the tail; it is the cell where allocator activity (if any) would show, and where
a single slow batch is most likely to be a scheduling interruption rather than
book code. Confirming the *cause* of the skew needs Phase 3M profiling.

## LIMITATION

Same as `map_A_1000/notes.md`: batch-normalized (not per-update); un-pinned
macOS P/E scheduling inflates p99.9/max. A 512-update batch here is ~280–310 µs,
so it is long enough that **max (5077 ns/update = ~2.6 ms for the batch) is very
likely a scheduling interruption**, not a property of the book code. The
`final_level_count < 2N` is expected for workload E, not a validation failure.

## Cross-reference

- Raw samples (source of truth): `raw_samples.csv`
- Full metric summary: `summary.txt`
- Methodology: `docs/profiling/PHASE4_TAIL_LATENCY.md`
- Phase 2 mean baseline: `docs/results/phase2-m3max/`
- Phase 3M same-host traces: `docs/results/phase3-macos-apple-silicon/`
