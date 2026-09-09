# docs/results — committed measurement datasets

This directory holds only **real, committed** measurement data, organized by
measurement family. Transient raw runs land in repo-root `results/`
(git-ignored) and are moved here only after inspection.

## Layout

- `phase2-m3max/` — the Phase 2 canonical **throughput** dataset: steady-state
  `apply()` ns/update across both books, five workloads, four scales, measured
  on the Apple M3 Max dev machine. Frozen, independent — used only for
  high-level cross-platform trend comparison, never as the absolute latency
  paired with Linux perf counters. See its `RESULTS_METADATA.md`.
- `phase3-macos-apple-silicon/` — Phase 3M **Apple Instruments** observations
  (Time Profiler) on the same Apple M3 Max host that produced the Phase 2
  dataset, read against that same-host ns/update. Phase 3M tooling COMPLETE and
  six real recordings COLLECTED; per-function call-tree / attribution analysis
  is still DEFERRED — an Instruments GUI pass over those recordings (see that
  README and `docs/profiling/MACOS_INSTRUMENTS.md`).
- `phase3-linux-<machine>/` — Phase 3L **Linux perf** counter data (one
  subdirectory per profiled cell) **plus a same-host Linux throughput
  baseline** from that machine/compiler/build — Linux counters are read against
  Linux latency, never against the M3 Max CSV. Not yet present — Phase 3L
  tooling is READY, but native Linux PMU data is DEFERRED (no Linux host); see
  `docs/profiling/README.md`.
- `phase4-macos-tail/` — the **canonical** Phase 4 **tail-latency / jitter**
  dataset from the Apple M3 Max (measured 2026-09-09 with the hardened Phase 4.1
  tooling: one invocation per cell, trailing partial batch excluded from every
  distribution metric, each summary re-verified from its own raw CSV). See its
  `RESULTS_METADATA.md`, `README.md`, and `PHASE4_ANALYSIS.md`.
- `phase4-macos-tail-pre4.1-invalid/` — the pre-Phase-4.1 cells from the buggy
  tooling (two independent runs per cell; trailing partial batch leaked into the
  summary). **INVALID / NOT canonical**, retained only as a labeled historical
  artifact — never cite it. See `docs/profiling/PHASE4_TAIL_LATENCY.md`.
- `orderbook-bitmap-optimization/` — the Experiment 01 Optimization Study
  dataset (post-Phase-4, internal): `BitsetFlatOrderBook` (hierarchical
  occupancy bitmap) vs the frozen `FlatOrderBook` — steady throughput on the
  A–E workloads at 1M, a controlled best-delete gap ladder sweep, per-workload
  best-delete re-scan-distance analysis, and occupancy-vs-quantity memory
  accounting, all on the same Apple M3 Max. It was credibility-hardened with a
  **no-bitmap control** (`TransitionAwareFlatOrderBook`): the three-impl
  isolation steady data live under `control-isolation/`, and the gap-crossover
  claim was hardened across 16 independent rounds (variable- and
  `--fixed-domain`) under `control-isolation/gap-crossover/` and
  `fixed-domain-gap-validation/`. None of it is a Phase 2/4 canonical dataset;
  it is read against them. See its `README.md` and
  `docs/ORDERBOOK_BITMAP_OPTIMIZATION.md`.

## Honesty rule

Never invent numbers. A dataset enters `docs/results/` only from a real run,
with provenance (machine, compiler/flags, perf version, the raw tool output,
the exact command) recorded next to it. Illustrative output elsewhere in the
repo is always labeled as such.
