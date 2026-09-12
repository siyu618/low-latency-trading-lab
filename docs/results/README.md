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
- `spsc-throughput/` — the **canonical Experiment 02 Phase 2 throughput
  baseline**: two-thread end-to-end message-transfer throughput of the frozen
  Phase-1 `SpscRingBuffer` against `MutexBoundedQueue`, 18 cells (2 impls × 3
  message sizes × 3 capacities), 10M messages per repetition, 5 measured
  repetitions per process, 4 sessions per cell in a **balanced AB/BA** order
  (2 mutex-first + 2 SPSC-first), 72 processes, 360 measured repetitions, all
  `correctness=PASS`, measured 2026-09-11 on the Apple M3 Max. `ns/msg` is
  END-TO-END elapsed / messages delivered — never a per-call or one-way handoff
  latency. Implementation direction is taken from the **paired per-session**
  medians (`PAIRED_COMPARISON.md`, `paired_summary.csv`); the pooled matrix is
  secondary. See its `RESULTS_METADATA.md` and `docs/SPSC_THROUGHPUT.md`. **No
  number in it is attributed to false sharing** (Phase 3).
- `spsc-false-sharing/` — the **canonical Experiment 02 Phase 3A controlled
  false-sharing dataset**: the same SPSC algorithm with cursors forced into
  **one** cache line (`same_line`) versus **distinct** cache lines
  (`separated`), 18 cells (2 layouts × 3 message sizes × 3 capacities), 10M
  messages per repetition, 5 measured repetitions per process, 4 sessions per
  cell in a **balanced AB/BA** order (2 same-line-first + 2 separated-first),
  72 processes, one implementation per process. **Cursor cache-line placement is
  the only variable** — no cached remote cursor, no batching, no memory-order
  change, no CAS, no affinity. Layout is established by construction
  (`static_assert` on block size and alignment) **and verified at runtime on
  every measured repetition**: the raw CSVs carry the measured cursor addresses,
  their line indices under the host's *reported* line size, and a `layout_ok`
  verdict. The host reported **128** bytes, not the 64 that is usually assumed —
  a 64-byte assumption would have placed the "separated" blocks in one real line
  and inverted the experiment's meaning, so the benchmark fails the run rather
  than publishing if the host's line size exceeds the compile-time assumption.
  `ns/msg` is END-TO-END elapsed / messages delivered. Direction is taken from
  the **paired per-session** median ratio (`separated ÷ same_line`, `< 1` means
  separated is faster) in `PAIRED_COMPARISON.md`; the pooled matrix is secondary.
  **Result: the direction is cell-dependent, not uniform** — 7 of 9 cells hold
  one direction across all four balanced sessions (5 separated-faster, the
  largest being 4.0× at 8 B / 1024; 2 same-line-faster, the largest 1.61× at
  32 B / 4096) and 2 cells are inconclusive. No general "padding is faster"
  claim is supported by this dataset.
  See its `RESULTS_METADATA.md`, `LAYOUT_VERIFICATION.md`, `invariants.txt` and
  `docs/SPSC_FALSE_SHARING.md`. The frozen
  `spsc-throughput/` dataset is unchanged and is **not** a Phase-3A control —
  Phase 2 verified no cursor addresses, so it cannot be one.
- `spsc-throughput-superseded-fixed-order/` — the second Phase-2 pass,
  **SUPERSEDED, not canonical**. Real and self-validating (all 54 processes
  `PASS`), but every session ran all nine mutex cells before all nine SPSC
  cells, so implementation was perfectly confounded with position in time; it
  also predates the corrected consecutive-miss yield policy. Do not cite it for
  any mutex-vs-SPSC comparison. Retained as labelled real data and as the
  evidence for the balanced AB/BA design. See its `SUPERSEDED.md`.
- `spsc-throughput-superseded-single-session/` — the first Phase-2 pass (one
  process per cell), **SUPERSEDED, not canonical**. Real and self-validating,
  but it samples only one thread/queue placement per cell, and the SPSC cells
  turned out to be strongly bimodal under that sampling. Retained as a labeled
  historical artifact and as the evidence for adopting pooled multi-session
  measurement. See its `SUPERSEDED.md`; cite it only for that purpose.

## Honesty rule

Never invent numbers. A dataset enters `docs/results/` only from a real run,
with provenance (machine, compiler/flags, perf version, the raw tool output,
the exact command) recorded next to it. Illustrative output elsewhere in the
repo is always labeled as such.
