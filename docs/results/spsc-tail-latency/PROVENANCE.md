# Experiment 02 Phase 4 — provenance

Collected **2026-09-13**, 14:34:06 → 14:35:42 (96 s of measured wall time), on
the Apple M3 Max development host described in `HOST.md`.

## Repository state

```
HEAD: 6924992bac27510997a4497c23ad126527063f61
```

### `git status --short` at collection time

```
 M CMakeLists.txt
?? benchmark/spsc_tail_harness.h
?? benchmark/spsc_tail_latency_bench.cpp
?? docs/SPSC_TAIL_LATENCY.md
?? docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/
?? docs/results/spsc-tail-latency/
?? scripts/analyze-spsc-tail.py
?? scripts/spsc-tail-latency.sh
?? scripts/verify-spsc-tail-summary.py
?? tests/spsc_tail_latency_tests.cpp
```

**The tree was dirty, and the recorded HEAD does not contain the benchmark.**
Every Phase-4 file was untracked at collection time. HEAD `6924992` is the
Phase-3B commit; `benchmark/spsc_tail_latency_bench.cpp` did not exist in it.
This is why `HOST.md` records the **digest of each source file as it was on disk
at run time** — those digests, not the commit id, are the authoritative record of
what was compiled. A future re-run from a clean commit is only comparable to
this dataset if these digests match.

## Source hashes

The digests below are what was on disk at run time. All six were **re-verified
against the working tree after collection and still match**:

```
dacfd7d4ea7c6a9bb2f474c5d1c22a9712c71628bc3d9b00f2767501903d740b  benchmark/spsc_tail_latency_bench.cpp
cffa0cf9675dc6752b7ce4ebe6f876a0d3037f1b5fc3e5f735a82828480b5404  benchmark/spsc_tail_harness.h
e5c2e8e172e9985904e4cc3b282824f6843d1e07286c0f2faef5e23774138f01  tests/spsc_tail_latency_tests.cpp
6ec3603cd9d7aa1dbc9ddc261d5499aa28a60c9a000378f3e53c492b8b6d8957  scripts/spsc-tail-latency.sh
8d300cf429a09c7c7a3edc8352881656101b7c69c96dfb148824835bc5eaa4f1  scripts/verify-spsc-tail-summary.py
ed4855223270e483128a4b8833df7d815b0b2b4bca9ffba1387b433918e8e96a  include/spsc_remote_cursor_ring_buffer.h
```

Note what is **not** in this list: `include/spsc_remote_cursor_ring_buffer.h` is
the frozen phase-3 header and its digest is recorded to show it was **unmodified**
— Phase 4 changes no queue implementation. `scripts/analyze-spsc-tail.py` is a
post-hoc consumer of `summaries/*.csv` and cannot affect the data; it is
deliberately not pinned to the run.

## Binary

```
7aa089fa819124adbab52518daa6573dddadf7388f50a4793d4c8794521829ea  build-spsc-tail/spsc_tail_latency_bench  (115816 bytes)
```

**This hash was recorded after collection, not at run time** — the runner pinned
the sources, not the executable, so it is weaker evidence than the digests above
and is labelled accordingly. The evidence that it is the producing binary:

- Built `2026-09-12 23:41:09`, i.e. **before** the earliest raw file
  (`2026-09-13 14:34:06`).
- Both sources are older (`benchmark/spsc_tail_harness.h` 22:22:47,
  `benchmark/spsc_tail_latency_bench.cpp` 23:22:50), so the runner's fresh
  `cmake --build` had nothing to recompile or relink and left the file
  untouched — consistent with the binary's mtime surviving the run.
- Nothing has rebuilt it since: the mtime is still 09-12 23:41.
- It contains **no** AddressSanitizer / UndefinedBehaviorSanitizer /
  ThreadSanitizer symbols, so the canonical numbers did not come from a
  sanitized build (the sanitizer sweeps used separate build directories).

A rebuild from the recorded sources with the recorded compiler and flags is
expected to reproduce the dataset's behaviour but **not** byte-identical
latencies; see "Stability" below.

## Exact commands

Every effective invocation, in the order it ran, is recorded verbatim in
`command.txt`. The collection was driven by:

```
scripts/spsc-tail-latency.sh
```

which configures and builds `build-spsc-tail/` as Release with forced
`-O3 -DNDEBUG`, runs the 36 processes, then runs the raw→summary verifier and
writes its output to `invariants.txt`.

## How to re-verify this dataset

```
scripts/verify-spsc-tail-summary.py docs/results/spsc-tail-latency
scripts/analyze-spsc-tail.py       docs/results/spsc-tail-latency
```

The first recomputes every percentile, the mean, the count and the checksum
relationships from `raw/*.csv` and compares them to `summaries/*.csv`; it must
print `ALL PHASE 4 RAW -> SUMMARY CHECKS PASSED` and a check count of
**5,240,255**. The second regenerates the three derived files and **refuses to
run** unless `invariants.txt` records that passing verification.

The queue-level correctness of the harness itself is covered by CTest
(`spsc_tail_latency_tests` plus the benchmark's smoke and rejection tests); the
canonical numbers come only from the non-sanitized Release build above.

## Stability

This dataset is a set of **runs**, not a constant. Repeat the collection and the
per-cell P50 and P90 will move — in some cells by two orders of magnitude — while
P99 and `ns_per_message` stay within a few percent. That variability is the
phase's result, not a defect in the collection. Do not treat the P50 or P90 in
`CELL_TAIL.csv` as reproducible constants, and do not compare them against a
re-run as though a difference were a treatment effect.

## What was NOT done

- **No frozen queue implementation was modified.** `include/` is untouched by
  this phase; the digest above is recorded as evidence.
- **No Phase-2, Phase-3A or Phase-3B dataset or result file was read, modified
  or regenerated.** Their directories are byte-identical.
- **No treatment comparison was performed.** The matrix contains exactly one
  queue configuration; the cached variant, `MutexBoundedQueue` and the
  `same_line` layout are absent by design.
- **No new optimization was introduced.** Phase 4 is a measurement phase.
- No number in this dataset is compared with a Phase-2/3A/3B `ns/msg` figure,
  and no latency value is attributed to a cache, coherence, scheduler, core-type
  or frequency cause.

## One run of this experiment was discarded

An earlier complete 36-process run — every gate passing, all 5,240,246
verification checks green — was measured **concurrently with other work on the
same host** and is therefore not usable. It is retained, unedited and clearly
labelled, at `docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/`,
with its derived aggregate tables **deleted** so no quotable summary of it
survives. That directory is deliberately **not committed** — 106 MB of raw data
for a run that cannot be used — so a clone will contain neither it nor its
`SUPERSEDED.md`; the repository keeps this paragraph and the "Two hazards"
section of `docs/SPSC_TAIL_LATENCY.md` instead. Every check count and cell figure
in the canonical dataset above belongs to the clean re-run only.
