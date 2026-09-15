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
  implementation per process. **Remote-cursor caching is the only intended
  treatment** — reduced remote-load frequency is its primary mechanism, though
  the treatment also carries its own local fast-path bookkeeping — and there is
  no batching, no memory-order change, no CAS, no MPSC/MPMC, no
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
  The primary mechanism metric is **remote loads per try attempt**
  (`remote_loads / (message_count + retries_on_that_side)`, derived in
  `mechanism/ATTEMPTS.csv`). The baseline is **exactly 1.000000** on both sides in
  every cell by construction, and the cached variant is **below 1.000000 on both
  sides in every cell** — it never refreshes more often per attempt (worst case
  **0.996582**, 32 B/65536 consumer). End-to-end **loads per message** is a
  secondary, confounded metric (`loads/message = loads/attempt ×
  attempts/message`): at 8 B the producer's fall 17×–70,102× while the consumer's
  rise 1.9×–4.1×; at 32 B the producer's fall 3×–65,806× while the consumer's
  change is **not one-directional** (0.72× and 0.84× *down* at 1024/4096, 1.08×
  *up* at 65536); at 64 B it reverses (consumer down 1,156×–44,910×, producer up
  2.7×–3.1×). Where it rises, the cause is **retry volume**, not a higher
  per-attempt refresh rate. Meanwhile the cached variant was **slower in 6 of 9
  cells, stably across all four balanced sessions, by 1.09×–2.00×**, and 3 cells
  (all at 32 B) are inconclusive. 32 of 36 paired observations favour the
  baseline; **no cell is stably cached-faster**. Cells exist where loads fall
  dramatically and throughput nonetheless degrades — 8 B / 4096 producer loads
  fall ~57× with throughput **2.005× worse**, the largest penalty in the dataset,
  while 8 B / 65536 falls ~70,102× for only a 1.103× regression: **reduction
  magnitude alone does not predict the throughput outcome**. The message-size
  relationship is **not monotonic** (32 B weak and directionally unstable, 64 B
  consistently moderate, 8 B stable but strongly capacity-dependent), and capacity
  is not irrelevant — it materially changes the magnitude at every size. Caching
  cannot help a thread that keeps finding the queue genuinely full or genuinely
  empty, because the real remote cursor has not moved; that is what drives the
  seven cells where the cached variant does *more* loads/message on one side. No
  cache-miss, coherence-transaction or cache-line-transfer count was measured, and
  none is claimed. **These `ns/msg` levels are NOT comparable to Phase 3A's**:
  this cell shape exhibits strong run-to-run and build-to-build regime variation
  on this host. The observed variation is large enough that absolute `ns/msg`
  values from independently built phases must not be interpreted as treatment
  effects, and it is comparable to or larger than several of the within-phase
  treatment differences reported here. A separate diagnostic suggested
  code-layout sensitivity as one possible contributor, but Phase 3B does not
  isolate the cause and no reproducible diagnostic package is preserved. Both
  variants come from one executable built
  under the same compiler and options — which supports build/toolchain
  comparability — but each is a distinct template instantiation with its own
  emitted machine code, and each leg is an independent process not guaranteed to
  share scheduler placement, core type, migration history, DVFS, thermal state or
  background load. That is neither identical code placement nor identical machine
  regime: the comparison is internally valid, the level is not portable.
  Verified: 72 canonical processes, 360 measured repetitions, `correctness=PASS`,
  `layout_ok=PASS` and `cached_placement_ok=PASS` on every row, equal
  `object_size` / `payload_offset` across the two variants in all 9 cells,
  `instrumentation_leak_check=PASS`, summary-vs-raw verification for every
  process, `all_invariants=PASS`.
  See its `RESULTS_METADATA.md`, `LAYOUT_VERIFICATION.md`, `MECHANISM.md`,
  `mechanism/ATTEMPTS.csv` (derived, not measured), `invariants.txt`,
  `PROVENANCE.md` and `docs/SPSC_REMOTE_CURSOR_CACHE.md`.
- `spsc-tail-latency/` — the **canonical Experiment 02 Phase 4 tail-latency /
  jitter dataset, sparse-sampled instrumentation**. Verified: 36 processes, 180
  measured repetitions, 1,745,280 sampled latencies, every repetition
  `correctness=PASS`, 0 timestamp inversions, 0 stamp-contract failures, and
  **5,240,804** raw→summary checks passed with 0 failures. It proves its own
  instrumentation was sparse: every repetition records
  `expected_samples == sample_count == producer_sample_clock_reads ==
  consumer_sample_clock_reads == 9,696`, against 10,000,000 if every message had
  been stamped — a repetition that took a per-message clock read **fails** rather
  than quietly producing the same 9,696 latencies. The stamp travels **inside**
  the sampled message along the SPSC payload path; there is no `2 × Capacity`
  side array and no second, capacity-dependent memory footprint. Reported per
  cell at the **session-blocked** level (repetition → median of 5 → median of 4
  session medians), which is PRIMARY; the all-20 median is retained only as a
  labelled diagnostic. What it finds: six cells (all 16 B and 32 B) have a
  session-blocked **P50 of exactly 125 ns in all 120 of their repetitions**, and
  three (all 64 B) sit at 30.0 µs / 120.4 µs / 1.95 ms with 1.05–1.09× session
  spread; the fast band is **timer-resolution-limited** (125 ns is three 41.7 ns
  quanta; 92–95% of its samples are within four quanta of zero); the two groups
  differ in which thread waits — established directly by which retry counter
  dominates, and **no queue occupancy or producer lead is recorded anywhere in
  this dataset**, so the associated backlog is described as *consistent with* an
  often-empty or full/near-full regime rather than as a measured occupancy; and
  extreme maxima are isolated in seven of nine
  cells — 32 B / 65536 is the exception, with 7 of 20 repetitions above 5× the
  cell's median maximum, named as a contamination *candidate* and **not
  censored**. The previous canonical pass — same cells, but per-message
  timestamping and a capacity-dependent side array — is preserved real and
  unedited at `spsc-tail-latency-superseded-per-message-timestamp/`; **its
  percentiles are not Phase-4 results and must not be cited as such**, and its
  findings are not assumed to reproduce. See `RESULTS_METADATA.md`,
  `PROVENANCE.md`, `invariants.txt`, `HOST.md`, `SESSIONS.md`,
  `CELL_SESSION_BLOCKED.csv` (**cell-level, PRIMARY**), `TAIL_MATRIX.md`
  (the same, readable), `CELL_TAIL.csv` and `TAIL_RATIOS.csv` (**level-1:
  one row per cell-session, not a cell summary**) and
  `docs/SPSC_TAIL_LATENCY.md`.

  **Phase 4 — COMPLETE / FROZEN.** Experiment 02 — SPSC is **COMPLETE**.
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
- `spsc-tail-latency-superseded-per-message-timestamp/` — the **first
  correctly-measured Phase-4 pass, SUPERSEDED, not canonical**. Real and
  self-validating: 36 processes, 180 measured repetitions, 1,745,280 sampled
  latencies, every repetition `correctness=PASS`, 0 timestamp inversions, and
  **5,240,255** raw→summary checks passed with 0 failures. Nothing in it was
  edited and nothing in it is fabricated. It is superseded because its
  **instrumentation was not sparse and it used a second memory path**:
  `Clock::now()` was executed for *every* message on both sides — 10,000,000
  producer reads and 10,000,000 consumer reads per repetition, not the 9,696
  latencies that were sampled — and the producer stamp lived in a separate
  `ready_ticks[2 * Capacity]` array, a second and independently addressed memory
  working set whose footprint scales with **capacity**. It therefore
  contaminates exactly the capacity comparison this phase exists to make, and it
  put instrumentation cost on every message instead of on the 1-in-1021 that is
  sampled. Sampling governed only which of the already-measured latencies were
  **retained**. Do not cite any percentile here as a Phase-4 result, and do not
  compare its absolute numbers against the final dataset as though the
  difference were a property of the queue. See its `SUPERSEDED.md`, which also
  preserves the side-array aliasing argument the final benchmark no longer
  contains.
- `spsc-tail-latency-CONTAMINATED-concurrent-load/` — **NOT ARCHIVED IN THIS
  REPOSITORY**, the **first Phase-4 pass, INVALID, not canonical**. Real, complete and self-validating: 36 processes, 180
  measured repetitions, every queue gate passing, all 5,240,246 verification
  checks green. It is invalid because it was measured **concurrently with other
  work on the same host** — 8 of its 180 repetitions account for **95%** of the
  experiment's total measured wall time, six of them clustered within
  25.48–25.66 s, with `ns_per_message` 50×–2,000× their own cell's median. Every
  gate in the harness is an invariant of the *queue*, and a starved thread
  violates none of them; it simply runs slower, so no correctness check could
  have caught this. It was caught by reading `ns_per_message` per repetition
  against its own cell's median, which is now an automated check in
  `scripts/verify-spsc-tail-summary.py`. Since Phase 4.1 that check **reports
  rather than invalidates**: a repetition above 5× its cell's median is raised as
  a prominent diagnostic WARNING / contamination *candidate*, and it does not by
  itself fail the dataset, because Phase 4 studies latency and jitter and a
  legitimately rare stall must not be censored for being extreme. What
  disqualified *this* run was the independent evidence — a known competing
  workload on the same host — not the magnitude. Magnitude alone is not proof of
  invalidity; see the H2 section of `docs/SPSC_TAIL_LATENCY.md`. Nothing in it
  was edited or deleted, but its
  **derived aggregate tables were deleted** so that no quotable summary of it
  survives. Do not cite a single number from it. The directory is deliberately
  not committed — 106 MB of raw data for a run that cannot be used — so it and
  its `SUPERSEDED.md` are absent from a clone, and this entry is the repository's
  only record of it. See the "Two hazards" section of `docs/SPSC_TAIL_LATENCY.md`;
  cite it only as the evidence for the per-repetition starvation check and the
  pre-flight load gate.

- `market-data-throughput/` — the **canonical Experiment 03 Phase 3A integrated
  market-data pipeline throughput baseline**. One composed two-thread system
  measured end to end: a pre-generated byte stream → `StreamDecoder` framing →
  the frozen Phase-1B `decode_one` → `SpscSeparatedBaselineRingBuffer<MdMessage,
  4096>` → `MarketDataPipeline<FlatOrderBook>`, every component frozen and none
  modified to move a number. **One canonical cell**, with the chunk size (64 KiB)
  and capacity (4096) as compile-time constants the argument parser refuses to
  override, so no treatment matrix can appear by accident. 4 sessions × (1
  excluded warm-up + 5 measured repetitions) × 5,000,000 live Level messages
  after a 130-message snapshot: **20 measured repetitions, all `PASS`, all with
  the identical reference checksum `0x7E6BA57E42B53319`**. Headline:
  **34.909762 ns/message, 28,646,780 messages/second** (median of four session
  medians), with a **3.676 %** spread across session medians — the figure is
  host-specific and must always be quoted with that spread. `elapsed` runs from
  the release of the start gate to the **consumer's final apply**, stamped by the
  consumer thread itself, so it INCLUDES framing, decode, the queue handoff,
  **both** retry loops, sequencing and the book apply; it is **not** a latency,
  **not** a percentile and **not** a per-message cost, and the clock is read
  twice per repetition only. The initial snapshot is applied **before** the gate,
  so the timed region carries no malformed bytes, sequence gap, stale message,
  recovery, terminal decode error or truncated input. Reported retry
  diagnostics: `consumer_empty_retries` exceeds `producer_full_retries` by
  roughly **3,331 : 1** pooled (420,762,731 vs 126,314) — reported as a
  diagnostic, with **no bottleneck attributed** and **no causal cache/coherence
  claim**, because a failed `try_pop` is not a unit of time and both loops sit
  inside the interval. Before any clock started the **frozen
  `ThreadedMdPipeline`** was required to agree with a single-threaded reference
  on every observable and all twenty counters over the same bytes, which is what
  licenses the harness owning its own thread pair. **Phase 3A — IMPLEMENTED /
  MEASURED. Experiment 03 Phase 3 is NOT complete, and Phase 3B is not
  started** — no P50/P90/P99/P99.9 and no per-message timestamp exists in this
  dataset or its tooling. **Phase-2 and Phase-3 SPSC numbers are NOT comparable
  to these**: a Phase-2 message is a raw payload on an empty queue, whereas a
  Phase-3A message is framed, decoded, sequenced and applied to a live order
  book. See its `RESULTS_METADATA.md`, `SUMMARY.csv` (derived), `raw/`
  (**primary**), `command.txt`, `invariants.txt`, `HOST.md`, `SESSIONS.md`, and
  `docs/MARKET_DATA_THROUGHPUT.md`.

## Honesty rule

Never invent numbers. A dataset enters `docs/results/` only from a real run,
with provenance (machine, compiler/flags, perf version, the raw tool output,
the exact command) recorded next to it. Illustrative output elsewhere in the
repo is always labeled as such.
