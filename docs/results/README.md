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
  dataset, read against that same-host ns/update. **Populated**: six real
  recordings committed; per-function call-tree symbolization still PENDING an
  Instruments GUI pass (see that README and `docs/profiling/MACOS_INSTRUMENTS.md`).
- `phase3-linux-<machine>/` — Phase 3L **Linux perf** counter data (one
  subdirectory per profiled cell) **plus a same-host Linux throughput
  baseline** from that machine/compiler/build — Linux counters are read against
  Linux latency, never against the M3 Max CSV. Not yet present — Phase 3L
  native Linux measurement is DEFERRED (no Linux host); see
  `docs/profiling/README.md`.
- `phase4-macos-tail/` — Phase 4 **tail-latency / jitter** artifacts from the
  Apple M3 Max. **INVALID / NOT canonical** — produced by the pre-Phase-4.1
  tooling (two independent runs per cell; trailing partial batch leaked into the
  summary). Retained only as a labeled historical artifact; **Phase 4 canonical
  measurement is PENDING** — re-measure with the hardened runner
  (`scripts/tail-bench.sh`), which issues ONE invocation per cell and verifies
  summary-from-raw via `scripts/verify-tail-summary.sh`. See that tree's
  `README.md` and `docs/profiling/PHASE4_TAIL_LATENCY.md`.

## Honesty rule

Never invent numbers. A dataset enters `docs/results/` only from a real run,
with provenance (machine, compiler/flags, perf version, the raw tool output,
the exact command) recorded next to it. Illustrative output elsewhere in the
repo is always labeled as such.
