# Experiment 02 Phase 4 — results metadata

Tail latency / jitter of the **frozen separated-cursor baseline SPSC** ring
buffer, measured on the Apple M3 Max with **sparse timestamp instrumentation**.
This dataset characterizes **one** queue configuration; it compares no
treatments.

It supersedes `spsc-tail-latency-superseded-per-message-timestamp/`, which
timestamped every message. See that directory's `SUPERSEDED.md`.

## Invariants

Every line below was **checked by the run**, not assumed. The verifier's own
output is preserved verbatim as `invariants.txt`:

```
Phase 4 raw -> summary verification
  results dir  : docs/results/spsc-tail-latency
  raw files    : 36
  summary files: 36

  verified repetitions : 180
  sampled latencies    : 1745280
  checks run           : 5240804

ALL PHASE 4 RAW -> SUMMARY CHECKS PASSED
```

| invariant | value | checked by |
|---|---|---|
| cells | 9 (3 message sizes × 3 capacities) | runner |
| sessions | 4 (forward / reverse / forward / reverse) | runner |
| processes | 36 (one `(cell, session)` per process) | runner |
| messages per repetition | 10,000,000 | runner |
| settling prefix | 100,000 | runner |
| sample interval | 1021 (odd; coprime with every power-of-two capacity) | runner |
| expected samples per repetition | **9,696** (DERIVED: ⌊(10,000,000−100,000)/1021⌋) | runner + verifier |
| `sample_count == expected_samples` | every repetition | verifier |
| `producer_sample_clock_reads == 9,696` | **every repetition** | verifier |
| `consumer_sample_clock_reads == 9,696` | **every repetition** | verifier |
| `stamp_contract_failures == 0` | every repetition | verifier |
| measured repetitions per process | 5 | verifier |
| excluded warm-up repetitions per process | 1 | runner |
| measured repetition distributions | 180 | verifier |
| sampled latencies | 1,745,280 (= 180 × 9,696 exactly) | verifier |
| raw / summary / calibration files | 36 / 36 / 36 | verifier |
| implementations in matrix | 1 (separated-cursor baseline; no cached, no mutex, no `same_line`) | runner |
| correctness | PASS on every repetition (delivery, strict sequence, payload validator) | benchmark |
| timestamp inversions | 0 on every repetition | verifier |
| checksum identical across a cell's 20 repetitions | VERIFIED per message size | verifier |
| cursor layout verified on the measured object | every process | benchmark (stderr trace) |
| summary vs raw | VERIFIED for every repetition | verifier |
| raw→summary checks run / failed | 5,240,804 / **0** | verifier |

The four instrumentation lines are the point of Phase 4.1: they are what makes
"the clock reads were sparse" a **checkable property of the dataset** rather than
a claim about the source. A repetition that took a per-message clock read reports
≈10,000,000 in the read counters and **fails**, instead of quietly producing the
same 9,696 retained latencies.

## Files

| path | contents |
|---|---|
| `command.txt` | every effective invocation, in execution order, with the measurement contract stated at the top |
| `raw/` | one raw per-sample CSV per process (36 files, 1,745,280 rows total) |
| `summaries/` | one per-repetition summary per process (36 files, 180 measured repetitions) |
| `calibration/` | the timer calibration of each process (200,000 clock pairs each) |
| `stderr/` | the runtime layout-verification trace of each process |
| `run_order.txt` | the execution order parsed back out of `command.txt` |
| `SESSIONS.md` | the session design and the cell order within each session |
| `HOST.md` | host, toolchain, flags, cache-line size, load average, and the **source digests of the exact files the binary was built from** |
| `CELL_SESSION_BLOCKED.csv` | **DERIVED, PRIMARY** per-cell summary: the blocked median plus the min/max session median it sits inside, and the all-20 median beside it as a labelled diagnostic |
| `CELL_TAIL.csv` | **DERIVED** per-cell, per-session aggregate (median of each repetition-level statistic) |
| `TAIL_MATRIX.md` | **DERIVED** the same as a readable matrix, with the two aggregation levels shown separately |
| `TAIL_RATIOS.csv` | **DERIVED** per-repetition tail ratios (P99/P50, P99.9/P50, max/P50) |
| `invariants.txt` | the passing raw→summary verification output |
| `PROVENANCE.md` | how the dataset was produced and how to re-verify it |

Everything under `raw/`, `summaries/`, `calibration/` and `stderr/` is
**MEASURED**. The four derived files are computed from `summaries/*.csv` by
`scripts/analyze-spsc-tail.py` and are never edited by hand; the script refuses
to run unless `invariants.txt` records a passing verification.

## The measured quantity

`producer_ready → consumer_received` on `std::chrono::steady_clock`: stamped
immediately **before** the producer's `try_push` retry loop and immediately
**after** a successful `try_pop`, before any validation. It **includes** the
producer's backpressure wait, the release/acquire synchronization, the payload
copy, the consumer's empty-retry loop and both clock reads.

It is **not** pure queue residence time, **not** a per-call queue cost and
**not** a one-way handoff latency, and it is **not comparable** to the `ns/msg`
figures in `spsc-throughput/`, `spsc-false-sharing/` or `spsc-remote-cursor/`
(different message shapes, different quantity).

Because the stamp precedes the push, the interval depends on how far ahead of
the consumer the producer has run, so it is a **producer-relative service
latency including backpressure**. See `docs/SPSC_TAIL_LATENCY.md`.

**The stamp travels inside the sampled message**, in the message's own
`ready_ticks` field, along the same SPSC payload path the payload takes. There is
no side array, no map and no shared metadata structure, and therefore no second,
capacity-dependent memory footprint in the measured path. The superseded dataset
kept stamps in a separate `ready_ticks[2 × Capacity]` array; that is the
instrumentation change Phase 4.1 makes, and it is why these numbers replace those.

## How these numbers are aggregated

The design is nested — 5 measured repetitions inside each of 4 sessions per cell
— so a cell's statistic has two defensible definitions:

- **PRIMARY — session-blocked**: repetition → median of the session's 5
  repetitions → **median of the 4 session medians**.
- **SECONDARY — all 20**: median across all 20 repetition-level statistics. A
  **diagnostic only**.

A median of medians is **not** a median of all values: with 4 sessions the
blocked median weights each session equally and the all-20 median weights each
repetition equally. Where they differ, the PRIMARY column is the cell's value.
`CELL_SESSION_BLOCKED.csv` carries both plus the min/max session medians; the
differences are small for P50 (0.0% in six cells) and material for max (−37% and
+18% in either direction), which is itself the reason the two are labelled.

## Result

**Sparse instrumentation removes the instability, and leaves a sharper, entirely
stable bimodality.**

Across 180 measured repetitions of nine fixed configurations (a fixed binary, a
fixed cell, 10M messages, 5 consecutive repetitions per process):

- **The nine cells split into two clean groups.** Six cells — all 16 B and 32 B —
  have a session-blocked **P50 of 125 ns**, and that P50 is **constant at exactly
  125 ns in all 120 of their repetitions** (spread 1.00×). The three 64 B cells
  sit at **30.0 µs / 120.4 µs / 1.95 ms**, one per capacity, stable to 1.05–1.09×
  across their four sessions.
- **P50 is now the most reproducible statistic, not the least.** Worst-case P50
  spread over a cell's 20 repetitions is 1.11×, against 6.0× for P90, 13,469× for
  P99 and 19,271× for max. This **reverses** the superseded dataset's ordering.
- **The fast band is timer-resolution-limited.** The clock quantum is ≈41.7 ns
  and 125 ns is three quanta. In the six fast cells **92–95%** of samples lie
  within four quanta of zero and **53–72%** are exactly 125 ns; in the three slow
  cells, 0.00% are within a microsecond.
- **The two groups differ in which thread waits.** In the fast cells the consumer
  spins empty 0.93–2.5 **billion** times while the producer almost never finds the
  ring full; in the 64 B cells the producer is blocked 26–204 **million** times
  and the consumer spins empty only 12 thousand to 3.9 million times.
- **Within the slow band, larger capacity means fewer producer stalls but longer
  measured latency.** P50 is 94–97% of one full ring's drain time
  (`capacity × ns_per_message`) in all three: 0.937, 0.946, 0.965.
- **Extreme maxima are largely isolated, with one cell where they are not.**
  In seven of nine cells 1–3 of 20 repetitions exceed 5× the cell's median
  maximum. In **32 B / 65536** it is **7 of 20**, including three consecutive
  repetitions inside a single process (1.85 ms, 3.62 ms, 20.4 ms). Reported and
  **not censored**: magnitude alone is not proof of invalidity, and by the H2 rule
  those repetitions are named contamination *candidates*, not verdicts. The
  verifier raised **no warning** on this dataset — worst cell `ns_per_message`
  spread 3.01×, under the 5× diagnostic threshold.
- **Tail ratios are not comparable across cells** whose medians sit in different
  bands: P99.9/P50 ranges 47–163× in the fast cells and 2.1–3.4× in the slow ones,
  driven by the *floor*, not by the tails.

Correctness: all 180 repetitions delivered every message in strict sequence with
every payload passing its validator, **0** timestamp inversions, **0** stamp
contract failures, and all **5,240,804** raw→summary checks passed.

## What this dataset does not support

- **No causal attribution of any kind.** Nothing here is profiled. No
  cache-miss, coherence-event, cache-line-transfer, preemption, scheduler,
  core-migration, P-core/E-core, frequency or thermal quantity was measured, and
  none may be inferred from a latency value.
- **No claim that the 64 B cells are slow *because of* anything.** The drain-time
  relationship in Q4 is a hypothesis the numbers are consistent with, and
  producer lead is not instrumented; it is not confirmed here.
- **No "the latency is the timer, not the queue."** The fast band is
  timer-resolution-limited — the queue is doing real work at a scale this clock
  bounds but does not resolve.
- **No ranking of cells**, and no "best" or "worst" configuration: the matrix
  contains one queue configuration, and the two bands differ in which thread is
  the bottleneck.
- **No comparison with Phase 2, 3A or 3B**, absolutely or as a ratio.
- **No claim that any level is a property of the queue alone**, because the
  contract includes backpressure and therefore depends on producer lead.
- **No claim about `MutexBoundedQueue`, the `same_line` layout or the cached
  remote-cursor variant** — none is in this matrix.
- **No number carried over from the superseded dataset.** Every figure here is
  regenerated from these files, and its findings are not assumed to reproduce.
