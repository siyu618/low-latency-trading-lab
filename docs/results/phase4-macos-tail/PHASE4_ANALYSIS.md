# Phase 4 analysis — tail latency / jitter on the Apple M3 Max

Everything below is derived from the **canonical, verified Phase 4 dataset** in
this directory — six cells, each a single warmed sequential run of 10,000,000
updates in fixed 512-update batches, seed `407715774446`. Every number traces to
a cell's `summary.txt` (kept at `%.6f` precision) or is recomputed from that
cell's `raw_samples.csv`; all six cells re-verify summary-from-raw in
`verify-all.log`. Environment and run parameters: `RESULTS_METADATA.md`. A
one-page table of the same numbers: `README.md`.

**Units.** All latencies are **batch-normalized ns/update** — a 512-update
batch's wall duration divided by 512. A value of `3.906` means one 512-update
batch took ~2.00 µs. Percentiles are nearest-rank over the 19,531 **full**
batches; the trailing 128-op partial batch is recorded but excluded from every
distribution metric. A number like p99.9 is therefore "the batch-normalized
duration below which 99.9% of full batches fell" — it is a property of batches,
not of individual updates.

**Claim discipline.** Sections label every statement as one of:

- **MEASURED** — a value read directly from a verified `summary.txt`.
- **DERIVED** — recomputed from `raw_samples.csv` (same source, extra arithmetic
  such as ratios or batch counts above a threshold).
- **INTERPRETATION** — a reading of the measured pattern that is plausible but
  not proven here; no microarchitectural attribution is asserted.
- **LIMITATION** — what this dataset cannot support.

---

## The six-cell distribution

MEASURED / DERIVED. ns per update, batch-normalized.

| Cell | mean | p50 | p90 | p99 | p99.9 | max | p99/p50 | p99.9/p50 | max/p50 |
|------|-----:|----:|----:|----:|------:|----:|--------:|----------:|--------:|
| map_A_1000 | 34.963 | 34.424 | 36.297 | 44.922 | 126.709 | 219.564 | 1.305 | 3.681 | 6.378 |
| map_A_1000000 | 187.093 | 177.572 | 214.520 | 328.207 | 456.299 | 644.367 | 1.848 | 2.570 | 3.629 |
| map_C_1000000 | 74.310 | 73.730 | 78.043 | 91.146 | 106.527 | 255.371 | 1.236 | 1.445 | 3.464 |
| map_E_1000000 | 484.410 | 467.367 | 632.080 | 897.543 | 1330.566 | 3016.113 | 1.920 | 2.847 | 6.453 |
| flat_A_1000000 | 3.912 | 3.906 | 4.068 | 4.314 | 6.021 | 16.275 | 1.104 | 1.541 | 4.166 |
| flat_C_1000000 | 6.106 | 6.023 | 6.268 | 7.730 | 18.066 | 70.230 | 1.283 | 2.999 | 11.660 |

Derived batch-outlier counts (full batches whose duration exceeds a multiple of
the cell's p50 batch duration; n = 19,531 full batches per cell):

| Cell | batches > 2×p50 | batches > 3×p50 | batches > 10×p50 |
|------|----------------:|----------------:|-----------------:|
| map_A_1000 | 69 (0.353%) | 27 (0.138%) | 0 |
| map_A_1000000 | 112 (0.573%) | 6 (0.031%) | 0 |
| map_C_1000000 | 6 (0.031%) | 3 (0.015%) | 0 |
| map_E_1000000 | 142 (0.727%) | 15 (0.077%) | 0 |
| flat_A_1000000 | 6 (0.031%) | 1 (0.005%) | 0 |
| flat_C_1000000 | 26 (0.133%) | 19 (0.097%) | 1 (max = 11.66×p50) |

Reading notes before the questions:

- The flat cells are a different universe from the map cells: flat p50s are
  3.9–6.0 ns/update versus 73.7–467.4 for the map at 1M levels — two to three
  orders of magnitude apart, mirroring the Phase 2 throughput gap.
- For every cell the mean sits close to p50 (mean/p50 = 1.002–1.054), because
  the distribution is massed near the median and only a small share of batches
  are slow. Tail information is in the high percentiles and the ratios, not in
  the mean.

---

## Question A — map, update-only (A), 1,000 → 1,000,000 levels

MEASURED. ns/update (batch-normalized); the right column is 1M ÷ 1K.

| Metric | map_A_1000 | map_A_1000000 | 1M ÷ 1K |
|--------|-----------:|--------------:|--------:|
| p50 | 34.424 | 177.572 | 5.16× |
| p90 | 36.297 | 214.520 | 5.91× |
| p99 | 44.922 | 328.207 | 7.31× |
| p99.9 | 126.709 | 456.299 | 3.60× |
| max | 219.564 | 644.367 | 2.93× |
| mean | 34.963 | 187.093 | 5.35× |
| p99 − p50 | 10.498 | 150.635 | 14.3× |
| p99.9 − p50 | 92.285 | 278.727 | 3.0× |

DERIVED. Proportional tail ratios change non-uniformly with scale:

| Ratio | map_A_1000 | map_A_1000000 |
|-------|-----------:|--------------:|
| p99/p50 | 1.305 | 1.848 |
| p99.9/p50 | 3.681 | 2.570 |
| max/p50 | 6.378 | 3.629 |
| batches > 2×p50 | 69 | 112 |
| batches > 3×p50 | 27 | 6 |

What growing the map from 1K to 1M levels does:

1. **The whole distribution moves right, and the p99 moves right fastest.**
   Median grows 5.2×, but p90 grows 5.9× and p99 grows 7.3× — the upper
   boundary of the central distribution stretches more than its middle. The
   absolute width of the near tail, p99 − p50, grows ~14×.
2. **The extreme far tail grows, but less than the median.** p99.9 grows 3.6×
   and max 2.9× (both *below* the 5.2× median growth), so p99.9/p50 and max/p50
   *narrow* from 1K to 1M even as every value in ns rises.
3. **The character of the tail changes.** At 1K the rare slow batches are spread
   widely: 27 (0.14%) exceed 3×p50. At 1M there are *more* moderately slow
   batches — 112 (0.57%) exceed 2×p50, more than at 1K — but almost none are far
   out: only 6 (0.03%) exceed 3×p50.

INTERPRETATION (why the pattern might look like this; not asserted as
proven). At 1M levels a std::map operation walks a pointer chain across
cache lines, so ordinary per-batch work is inherently less uniform than at 1K
(where the whole tree fits near the core) — that pushes the p99 boundary outward
proportionally more than the median and produces the wide band of "moderately
slow" batches seen at 1M. The extreme tail at 1K (p99.9/max at 3.7×/6.4× a
34-ns median) is consistent with rare external events — scheduler or timer
interference landing on a very fast median — rather than with the book itself.
LIMITATION: a single run per cell cannot separate external interference from
book-internal effects; that separation is a Phase 3 job, and the single max
values in particular (one batch in ~19.5k) are the least stable numbers in this
dataset.

---

## Question B — map at 1M levels: A vs C vs E

MEASURED. ns/update (batch-normalized).

| Metric | map_A_1000000 | map_C_1000000 | map_E_1000000 |
|--------|--------------:|--------------:|--------------:|
| p50 | 177.572 | 73.730 | 467.367 |
| p90 | 214.520 | 78.043 | 632.080 |
| p99 | 328.207 | 91.146 | 897.543 |
| p99.9 | 456.299 | 106.527 | 1330.566 |
| max | 644.367 | 255.371 | 3016.113 |
| p99 − p50 | 150.635 | 17.416 | 430.176 |
| p99.9 − p50 | 278.727 | 32.797 | 863.199 |

DERIVED.

| Ratio | map_A_1000000 | map_C_1000000 | map_E_1000000 |
|-------|--------------:|--------------:|--------------:|
| p99/p50 | 1.848 | 1.236 | 1.920 |
| p99.9/p50 | 2.570 | 1.445 | 2.847 |
| max/p50 | 3.629 | 3.464 | 6.453 |
| batches > 2×p50 | 112 | 6 | 142 |
| batches > 3×p50 | 6 | 3 | 15 |

Answers among the three 1M-level map workloads:

- **Lowest median: C.** C's p50 is 73.730 ns/update — 2.4× faster than A's
  177.572 and 6.3× faster than E's 467.367. This *ordering* (C < A < E) is the
  same one Phase 2 measured for these cells' best-of-3 means (C 70.6 < A 182.7
  < E 383.5), so the ranking is consistent across two independent measurement
  families on this host.
- **Widest absolute tail: E, by a large margin.** E's p99 (897.5) exceeds A's
  max (644.4); E's p99.9 is 1330.6 and its max is 3016.1. Absolute spread
  p99.9 − p50 is 863 ns for E versus 279 ns for A and 33 ns for C. Note that E's
  spread is larger in absolute terms than A's or C's entire medians.
- **Widest proportional tail: E.** E leads on the far-tail ratios — max/p50
  6.45 (vs A 3.63, C 3.46), p99.9/p50 2.85 (vs A 2.57, C 1.44) — and is
  essentially tied with A on p99/p50 (1.92 vs 1.85). A is second, C is the
  narrowest distribution of the three by every ratio (and also by absolute ns).
  E's *only* non-extreme tail number is its >3×p50 count (15), which trails A's
  total >2×p50 count — consistent with E having a high median and a long,
  sparsely populated upper tail.

MEASURED (structural, from the workload definitions, not latency): C confines
its churn to the best end of the book (~45% of ops delete the current best level
and the rest refill just below it), whereas A re-quantifies uniformly random
levels across the full 1M-level domain and E is a fair-coin, whole-domain
delete/add workload whose occupancy drifts (its run ended at 1,994,440 of
2,000,000 levels). E is also the slowest *and* the noisiest; C is the fastest
*and* the steadiest.

INTERPRETATION (labeled, not asserted): that C, the workload whose map activity
is concentrated near the top of the book, is both fastest and steadiest is
consistent with its operations staying in a small, repeatedly-touched region of
the tree — but explaining *why* that locality shows up in the tree's cost is a
Phase 3 attribution question and is not answered by this dataset. The consistent
C < A < E ordering across Phase 2 and Phase 4 is a MEASURED pattern; its cause
is not established here.

---

## Question C — flat at 1M levels: A vs C (best-price deletion)

MEASURED. ns/update (batch-normalized); right column is C ÷ A.

| Metric | flat_A_1000000 | flat_C_1000000 | C ÷ A |
|--------|---------------:|---------------:|------:|
| p50 | 3.906 | 6.023 | 1.54× |
| p90 | 4.068 | 6.268 | 1.54× |
| p99 | 4.314 | 7.730 | 1.79× |
| p99.9 | 6.021 | 18.066 | 3.00× |
| max | 16.275 | 70.230 | 4.31× |

DERIVED.

| Ratio | flat_A_1000000 | flat_C_1000000 |
|-------|---------------:|---------------:|
| p99/p50 | 1.104 | 1.283 |
| p99.9/p50 | 1.541 | 2.999 |
| max/p50 | 4.166 | 11.660 |
| batches > 2×p50 | 6 | 26 |
| batches > 3×p50 | 1 | 19 |
| batches > 10×p50 | 0 | 1 |

Effect of frequent best-price deletion on the flat book:

1. **It raises the whole distribution, and the tail more than the middle.**
   Median and p90 rise 1.54×; p99 rises 1.79×; p99.9 rises 3.00×; max rises
   4.31×. Under workload C ~45% of ops delete the current best level, and the
   flat book answers a best deletion by re-scanning inward to the next live
   level — that rescan is the mechanism already identified in the Phase 2 README
   for C's extra flat cost.
2. **It is the single most outlier-prone cell in the dataset.** flat_C has the
   widest max/p50 ratio of any cell (11.66 — the only full batch in any cell
   above 10× its p50), and 19 batches (0.10%) above 3×p50 versus 1 for flat_A.

INTERPRETATION (labeled): a typical C batch pays a best-deletion rescan on
roughly half its ops, which explains the ~1.5× shift of the central
distribution; the occasional batches that hit *runs* of best-deletions pay many
rescans in one batch, which plausibly accounts for the far tail (p99.9 3×,
max 11.7×p50). The flat book's rescan cost being proportional to how far it must
scan is a documented property of the implementation, but attributing the tail
shape to specific scan-length sequences is not established from this dataset.
LIMITATION: flat_C's single 70.2-ns batch — 11.7× its median — is one sample in
19.5k and could include external interference; do not read the precise magnitude
of that one batch as a book property.

---

## Question D — what Phase 2's mean alone hid

Phase 2 (canonical, frozen) reported one number per cell: **best-of-3 mean**
`apply()` ns/update over 2M-op blocks. Best-of-3 deliberately reports the
*faster* of three blocks to discount scheduler noise, and a mean collapses the
distribution. Phase 4 shows what that choice could not express:

1. **flat_C's tail story is invisible in its mean.** The Phase 2 means said C is
   ~37% more expensive than A on flat (5.91 vs 4.33). Phase 4 shows the tail is
   the larger fact: p99.9 is 3× A's, the max is 4.3×, and flat_C has the only
   >10×p50 batch in the dataset (max/p50 11.66 vs 4.17 for A). A tail budget set
   from the mean would be off by ~3× at p99.9.
2. **map_A_1000's rare but large outliers.** Its mean (34.96) is only 1.6% above
   its p50 (34.42) — the mean makes the 1K map look essentially deterministic.
   Yet 0.35% of its batches exceed 2×p50, its p99.9 is 3.7×p50, and its single
   max is 6.4×p50 (~220 ns on a 34-ns median). Phase 2's best-of-3 mean (32.46)
   is even *below* this run's p50 — the right number for comparing steady
   throughput, the wrong number for anyone budgeting a tail.
3. **map E is not just slower, it is qualitatively noisier — in a way the mean
   flattens.** Phase 2's means already separated E (383.5) from C (70.6). But the
   mean cannot convey that E has 142 batches above 2×p50 (C has 6), that E's
   p99.9 is 1331 ns (2.85× its p50, and 2.9× *A's* p99.9), or that a small share
   of E batches exceed 3 µs on a 467-ns median. E is the worst cell by both
   central cost and tail; the mean shows only the former.
4. **The mean understates how fast map p99 degrades with scale.** From 1K to 1M
   levels, map A's mean grows 5.35× but its p99 grows 7.31×. Budgeting by the
   mean would under-provision the p99 by ~35% at the larger scale. (It also
   *over*states tail risk at small scale, where the far tail is dominated by
   rare external events on a fast median — Question A.)

General point: best-of-3 mean reports the distribution's *floor*; Phase 4's
p50/p90/p99/p99.9 show the floor is not the shape. Both are real and
complementary — mean for steady-state throughput comparison, the percentiles for
any claim about worst-case or jitter.

---

## Phase 2 cross-reference (same host, qualitative only)

The six Phase 4 cells correspond to Phase 2 cells measured on the **same M3 Max
host**. ns/update. Phase 2 = best-of-3 mean over 2M-op blocks (FROZEN, from
`../phase2-m3max/`); Phase 4 = this dataset (single 10M-op warmed run,
batch-normalized).

| Cell | Phase 2 best-mean | Phase 4 p50 | Phase 4 mean |
|------|------------------:|------------:|-------------:|
| map_A_1000 | 32.456 | 34.424 | 34.963 |
| map_A_1000000 | 182.714 | 177.572 | 187.093 |
| map_C_1000000 | 70.551 | 73.730 | 74.310 |
| map_E_1000000 | 383.549 | 467.367 | 484.410 |
| flat_A_1000000 | 4.327 | 3.906 | 3.912 |
| flat_C_1000000 | 5.912 | 6.023 | 6.106 |

Methodology difference (do not read as a regression): Phase 2 took the minimum
wall time of 3 × 2M-op blocks with block-level timing; Phase 4 is a single
10M-op run timed per 512-update batch and warmed end-to-end. The two are not
expected to be equal, and the differences go in both directions (map_A_1000 and
map_C/E read slightly higher in Phase 4; flat_A reads slightly lower) with no
consistent bias. Only the *ordering* of cells is meaningfully comparable here,
and it agrees between the two phases: C < A < E on the map at 1M levels, and
flat well under map everywhere. Nothing in this comparison is a regression
claim.

---

## Limitations

- **One machine, one run per cell.** All numbers are from the Apple M3 Max dev
  host, single-run (the spec forbids re-running a cell just because a tail looks
  bad). Apple Silicon mixes P and E cores and macOS schedules freely; no
  affinity pinning is claimed (per-cell `host.txt`). A repeat run would move
  individual numbers, especially maxima.
- **Maxima are the least trustworthy numbers.** Each max is one batch in 19,531.
  Treat max/p50 as indicative of the far tail, not as a measured property of the
  book; do not assert a cause for any single max (scheduler/timer interference
  is a plausible contributor and is not excluded).
- **Batch granularity.** Batch-normalized ns/update averages 512 ops per sample.
  A batch containing one slow op and 511 fast ones reports only a small
  elevation, so per-op single-update tail latency is *not* measured here — the
  distribution is over batches, by design.
- **Occupancy drift is real but expected.** map/flat C ended at 1,999,983 levels
  and map E at 1,994,440 (of 2,000,000); each cell reports `final_synced=1` and
  `final_seq == final_seq_expected`. The drift is a property of the generated
  streams, not an error, and it means C/E cells measure a book a hair below full
  occupancy.
- **Phase 2 comparison is qualitative.** Same host, different methodology (above);
  differences between individual Phase 2 and Phase 4 values are not
  regression/improvement evidence.
- **No microarchitectural attribution.** INTERPRETATION sections name plausible
  mechanisms; determining where time actually goes is Phase 3M/3L work, which
  this document does not attempt.
