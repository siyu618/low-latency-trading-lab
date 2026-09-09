# Phase 3M analysis — answering the Phase 2 "why" questions (draft)

**Status: analysis draft.** Six real Time Profiler recordings are committed and
cross-checked headlessly (sample counts, P/E-core mix, thread-state, code
fragment counts). What is **not** yet derived is a **symbolized call tree** —
headless export of a Release trace gives raw PC addresses only, and symbolication
requires the Instruments GUI (see each cell's `trace_notes.md` LIMITATION). This
draft therefore answers the Phase 2 "why" questions with the evidence actually
in hand and states, per question, exactly which profiling observation would
upgrade the answer.

## Evidence inventory — what is real, and what it is

All numbers below come from the **same host** (Apple M3 Max, macOS 14.2.1)
that produced the Phase 2 canonical dataset (`../phase2-m3max/`), recorded at
commit `8383dc8`, tree clean. Labeling conventions per the honesty rule.

### MEASURED (Phase 2 canonical, best-of-3) — from `results_2M_reps3_isolated.csv`

| Cell | Canonical ns/update (best of 3) |
|---|---|
| `map A 1000` | 32.456 |
| `map A 1000000` | 182.714 |
| `map C 1000000` | 70.551 |
| `map E 1000000` | 383.549 |
| `flat A 1000000` | 4.327 |
| `flat C 1000000` | 5.912 |

### MEASURED (same-host recording anchor, `reps=1`, ungated) — `anchor_ns.txt` per cell

The anchors are single-rep runs (`reps=1`, matching each recording's one apply
block), **not** Phase 2 best-of-3, so each sits slightly above its canonical
number — expected, and not comparable one-for-one:

| Cell | Anchor ns/update | Ratio vs canonical |
|---|---|---|
| `map A 1000` | 34.873 | 1.07× |
| `map A 1000000` | 219.734 | 1.20× |
| `map C 1000000` | 75.644 | 1.07× |
| `map E 1000000` | 513.941 | 1.34× |
| `flat A 1000000` | 5.075 | 1.17× |
| `flat C 1000000` | 6.697 | 1.13× |

The ratio pattern (E 1.34×, map A 1M 1.20× — the slow, long cells — drifting
farthest above best-of-3) is consistent with **thermal/scheduling drift between
reps on one machine**, which the Phase 2 metadata already flags as a known
limitation; the anchors preserve the canonical *ordering* (E ≫ A > C for map at
1M; flat ~5–7) and are the baseline the recordings are read against.

### OBSERVED IN INSTRUMENTS (headless time-sample export of the committed traces)

| Cell | Samples | Core mix (P/E) | Distinct fragments | Apply-block share of process * |
|---|---|---|---|---|
| `map A 1000` | 1,166 | 1,133 / 33 | 9 | ~41 % |
| `map A 1000000` | 5,693 | 5,692 / 1 | 51 | ~70 % |
| `map C 1000000` | 2,209 | 2,209 / 0 | 51 | ~66 % |
| `map E 1000000` | 32,117 | 32,109 / 8 | 160 | ~32 % |
| `flat A 1000000` | 545 | 511 / 34 | 14 | ~11 % |
| `flat C 1000000` | 693 | 693 / 0 | 12 | ~17 % |

\* **DERIVED (estimate):** `anchor ns/update × 20,000,000 / process duration`.
It estimates how much of each process's sampled wall-time the timed `apply()`
block could occupy, assuming the ungated anchor holds at 20M updates. It is an
estimate, not a measurement — the recording process's own latency is unmeasured
(xctrace deferred the launch), and process duration includes startup/teardown +
the untimed snapshot load. Its use is purely to frame the LIMITATION below:
**without a signpost interval to scope the GUI call-tree read to, a whole-process
read would spend a large share of its samples outside `apply()`** — most
dramatically for the flat cells (~11–17 %) and workload E (~32 %).

## The three Phase 2 "why" questions — current state of evidence

### Q1. Why does map latency grow as the tree grows? (`map A 1000` → `map A 1000000`)

- **MEASURED:** ~32.5 → ~182.7 ns/update canonical (best-of-3); the recording
  anchors bracket the same ~6× gap (~34.9 → ~219.7 ns/update at `reps=1`).
- **OBSERVED IN INSTRUMENTS:** the two recordings differ sharply in sample
  yield and fragment diversity — `map A 1000` 1,166 samples across 9 distinct
  code fragments; `map A 1000000` 5,693 samples across 51 fragments — i.e. at
  1M levels the hot PC set is far more diverse, consistent with a deeper/larger
  traversal. (Fragment count is a coarse page-level proxy; no symbols yet.)
- **INTERPRETATION (hypothesis, NOT measured):** scale growth in a `std::map`
  apply is commonly dominated by tree traversal / pointer-chasing / cache
  misses, not allocator churn (workload A re-quantifies existing nodes and does
  not allocate in the timed path). The fragment-count jump is *consistent with*
  a larger instruction footprint, but does **not** prove the cause.
- **LIMITATION / what would answer it:** the GUI heavy-path read of both traces
  (what fraction of samples is inside `MapOrderBook::apply` and its tree walk)
  and — on a Linux box — `perf` cache-miss counters (Phase 3L). Without those,
  "cache misses vs allocator vs branch cost" stays a hypothesis.

### Q2. Why is map C empirically cheaper than map A/E at 1M? (`map C 1000000` vs A/E)

- **MEASURED:** canonical 70.551 (C) vs 182.714 (A) vs 383.549 (E) ns/update;
  recording anchors 75.6 vs 219.7 vs 513.9 — C is ~3× cheaper than A and ~7×
  cheaper than E at 1M in both.
- **OBSERVED IN INSTRUMENTS:** C's trace is the **only** map cell with **zero**
  E-core samples (2,209/0 P/E) and the fewest distinct cores touched (8), and it
  shares the same 51-fragment diversity as map A 1M but with ~2.6× fewer samples
  for a shorter wall-clock (2.30 s vs 6.30 s). Nothing in the headless facts
  attributes the cheapness; they only confirm C ran a shorter, P-core-only,
  still-fragment-rich block.
- **INTERPRETATION (hypothesis, NOT measured):** workload C's best-price churn
  keeps the hot access near the touchpoint (improved locality / branch
  predictability) versus E's uniform full-tree churn — the mechanism Phase 2
  deferred. **Unsupported by the headless trace.**
- **LIMITATION / what would answer it:** the GUI heavy path for C vs A vs E at
  1M — where samples concentrate (tree root vs deep nodes), and whether E's
  trace (160 fragments, the richest) shows allocator frames that C/A lack.
  Workload A does not allocate in the timed path, but E (uniform add/delete)
  does — so E is the cell where allocator activity, if any, would show.

### Q3. What does the flat book's best-price rescan actually cost? (`flat C 1000000` vs A)

- **MEASURED:** canonical 5.912 (C) vs 4.327 (A); recording anchors 6.697 vs
  5.075 — workload C's frequent best-deletion rescan costs roughly **+37 %
  (canonical) to +32 % (anchor)** over A's pure store at 1M.
- **OBSERVED IN INSTRUMENTS:** both flat traces are P-core-dominant; C ran
  P-core-only (693/0), A spent 34 of 545 samples on E-cores. Sample counts are
  the matrix's smallest (545 / 693) because flat apply is ~5–7 ns/update: even
  at 20M updates the timed block is only ~0.10–0.13 s of a ~0.8–1.0 s process.
  **Few samples, no symbols** — nothing here yet attributes the +32–37 %.
- **INTERPRETATION (hypothesis, NOT measured):** the +1.4–1.6 ns/update is the
  inward best re-scan on delete — the obvious candidate, but **unconfirmed**.
- **LIMITATION / what would answer it:** the flat cells have the weakest sample
  counts in the matrix, so a useful GUI call-tree read needs the signpost-scoped
  interval (a GUI/live recording — the CLI capture got none) to exclude the
  ~83–89 % of process time that is snapshot load + startup. Alternatively a
  RECORD-ONLY flat run at a higher update count and/or a coarser sampler would
  raise the apply share.

## What is intentionally NOT claimed

- No per-function percentage, no "heavy path", no cache-miss / branch / stall
  attribution — none of these is derivable from the headless Release export.
- No number is invented: the fragments/sample facts above are exported from the
  committed traces; the anchors are the recorded `anchor_ns.txt` rows; the
  apply-share column is explicitly a DERIVED estimate.
- The recordings are RECORD-ONLY (`updates=20000000`), never relabeled as the
  Phase 2 methodology, and never used for any per-update counter derivation.

## Next step to upgrade this draft

Open each `recording.trace` in the Instruments GUI and record, per cell: the
top call-tree / heavy-path rows for `MapOrderBook::apply` / `FlatOrderBook::apply`
(and, for E, whether allocator frames appear; for flat C, the rescan path), with
the share of samples in each. For flat + E cells, prefer a GUI/live recording
that captures the `llob.apply.block` signpost so the read can be scoped to the
timed block (the CLI traces carry no signpost). Each such observation is then
labeled OBSERVED IN INSTRUMENTS and this draft's INTERPRETATION rows are
upgraded or replaced accordingly.
