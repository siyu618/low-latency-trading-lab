# Phase 4 — macOS tail-latency results

**Status: EMPTY — no real Phase 4 canonical tail-latency dataset exists yet.**

This tree is the committed home for Phase 4 (tail latency / jitter) measurements
made with `orderbook_tail_bench` on the Apple Silicon macOS host. It is
intentionally **not populated**: no canonical run has been made and reviewed yet.
It is filled only from **real** runs, following
`docs/profiling/PHASE4_TAIL_LATENCY.md`. No number here is ever invented.

## What belongs here after a real run

One subdirectory per profiled cell (e.g. `map_A_1000000/`), each containing:

- `summary.txt` — the benchmark's key:value summary (MEASURED distribution +
  DERIVED percentiles/jitter ratios + final-state validation).
- `raw_samples.csv` — the raw batch samples (source of truth), one row per batch.
- `command.txt` — the exact reproduction command.
- `host.txt` — machine/tool/build metadata at run time.
- `notes.md` — any interpretation, labeled INTERPRETATION, and any LIMITATION.

Plus a top-level `PHASE4_ANALYSIS.md` once real data exists, answering the
tail/jitter questions with this machine's own numbers and keeping every claim
labeled `MEASURED` / `DERIVED` / `INTERPRETATION` / `LIMITATION`.

Transient raw experiments land in repo-root `results/` (git-ignored) and are
moved here only after inspection.
