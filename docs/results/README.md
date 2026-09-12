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
  cursor-placement dataset** (measured 2026-09-12 under the Phase-3A.1
  equal-footprint design): the same SPSC algorithm with cursors forced into
  **one** cache line (`same_line`) versus **distinct** cache lines
  (`separated`), 18 cells (2 layouts × 3 message sizes × 3 capacities), 10M
  messages per repetition, 5 measured repetitions per process, 4 sessions per
  cell in a **balanced AB/BA** order (2 same-line-first + 2 separated-first),
  72 processes, one implementation per process. **Cursor cache-line placement is
  the only program-layout treatment** — no cached remote cursor, no batching, no
  memory-order change, no CAS, no affinity — **and, since Phase 3A.1, both cursor
  policies have the same `2 × 128 = 256`-byte footprint**, so the payload array
  keeps the same *relative* offset within the object in both variants. Same-line
  keeps *both* cursors in the first block and reserves an inert second block whose
  only purpose is to equalize the footprint; the earlier 128-vs-256-byte design
  shifted the payload's relative offset by 128 bytes along with the cursor
  placement — an uncontrolled object-layout change — which is why its dataset was
  superseded. The equality is of the relative offset: the legs run as separate
  processes with independently allocated objects, so absolute addresses and the
  actual hardware cache-set mapping remain uncontrolled and unmeasured. Layout is
  established by construction (`static_assert` on policy size, alignment and
  cursor offset) **and verified at runtime**: a pre-timing gate aborts the process
  if the two variants' `object_size` / `payload_offset_from_object_base` disagree,
  and **every measured repetition** records the measured cursor addresses, their
  line indices under the host's *reported* line size, the object address/size and
  payload offset, and a `layout_ok` verdict. The host reported **128** bytes, not
  the 64 that is usually assumed — a 64-byte assumption would have placed the
  "separated" blocks in one real line and inverted the experiment's meaning, so
  the benchmark fails the run rather than publishing if the host's line size
  exceeds the compile-time assumption. `ns/msg` is END-TO-END elapsed / messages
  delivered. Direction is taken from the **paired per-session** median ratio
  (`separated ÷ same_line`, `< 1` means separated is faster) in
  `PAIRED_COMPARISON.md`; the pooled matrix is secondary.
  **Result: the direction is cell-dependent, not uniform** — 6 of 9 cells hold
  one direction across all four balanced sessions (**5 separated-faster**, the
  largest being ~4.0× at 8 B / 1024 and ~1.6× at 64 B / 4096 and 64 B / 65536;
  **1 same-line-faster**, 1.43× at 32 B / 4096) and 3 cells are inconclusive
  (32 B / 1024, 32 B / 65536, 64 B / 1024). No general "padding is faster" claim
  is supported by this dataset, and separated-faster cells are described as
  *consistent with* reduced false-sharing interference, never as proof that false
  sharing caused the whole measured difference. Verified: 72 processes, 360
  measured repetitions, `correctness=PASS` and `layout_ok=PASS` on every row,
  equal `object_size` and equal `payload_offset` across the two layouts in all 9
  cells, `all_invariants=PASS`.
  See its `RESULTS_METADATA.md`, `LAYOUT_VERIFICATION.md`, `invariants.txt`,
  `PROVENANCE.md` and `docs/SPSC_FALSE_SHARING.md`. The frozen
  `spsc-throughput/` dataset is unchanged and is **not** a Phase-3A control —
  Phase 2 verified no cursor addresses, so it cannot be one.
- `spsc-remote-cursor/` — the **canonical Experiment 02 Phase 3B remote
  cursor-caching dataset** (measured 2026-09-12): the Phase-3A **separated**
  layout only, with a thread-owned non-atomic cached copy of the remote cursor
  (`direct` = `baseline` reads the remote cursor on every attempt; `cached`
  refreshes it only when the cached value cannot prove progress is safe), 18
  cells (2 variants × 3 message sizes × 3 capacities), 10M messages per
  repetition, 5 measured repetitions per process, 4 sessions per cell in a
  **balanced AB/BA** order (2 baseline-first + 2 cached-first), 72 processes, one
  implementation per process. **Remote cursor refresh frequency is the only
  treatment** — no batching, no memory-order change, no CAS, no MPSC/MPMC, no
  affinity. The release/acquire publication edge exists in **both** variants and
  was not weakened; a stale cached value can only cause a false full or a false
  empty, never a reused slot or a read of unpublished data. Cursor placement,
  object footprint (`2 × 128 = 256` bytes of cursor state), payload offset,
  capacity, indexing, retry/yield policy and message types are identical by
  construction, enforced by `static_assert` plus a pre-timing runtime gate, and
  **every measured repetition** records the cursor and cached-value addresses,
  their line indices and both a `layout_ok` and a `cached_placement_ok` verdict
  (host-reported line size **128**). The canonical throughput figures come only
  from `--instrument=0` processes; remote-load counting lives in a **separate
  18-process `--instrument=1` leg** (`mechanism/`, `MECHANISM.md`) whose counters
  are thread-owned and whose throughput numbers are **not** the canonical result
  and must not be quoted as such. `ns/msg` is END-TO-END elapsed / messages
  delivered. Direction is taken from the **paired per-session** median ratio
  (`cached ÷ baseline`, `< 1` means cached is faster) in `PAIRED_COMPARISON.md`;
  the pooled matrix is secondary.
  **Result: negative — the mechanism worked and the performance did not follow.**
  Remote cursor loads fell by up to **~44,910×** on one side of the transfer, and
  the reduction is **one-sided**: at 8 B / 32 B the producer's loads collapse (up
  to ~70,102×) while the consumer's rise, and at 64 B the reverse (consumer up to
  ~44,910×, producer rising ~3×). Meanwhile the cached variant was **slower in 6
  of 9 cells, stably across all four balanced sessions, by 1.09×–2.00×**, and 3
  cells (all at 32 B) are inconclusive. 32 of 36 paired observations favour the
  baseline; **no cell is stably cached-faster**. Cells exist where loads fall
  dramatically and throughput nonetheless degrades — 8 B / 4096 producer loads
  fall ~57× with throughput **2.005× worse**, the largest penalty in the dataset.
  Caching cannot help a thread that keeps finding the queue genuinely full or
  genuinely empty, because the real remote cursor has not moved; seven cells show
  the cached variant doing *more* remote loads on one side. No cache-miss,
  coherence-transaction or cache-line-transfer count was measured, and none is
  claimed. **These `ns/msg` levels are NOT comparable to Phase 3A's**: the cell
  shape is strongly bimodal on this host and the compiled image's code placement
  selects the regime, so both Phase-3B variants share one binary and therefore one
  regime — the comparison is internally valid, the level is not portable.
  Verified: 72 canonical processes, 360 measured repetitions, `correctness=PASS`,
  `layout_ok=PASS` and `cached_placement_ok=PASS` on every row, equal
  `object_size` / `payload_offset` across the two variants in all 9 cells,
  `instrumentation_leak_check=PASS`, summary-vs-raw verification for every
  process, `all_invariants=PASS`.
  See its `RESULTS_METADATA.md`, `LAYOUT_VERIFICATION.md`, `MECHANISM.md`,
  `invariants.txt`, `PROVENANCE.md` and `docs/SPSC_REMOTE_CURSOR_CACHE.md`.
- `spsc-false-sharing-pre3a1-payload-offset-confounded/` — the **first
  Phase-3A pass, SUPERSEDED, not canonical**. Real and self-validating: 72
  processes, 360 measured repetitions, all `correctness=PASS`, every row's cursor
  placement verified at runtime under the host's 128-byte line. It is superseded
  because it changed **two** variables at once — the cursor policy footprints
  were 128 bytes (`same_line`) and 256 bytes (`separated`), and since the payload
  array follows the cursors in the object, the payload's relative offset differed
  by 128 bytes between the variants — a second object-layout variable, not merely
  a cursor-placement one. Its numbers
  therefore cannot be attributed to cursor placement, and it must **not** be cited
  for causal cursor-placement claims. Nothing in it was edited or deleted; see its
  `SUPERSEDED.md` for why, and for the one comparison that is still legitimate
  (a secondary methodology observation against the new equal-footprint dataset).
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
