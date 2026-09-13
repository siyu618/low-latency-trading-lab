# Experiment 02 Phase 4 — results metadata

Tail latency / jitter of the **frozen separated-cursor baseline SPSC** ring
buffer, measured on the Apple M3 Max. This dataset characterizes **one** queue
configuration; it compares no treatments.

## Invariants

```
# Experiment 02 Phase 4 — dataset invariants
# Every line below was CHECKED, not assumed. Any violation aborted the run.
cells=9 (3 message sizes x 3 capacities)
sessions=4 (forward / reverse / forward / reverse)
processes=36 (one (cell, session) per process)
messages_per_repetition=10000000
settling_prefix=100000
sample_interval=1021 (odd, coprime with every power-of-two capacity)
expected_samples_per_repetition=9696 (DERIVED: (10000000-100000)/1021)
measured_repetitions_per_process=5
excluded_warmup_repetitions_per_process=1
measured_repetition_distributions=180
sampled_latencies=1745280 (= 180 x 9696 exactly)
raw_csv_files=36
summary_csv_files=36
calibration_files=36
implementations_in_matrix=1 (separated-cursor baseline; NO cached, NO mutex, NO same_line)
correctness=PASS on every repetition (delivery, strict sequence, payload validator)
timestamp_inversions=0 on every repetition
sample_count_matches_derived=every repetition
checksum_identical_across_all_20_repetitions_per_message_size=VERIFIED
cursor_layout_verified_on_the_measured_object=every process
summary_vs_raw_verification=VERIFIED for every repetition
raw_to_summary_checks_run=5240255
raw_to_summary_checks_failed=0
```

The verifier's own output is preserved verbatim as `invariants.txt`.

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
| `CELL_TAIL.csv` | **DERIVED** per-cell, per-session aggregate (median of each repetition-level statistic) |
| `TAIL_MATRIX.md` | **DERIVED** the same as a readable matrix, plus the per-cell P50 and P99 spread |
| `TAIL_RATIOS.csv` | **DERIVED** per-repetition tail ratios (P99/P50, P99.9/P50, max/P50) |
| `invariants.txt` | the passing raw→summary verification output |
| `PROVENANCE.md` | how the dataset was produced and how to re-verify it |

Everything under `raw/`, `summaries/`, `calibration/` and `stderr/` is
**MEASURED**. The three derived files are computed from `summaries/*.csv` by
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

## Result

**The reproducibility of a statistic is not the same for every statistic, and
the median is among the least reproducible.**

Across 180 measured repetitions of nine fixed configurations (a fixed binary, a
fixed cell, 10M messages, 5 consecutive repetitions per process):

- **P50 and P90 are not reproducible properties of a cell.** Five consecutive
  repetitions of an identical configuration produced P50s spanning **379×**
  (32 B / 4096) and P90s spanning **442×** (16 B / 4096, across the cell's 20
  repetitions). A cell-level P50 or P90 describes *the runs that happened*.
- **P99 is comparatively reproducible**, and most so exactly where P50 is least
  reproducible: in 32 B / 4096 the P50 spread is 379× while the **P99 spread is
  1.08×**. Across all nine cells, P99 spread never exceeded **5.00×** and was
  ≤ 2.43× in six.
- **End-to-end throughput is stable where the latency distribution is not.**
  `ns_per_message` varies by only **1.12×–1.70×** per cell across the same 20
  repetitions in which the P50 moves up to 709×.
- **The fast mode is the timer, not the queue.** 81 of 180 repetitions have
  P50 < 1 µs, and those P50s take only **ten distinct values** in the whole
  dataset (84, 125, 167, 291, 292, 333, 375, 417, 500, 625 ns) — each within
  1 ns of a whole multiple of the 41.7 ns quantum the independent calibration
  reports. The floor is three quanta wide and is not resolvable below that.
- **Capacity and message size do not determine the mode.** At capacity 4096 the
  three message sizes land in three different bands; at 1024 all are fast; at
  65536 all are slow.
- **Tail ratios (P99/P50) are not comparable across cells** whose medians sit in
  different bands: that ratio ranges 394–1268 in the fast-band cells and 1.8–2.4
  in the slow-band cells, driven by the *denominator*, not by the tails.

Correctness: all 180 repetitions delivered every message in strict sequence with
every payload passing its validator, **0** timestamp inversions, and all
**5,240,255** raw→summary checks passed.

## What this dataset does not support

- **No causal attribution of any kind.** Nothing here is profiled. No
  cache-miss, coherence-event, cache-line-transfer, preemption, scheduler,
  core-migration, P-core/E-core, frequency or thermal quantity was measured, and
  none may be inferred from a latency value.
- **No ranking of cells**, and no "best" or "worst" configuration: a fast median
  and a slow median were observed from the *same* configuration on different
  repetitions, so a table ordered by P50 orders the runs, not the ring.
- **No comparison with Phase 2, 3A or 3B**, absolutely or as a ratio.
- **No claim that any level is a property of the queue alone**, because the
  contract includes backpressure and therefore depends on producer lead.
- **No claim about `MutexBoundedQueue`, the `same_line` layout or the cached
  remote-cursor variant** — none is in this matrix.
