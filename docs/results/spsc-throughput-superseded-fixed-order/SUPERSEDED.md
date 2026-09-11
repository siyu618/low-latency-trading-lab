# SUPERSEDED — fixed implementation run order

> ## REAL DATA
> ## SUPERSEDED FOR COMPARATIVE CLAIMS

**Do not cite these numbers for any mutex-vs-SPSC comparison.** The canonical
Phase-2 dataset is `../spsc-throughput/`.

## What this run is

A complete, real, self-validating pass over the 18-cell matrix
(2 implementations x 3 message sizes x 3 capacities) at `message_count=10000000`,
`reps=5`, `warmup=1`, `SESSIONS=3`, run 2026-09-11 with one implementation per
process. Every one of the 54 processes reported `correctness=PASS`, every cell's
checksum is identical across all of its repetitions, and every summary was
re-verified against its own raw per-repetition CSV before the matrix was
derived. Nothing here is fabricated, estimated, or edited; no repetition was
discarded.

The raw numeric content of `raw/` is **frozen and unedited**.

## Why it was superseded

**The runner executed all nine mutex cells before all nine SPSC cells, in every
session.** Implementation was therefore perfectly confounded with run position
in time. On an unpinned macOS host where scheduler placement, thread migration,
P/E-core assignment, DVFS, thermal state and background load all drift over the
course of a run, a systematic `all-A-then-all-B` order means an implementation
difference and a time-of-run difference are indistinguishable in the result.

The hardened runner fixes this by running the two implementations of the same
`message_bytes + capacity` pair as **adjacent processes**, with implementation
order balanced AB/BA across four sessions:

| session | traversal | implementation order |
|---|---|---|
| 1 | forward | mutex → spsc |
| 2 | reverse | spsc → mutex |
| 3 | forward | spsc → mutex |
| 4 | reverse | mutex → spsc |

so every bytes/capacity pair gets 2 mutex-first and 2 spsc-first comparisons,
and the cell-position/time-order axis is balanced by the forward/reverse
traversal.

A second, independent reason for re-running is that the retry/yield policy was
also hardened: the harness previously accumulated misses across successful
operations, so a `yield()` did not necessarily follow 1024 *consecutive*
failures as documented. That changes scheduler interaction, so every canonical
performance number had to be regenerated rather than patched.

## What remains valid here

- The **correctness** evidence: sequence, checksum and delivery validation are
  unaffected by run order or the yield policy.
- The **retry counts**, as measurements of how often each queue failed a
  `try_push`/`try_pop`, though their absolute values shift with the yield policy.
- The **evidence about run-to-run variability** that motivated the balanced
  design: this dataset is what showed that identical binaries on this host can
  produce medians differing by ~2x, and that the SPSC cells are the unstable
  ones.

## Contents

- `raw/<impl>_b<bytes>_c<capacity>_s<session>.csv` — 54 files, 5 measured rows each
- `summaries/<...>.txt`, `stderr/<...>.txt` — per-process summary and progress log
- `summary.csv`, `MATRIX.md`, `SESSIONS.md` — derived matrix (pooled over 3 sessions)
- `command.txt` — the exact invocations, in the order they ran
- `HOST.md` — host and toolchain metadata
- `RESULTS_METADATA.md` — provenance as published at the time
