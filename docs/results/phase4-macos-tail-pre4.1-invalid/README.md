# Phase 4 — macOS tail-latency results

> **⚠️ INVALID / NOT canonical — retained as a labeled historical artifact.**
>
> The six cells in this tree were produced with the **pre-Phase-4.1 tooling**,
> which had two P0 reproducibility bugs (both since fixed in
> `orderbook_tail_bench` / `scripts/tail-bench.sh`):
>
> 1. **Each cell ran the benchmark TWICE** — once for `summary.txt`, once for
>    `raw_samples.csv`. The two files therefore describe **two independent
>    latency distributions**, and the summary can NOT be recomputed from its own
>    raw CSV. `scripts/verify-tail-summary.sh` now proves this: run against any
>    cell here it fails on every metric.
> 2. **The trailing 128-update partial batch leaked into the summary's
>    distribution.** `min`/percentiles/`max` could include the partial and the
>    mean numerator included its elapsed time while the denominator still used
>    the full-batch count (e.g. `map_A_1000000` reports `min=71.77` — that value
>    is the 128-op partial's elapsed normalized by 512, not a real full-batch
>    min).
>
> **Do not cite any number from this tree.** Phase 4 canonical measurement is
> **PENDING**: the canonical matrix must be re-measured with the hardened
> tooling before any Phase 4 result is presented. The history is kept here only
> so the pre-fix artifacts remain inspectable; nothing below is a measured claim.

## What this tree is

A historical snapshot of six Phase 4 (tail latency / jitter) runs made with
`orderbook_tail_bench` on the Apple M3 Max / macOS 14.2.1 host that produced the
Phase 2 dataset (`../phase2-m3max/`) and the Phase 3M traces
(`../phase3-macos-apple-silicon/`). Every `raw_samples.csv` here is a real
benchmark output (nothing was invented), but the paired `summary.txt` came from a
*separate* benchmark run and is **not derivable** from that CSV, and its
distribution included the trailing partial batch — so the dataset is **not a
valid canonical Phase 4 dataset** and predates the Phase 4.1 hardening fixes.

## Layout of each cell directory (pre-fix artifacts)

- `summary.txt` — from run #1 of the old (buggy) runner. **Invalid**: distribution
  included the partial batch; not recomputable from the CSV below.
- `raw_samples.csv` — from run #2 of the old (buggy) runner. A real sample of one
  run, but unpaired with a valid summary.
- `command.txt` — the old runner's provenance (omitted `--stats-out`/`--samples-out`).
- `host.txt` — machine/tool/build metadata at run time.
- `notes.md` / `PHASE4_ANALYSIS.md` — written against the invalid summaries; read
  only as an example of the analysis format, never as results.

## Regeneration path

Run the hardened canonical runner and commit the verified cells under a fresh
`docs/results/phase4-macos-tail/` layout (each cell now from ONE invocation,
verified by `scripts/verify-tail-summary.sh`). Then update this README and
`docs/profiling/PHASE4_TAIL_LATENCY.md` with real, recomputable numbers.
