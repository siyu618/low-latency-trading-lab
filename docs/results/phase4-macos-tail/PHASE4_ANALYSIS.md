# Phase 4 analysis — tail latency and jitter on the Apple M3 Max

**Status: real canonical data, reviewed.** Six cells, each a full deterministic
distribution run (`--updates 10000000 --batch-size 512`, default seed, one
process per book) on the same Apple M3 Max / macOS 14.2.1 host as the Phase 2
and Phase 3M datasets. Every number below is measured by the benchmark
(`summary.txt` / `raw_samples.csv`); labels follow the honesty rule.

## Method recap (what these numbers are)

- Each metric is **batch-normalized** `elapsed_ns / 512` over a fixed 512-update
  batch — a batch average, NOT a directly measured single-update latency (which
  would be dominated by clock overhead at these ns/update costs). See
  `docs/profiling/PHASE4_TAIL_LATENCY.md`.
- 19,531 full batches form each distribution; the trailing 128-update partial
  batch is recorded but excluded from every metric.
- Percentiles are **nearest-rank** on the raw integer batch durations — each is
  a duration a batch actually took.
- All cells share the same 512-batch size, so distributions are comparable, but
  the **absolute ns scale differs ~100×** between flat (~5 ns/update) and map E
  (~600 ns/update). A scheduling interruption of fixed wall-time therefore lands
  as a vastly larger *multiple* of p50 on the flat cells than on the map cells.
  This is central to reading the tail ratios below.

## MEASURED / DERIVED — the six canonical distributions

Batch-normalized ns/update. `p50` is the median batch; ratios are vs that cell's
own p50.

| Cell | mean | p50 | p90 | p99 | p99.9 | max | max/p50 |
|---|---|---|---|---|---|---|---|
| `map A 1000` | 34.22 | 33.45 | 35.64 | 55.66 | 132.24 | 511.88 | 15.3× |
| `map A 1000000` | 285.48 | 267.58 | 374.84 | 496.91 | 637.04 | 1164.79 | 4.4× |
| `map C 1000000` | 77.04 | 75.60 | 84.39 | 105.14 | 213.54 | 273.84 | 3.6× |
| `map E 1000000` | 615.33 | 550.13 | 915.69 | 1281.98 | 1627.85 | 5077.07 | 9.2× |
| `flat A 1000000` | 5.13 | 5.04 | 6.27 | 11.96 | 28.48 | 284.59 | 56.4× |
| `flat C 1000000` | 6.23 | 6.10 | 6.35 | 9.20 | 22.05 | 59.90 | 9.8× |

### Internal consistency checks (MEASURED)

- Every cell: `total_samples=19532`, `distribution_samples=19531`,
  `partial_batch_present=1`, `partial_batch_ops=128` — the canonical
  `10,000,000 % 512 = 128` partial batch was recorded and excluded as designed.
- `final_synced=1` in all six; `final_seq = 2N + 10,000,000`. Ending level
  counts are 2N for A/flat A, and 1,999,983 / 1,994,440 for the C / E cells —
  the workloads' expected level drift, not validation failures.
- **The mean tracks the Phase 2 mean ordering exactly**, and each cell's
  mean/p50 lands at the same scale as its Phase 2 canonical mean and Phase 3M
  single-rep anchor:

| Cell | Phase 4 mean | Phase 2 mean | Phase 3M anchor |
|---|---|---|---|
| `map A 1000` | 34.2 | 32.5 | 34.9 |
| `map A 1000000` | 285.5 | 182.7 | 219.7 |
| `map C 1000000` | 77.0 | 70.6 | 75.6 |
| `map E 1000000` | 615.3 | 383.5 | 513.9 |
| `flat A 1000000` | 5.13 | 4.33 | 5.08 |
| `flat C 1000000` | 6.23 | 5.91 | 6.70 |

  The Phase 4 means sit at or above the Phase 2 best-of-3 means — expected: a
  single long sequential rep (not a min-of-3) plus the run-to-run drift already
  seen between Phase 2 and the Phase 3M single-rep anchors. **The relative
  ordering and the cross-cell gaps are preserved**, so the tail comparison below
  is read against the same structure Phase 2 established.

## Answers to the tail questions

### Q4a. Is the tail "hotter" (wider) than the mean suggests?

**MEASURED — yes, on every cell.** Even the tightest cells show a real
distribution, not a point mass:

- **flat A 1000000** has the widest *ratio* tail: p90 is 1.24× p50, but p99 is
  2.37×, p99.9 is 5.65×, and max is **56× p50**. The mass is a very tight
  ~5 ns core; the tail is interruption-driven.
- **map A 1000** (small scale) is the widest *map* tail: p99 1.66× p50, p99.9
  3.95×, max 15.3× p50.
- **map C 1000000** is the tightest map cell (max 3.6× p50, p99 1.39×) — C is
  not just cheaper, its tail is proportionally tighter.
- **map E 1000000** is the widest in absolute ns and the most right-skewed:
  its mean (615) is 12 % above its median (550), and p90 is already 1.66× p50.
  Its max, 5077 ns/update ≈ 2.6 ms for the batch, is the largest single sample
  in the dataset.

### Q4b. Which tail should a latency-sensitive consumer care about?

**INTERPRETATION (from the measured ratios, not a measured cause):**

- For **flat** cells the mean/p50 (~5–6 ns) is misleading as a latency promise:
  a consumer will regularly see 2× (p99) and occasionally 50× (max) that — but
  those multiples are dominated by **transient OS interruptions**, because a
  512-update batch is only ~2.6–3.1 µs and any scheduling tick dwarfs the work
  (`LIMITATION`). The flat book's *own* tail contribution is too small to
  separate from the OS here.
- For **map** cells the absolute tail (hundreds to thousands of ns/update) is
  larger than flat's *even at p50*, so a latency-sensitive consumer comparing
  the two books should weight the map's p99/p99.9 directly against the flat's —
  the map is slower at every percentile, and flat's large *ratio* tail is on a
  ~100× smaller absolute base.
- **map E** is the worst absolute tail (p99 ≈ 1282, p99.9 ≈ 1628, max ≈ 5077
  ns/update) — a consumer running uniform full-book churn against a map book at
  1M levels should budget for multi-microsecond apply() outliers, with the max
  most plausibly a scheduling interruption (`LIMITATION`).

### Q4c. Do the Phase 2 "why" findings reproduce in the tail?

**MEASURED — the ordering reproduces, the cost gaps persist:**

- **map C ≪ map A at 1M** holds in the tail as in the mean: C p50 75.6 vs A
  p50 267.6 (~3.5×), and C p99 105 vs A p99 497 (~4.7×). C is cheaper *and* has
  a proportionally tighter tail — consistent with (but not proof of) the Phase 2
  hypothesis that C's near-touch churn localizes the tree access.
- **map grows with scale**: A 1000 → A 1M lifts p50 ~8× (33 → 268) and p99 ~9×
  (56 → 497).
- **flat C costs more than flat A** at the median too: C p50 6.10 vs A p50 5.04
  (+~1.1 ns/update), matching the Phase 2 mean gap (+~1.5). C's max here is
  smaller than A's, but that is run-to-run scheduling noise (`INTERPRETATION`),
  not evidence the rescan removes tail events.

## LIMITATION (what qualifies every claim above)

1. **Batch-normalized, not per-update.** A slow update inside a batch is diluted
   by its 511 neighbours. The true per-update tail is *at least as wide* as what
   is shown — especially for the flat cells (512 ops at ~5 ns each).
2. **Timer boundary overhead is not subtracted** (see PHASE4_TAIL_LATENCY.md).
3. **macOS P/E scheduling is un-pinned.** A single context switch inside a batch
   appears as one large sample. This most affects **max**, and most of all the
   flat cells (short batches) and map E (the longest wall-clock run). A large
   max is **not attributed to order-book code**; Phase 3M profiling is where
   such attribution would be tested.
4. **Single run per cell** — one process, one distribution. Run-to-run and
   thermal drift are documented in the Phase 2 metadata and visible in the
   Phase 4-vs-Phase 2 absolute gap; the *ordering* and *relative* findings are
   robust to it, but the absolute percentiles are one sample of the host's noise
   environment.
5. Workload C/E ending level counts < 2N are the workloads' expected drift, not
   a validation failure.

## INTERPRETATION summary (hypotheses the data supports but does not prove)

- The flat book's huge *ratio* tail (max/p50 up to 56×) is overwhelmingly OS
  interruption on a tiny absolute base, not a property of the array store —
  supported by the p90 being only ~1.2× p50 while max is ~56×, i.e. the tail is
  sparse and spike-like rather than a broad distribution.
- Map C's tight tail (smallest max/p50 of the map cells) is consistent with the
  locality hypothesis but unproven without profiling.
- Map E's right-skew (mean 12 % above median, p90 1.66× p50) is consistent with
  a workload that periodically hits the map's worst traversal/allocator path,
  but the cause needs Phase 3M.
