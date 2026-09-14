# low-latency-trading-lab

A lab for experiments in low-latency C++ trading infrastructure. This
repository currently hosts three experiments. **Experiment 01** — L2 order book
(`std::map` vs flat representation) — and **Experiment 02** — SPSC ring buffer /
concurrency — live at the repo root: Experiment 02's files sit alongside
Experiment 01's in the shared `include/`, `tests/`, and `docs/` trees, clearly
separated by file name and by namespace (`llob` = order book, `lltl` = queue).
**Experiment 03** — market data pipeline — takes the option that paragraph
always held open and lives in its **own top-level directory**,
`market-data-pipeline/`, with its own `include/`, `tests/`, `CMakeLists.txt` and
namespace (`llmd`), leaving the Experiment 01/02 trees untouched.

> **Status — Experiment 01: Phase 1 COMPLETE / FROZEN; Phase 2 COMPLETE /
> FROZEN; Phase 3M tooling COMPLETE, recordings COLLECTED, attribution analysis
> DEFERRED; Phase 3L tooling READY, native Linux measurement DEFERRED;
> Phase 4 COMPLETE / FROZEN:**
> **Experiment 01 — L2 Order Book: `std::map` vs Flat Representation** is
> implemented, its correctness tests are green, and the deterministic benchmark
> has measured steady-state `apply()` throughput across both implementations,
> five workloads, and four book sizes (Phase 2, COMPLETE / FROZEN). Profiling —
> where the time goes — is split into **Phase 3M** — macOS / Apple Instruments
> on the same M3 Max that produced Phase 2 (tooling COMPLETE; six real
> recordings COLLECTED under `docs/results/phase3-macos-apple-silicon/`;
> per-function call-tree / attribution analysis DEFERRED — it still needs an
> Instruments GUI pass over those recordings) — and **Phase 3L** — Linux `perf`
> (tooling READY; native Linux PMU data DEFERRED, no Linux host). Both use
> opt-in markers around the timed `apply()` loop: an os_signpost interval on
> Apple (`LLOB_SIGNPOSTS=1`) and a perf-control gate on Linux
> (`LLOB_PERF_CONTROL`). Phase 4 tail-latency analysis is **COMPLETE / FROZEN**:
> the hardened tooling (`orderbook_tail_bench`, `scripts/tail-bench.sh`) and the
> canonical six-cell distribution dataset are measured, verified, and published
> under `docs/results/phase4-macos-tail/`. The earlier buggy-tooling cells are
> archived — INVALID, not canonical — under
> `docs/results/phase4-macos-tail-pre4.1-invalid/`.
>
> **Experiment 02 — SPSC Ring Buffer / Concurrency: Phase 1 (Correctness /
> Memory Model) COMPLETE / FROZEN; Phase 2 (Throughput Baseline) COMPLETE /
> FROZEN.** A bounded single-producer / single-consumer ring buffer
> (`include/spsc_ring_buffer.h`) and a mutex reference queue
> (`include/mutex_bounded_queue.h`) are implemented, correctness is green
> (single-thread semantics, one-producer / one-consumer deterministic stress,
> rapid slot reuse at Capacity = 2, a multi-field fixed-size payload stress, and
> a differential run against the mutex queue), strict-warning clean, and
> sanitizer-clean (ASan/UBSan/TSan where the host supports them — see
> `docs/SPSC_MEMORY_MODEL.md` §10 for the exact commands). The memory-model
> argument (happens-before, ordering choices, counter wrap) is documented in
> `docs/SPSC_MEMORY_MODEL.md`. Phase 2 measured the **frozen, unpadded** Phase-1
> SPSC against `MutexBoundedQueue` — 18 cells, 10M messages per repetition, 5
> measured repetitions per process, 4 sessions per cell in a **balanced AB/BA**
> order (2 mutex-first + 2 SPSC-first), 72 processes, 360 measured repetitions,
> 3.6 billion message transfers, all `correctness=PASS` — in
> `docs/results/spsc-throughput/`, with methodology and analysis in
> `docs/SPSC_THROUGHPUT.md`. The result is **not a clean win for either queue**:
> 6 of 9 cells hold one direction across all four balanced sessions (3 each way)
> and 3 cells are inconclusive. Phase 3A then re-ran the same algorithm with the
> cursors forced into one cache line versus distinct cache lines — layout
> **verified at runtime on every measured repetition** — as a controlled
> *cursor-placement / coherence-layout* experiment in
> `docs/results/spsc-false-sharing/`. Phase 3A is **COMPLETE / FROZEN**: 6 of 9
> cells hold one direction across all four balanced sessions (5 separated-faster,
> 1 same-line-faster) and 3 are inconclusive, so the result is a cell-dependent
> coherence-layout effect rather than a general win for padding. Its first dataset
> changed a second variable along with cursor placement (the two cursor policies
> were 128 and 256 bytes, so the payload array's offset within the queue object
> shifted by 128 bytes between the variants — an uncontrolled object-layout
> variable, not just a cursor-placement one); that dataset is real and
> self-validating, and is retained unedited at
> `docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/` but must not
> be cited for causal cursor-placement claims. The shipped design gives both
> cursor policies the **same 256-byte footprint** — same-line keeps *both* cursors
> in the first line and reserves an inert second line purely to equalize the
> footprint — so the payload offset is identical across variants, enforced by both
> the compiler and a pre-timing runtime gate. Phase 3B then re-ran the **separated**
> layout only, adding a thread-owned cached copy of the remote cursor that is
> refreshed only when the cached value says the operation may fail — cursor
> placement, footprint, payload offset, capacity, publication protocol and harness
> all held identical by construction, so **remote-cursor caching is the only
> intended treatment** — reduced remote-load frequency is its primary mechanism,
> but the treatment also carries its own local fast-path bookkeeping (a cached
> read, a comparison, a branch), so **no throughput difference here isolates the
> cost of a single remote atomic load**. It is **COMPLETE / FROZEN** in
> `docs/results/spsc-remote-cursor/`, and the result is **negative**: the
> mechanism worked and the performance did not follow. **Per try attempt the
> cached variant never refreshed the remote cursor more often than the baseline —
> in any cell, on either side.** The headline reductions are **~65,789× fewer
> remote loads per try attempt** (the primary metric; 8 B/65536 producer, reached
> identically at 32 B/65536 producer) and **~70,102× fewer loads per delivered
> message** (the secondary end-to-end metric, same cell). The larger of the two
> is the *secondary* one, inflated by retry volume — and on that metric the
> *other* side's loads/message *rose*, because the side that was blocked simply
> attempted more times, not because it refreshed more often. Yet
> the cached variant was **slower in 6 of 9 cells, stably across all four balanced
> sessions, by 1.09×–2.00×**, with 3 cells inconclusive — and how far the loads
> fell did **not** predict how much throughput suffered (8 B/4096 has a far smaller
> reduction than 8 B/65536 but the largest penalty, ~2.005×). The release/acquire
> publication edge still exists in both variants; caching reduced its *frequency*,
> it did not replace it. **Phase 4 (tail latency / jitter) is COMPLETE / FROZEN**
> for the Experiment 02 queue — see `docs/SPSC_TAIL_LATENCY.md`. It compares **no
> treatments**: the matrix contains the separated-cursor baseline only, so every
> number in it is attributable to one queue. Its **timestamp instrumentation is
> sparse and the dataset proves it** — 9,696 clock reads per thread per
> repetition, not 10,000,000, each repetition's own counters checked against the
> derived count, with the stamp carried *inside* the sampled message rather than
> in a capacity-sized side array — a **structural** advantage (it reuses the
> existing payload publication path and adds no second, independently addressed
> shared-memory path), not a claim that the stamp costs neither a write nor a
> read. Its result is a **stable
> bimodality**: the nine cells split into six whose session-blocked **P50 is
> exactly 125 ns in all 120 of their repetitions** (every 16 B and 32 B cell, at
> every capacity) and three — all 64 B — at 30.0 µs / 120.4 µs / 1.95 ms, stable
> to 1.05–1.09× across sessions. The fast band is **timer-resolution-limited**:
> 125 ns is three 41.7 ns clock quanta, and 92–95% of those cells' samples lie
> within four quanta of zero. The two groups differ in **which thread waits**,
> and the waiting side is the held-up one, so the *other* side is pace-limiting:
> the fast cells are consumer-starved (producer pace-limiting, latencies
> **consistent with an often-empty / low-backlog regime**), the 64 B cells are
> producer-blocked (consumer pace-limiting, latencies **consistent with a full /
> near-full backlog regime**). Occupancy itself is not recorded — no queue-depth
> or producer-lead instrument exists in this harness — so the backlog is a
> reading the retry counters and latency levels support, not a measurement.
> Within the slow band a larger capacity means fewer producer stalls but a
> *longer* measured latency, **≈ 0.95 of one full ring's drain time**, which is
> consistent with a one-ring drain/backlog model but neither proves it nor is
> proved by it.
> Percentiles are reported at the **session-blocked** level (repetition → median
> of 5 → median of 4 session medians) as PRIMARY; the all-20 median is a labelled
> diagnostic only — two different estimands under two different aggregation
> rules, which need not be equal. Session blocking is a **process/time**
> grouping: one process and address-space lifetime, one binary, a short temporal
> block — and **not** a verified fixed thread placement, since no affinity is set
> anywhere. The 16/32/64 B axis is a **message shape**, not a pure payload byte
> count — size and per-message construction/validation work are one bundled
> workload dimension — so what is claimed is that the 64-byte message shape
> consistently produced the consumer-limited regime, never that payload size
> *causes* it. **No causal attribution of any kind is made** — nothing was
> profiled, and "the latency is the timer, not the queue" is explicitly **not**
> claimed: the queue does real work at a scale this clock bounds but does not
> resolve. Extreme maxima are
> isolated in seven of nine cells; 32 B / 65536 is the exception (7 of 20
> repetitions above 5× the cell's median maximum), named as a contamination
> *candidate* and left uncensored. **No Phase-2
> number is attributed to false sharing** — Phase 2 verified no cursor addresses,
> so it cannot be cited for or against the frozen layout's line placement.
>
> **Phase 3B's absolute `ns/message` values are NOT comparable to Phase 3A's.**
> This cell shape exhibits **strong run-to-run and build-to-build regime variation
> on the development host**. The observed variation is large enough that absolute
> `ns/message` values from independently built phases must not be interpreted as
> treatment effects: Phase 3A's separated 8 B/1024 cell measured ~15.6 ns/message
> from its own binary while the Phase-3B `baseline` variant of the same cell
> measures ~46 ns/message, with ~10× the consumer spinning — a gap of that order
> between two builds of the *same* algorithm. A separate
> diagnostic, run outside this dataset, suggested code-layout sensitivity as **one
> possible contributor** to that variation, but **Phase 3B does not isolate its
> cause** and no reproducible diagnostic package is preserved alongside the data.
> Both Phase-3B variants do come from **one executable built under the same
> compiler and options**, which supports build/toolchain comparability — but each
> variant is still a **distinct template instantiation with its own emitted machine
> code**, and each leg runs in an **independent process** whose scheduler
> placement, core type, migration history, DVFS, thermal state and background load
> are not guaranteed to match. That is **not** the same as sharing runtime state or
> regime. The balanced adjacent AB/BA design mitigates temporal ordering bias; it
> does not guarantee identical machine state. See `docs/SPSC_REMOTE_CURSOR_CACHE.md`
> §7.5.
>
> **Experiment 03 — Market Data Pipeline: Phase 1 (Sequencer + Binary Decoder /
> Correctness) COMPLETE / FROZEN.** A feed-handler sequencer over a sequenced L2
> stream with inline snapshot framing (`market-data-pipeline/`) detects when the
> stream it is reading is lossy, refuses to pretend otherwise, and accounts for
> it; a byte-stream decoder turns raw bytes into the same typed messages that
> sequencer consumes. **Phase 1A** (sequencer / snapshot / recovery) and
> **Phase 1B** (wire protocol / decoder) are both COMPLETE / FROZEN. **Neither
> phase measures anything**: no clock is read anywhere, there is no benchmark
> binary and no `BENCH_ARCH_FLAGS` entry, and none of their numbers are durations.
> The state machine has four states (`NotSynced` / `Live` / `Snapshot` / `Gap`)
> and ten per-message outcomes, and the pipeline owns framing and sequencing
> while the book owns snapshot validity and application — a bracketed run commits
> through the sink's own `load_snapshot()`, so there is **one snapshot contract,
> not two**. In-order levels are delegated to `book.apply()` rather than
> re-decided, because `InvalidUpdate` (`qty < 0`) unsyncs the book and
> `OutOfRange` consumes the sequence — two outcomes a sequence comparison alone
> cannot see. Verification is six layers with their limits stated: the
> specification sketch verbatim as a literal expected-outcome table (the only
> layer that can catch a *specification* error), 28 hand-written scenario vectors,
> an accounting identity asserted over the whole corpus plus a coverage assertion
> that every outcome and counter is actually reached, 1550 seeded traces against
> an independently written oracle, 300 traces run through two different sinks,
> and generator determinism. **Phase 1B** adds a synthetic big-endian wire
> protocol (12-byte header, 1 = `SnapshotBegin` / 2 = `Level` / 3 = `SnapshotEnd`,
> version 1) with an allocation-free `decode_one()` that returns one message per
> call from the front of a byte span. Its central invariant is that
> `consumed != 0` **if and only if** the status is `Ok` — so `NeedMoreData`, the
> ordinary answer for a partial socket read, can never publish a half-written
> message into a live book. Bytes are assembled by explicit shifts with no
> `reinterpret_cast`, no packed struct and no unaligned load, and the tests
> compare against literal byte constants so an encoder and decoder written by the
> same hand cannot agree on a wrong byte order unnoticed — a sabotage run that
> reversed both helpers consistently left every round-trip suite green and failed
> only the literal-byte suites. All green under CTest (33/33 including the
> exit-code guards), byte-identical across two runs, and clean under ASan and
> UBSan. Methodology: `docs/MARKET_DATA_PIPELINE.md`; wire protocol and decoder
> contract: `docs/MARKET_DATA_PROTOCOL.md`.

## Experiments

| # | Experiment | Status |
|---|------------|--------|
| 01 | L2 Order Book: `std::map` vs Flat Representation | Phase 1, 2, 4 COMPLETE / FROZEN; Phase 3M tooling COMPLETE + recordings COLLECTED (attribution analysis deferred); Phase 3L tooling READY (native Linux measurement deferred) |
| 02 | SPSC Ring Buffer / Concurrency | Phase 1 (Correctness / Memory Model) COMPLETE / FROZEN; Phase 2 (Throughput Baseline) COMPLETE / FROZEN; Phase 3A (Controlled Cursor Placement) COMPLETE / FROZEN; Phase 3B (Remote Cursor Caching) COMPLETE / FROZEN; Phase 4 (Tail Latency / Jitter, measurement only — no treatment comparison) COMPLETE / FROZEN |
| 03 | Market Data Pipeline (sequencer + binary decoder) | Phase 1A (Sequencer / Snapshot / Recovery Correctness) COMPLETE / FROZEN; Phase 1B (Binary Protocol / Decoder Correctness) COMPLETE / FROZEN; Phase 1 overall COMPLETE / FROZEN. Phase 2 (Decoder Thread → SPSC → Book Thread) NOT STARTED |

## Layout

```
low-latency-trading-lab/
├── include/
│   ├── types.h              # Side, L2Update, BookSnapshot, apply contract
│   ├── map_order_book.h     # std::map baseline
│   ├── flat_order_book.h    # dense tick-addressed book
│   ├── bitset_flat_order_book.h  # Optimization Study: hierarchical-occupancy
│   │                             #   bitmap twin of flat_order_book (drop-in)
│   ├── transition_aware_flat_order_book.h  # Optimization Study CONTROL: frozen
│   │                                       #   flat book + transition-aware
│   │                                       #   positive path, NO bitmap
│   ├── mutex_bounded_queue.h  # Exp 02 reference: mutex-guarded bounded FIFO
│   ├── spsc_ring_buffer.h     # Exp 02 candidate: SPSC ring buffer; lock-free
│   │                          #   cursor protocol where atomic<size_t> is
│   │                          #   always lock-free (static_assert, no fallback)
│   ├── cache_line.h           # Exp 02 Phase 3A: runtime host cache-line query
│   │                          #   + the guards that fail rather than mis-measure
│   ├── spsc_cursor_layout_ring_buffer.h  # Exp 02 Phase 3A: the two cursor-layout
│   │                          #   controls over ONE algorithm body (same-line vs
│   │                          #   separated) + runtime layout report
│   └── spsc_remote_cursor_ring_buffer.h  # Exp 02 Phase 3B: remote-cursor caching
│                              #   over the ONE Phase-3A separated layout (direct vs
│                              #   cached remote reads) + cached-placement report
├── tests/
│   ├── order_book_tests.cpp # every scenario run against BOTH books
│   ├── bitset_order_book_tests.cpp  # Optimization Study: four-way differential
│   │                                 #   (map/flat/tuned/bitset) correctness suite
│   ├── phase4_stats_tests.cpp  # Phase 4 distribution/percentile regression tests
│   ├── spsc_ring_buffer_tests.cpp  # Exp 02: semantics + SPSC stress +
│   │                               #   differential vs the mutex queue
│   ├── spsc_false_sharing_tests.cpp  # Exp 02 Phase 3A: the same protocol against
│   │                               #   BOTH layouts + runtime layout evidence
│   │                               #   (incl. a negative control on the guard)
│   ├── spsc_remote_cursor_tests.cpp  # Exp 02 Phase 3B: the same protocol against
│   │                               #   BOTH remote-read treatments + cached-state
│   │                               #   placement evidence + counter-wrap tests
│   └── spsc_tail_latency_tests.cpp  # Exp 02 Phase 4: the tail-latency harness —
│                                    #   sampling, percentile and clock conversion
│                                    #   rules + bench smoke and rejection tests
├── benchmark/
│   ├── stream_gen.h            # single source of truth for the A/B/C/D/E op streams
│   ├── order_book_bench.cpp    # Phase 2 steady-state apply() throughput benchmark
│   ├── order_book_bitmap_bench.cpp  # Optimization Study: flat/tuned/bits steady
│   │                                 #   throughput + --check/--gaps/--memory/--inproc
│   ├── order_book_gap_bench.cpp     # Optimization Study: next-best-gap ladder sweep
│   ├── order_book_tail_bench.cpp  # Phase 4 fixed-batch tail-latency sampler
│   ├── spsc_throughput_bench.cpp  # Exp 02 Phase 2: two-thread end-to-end SPSC vs
│   │                              #   mutex transfer throughput (-O3 -DNDEBUG forced)
│   ├── spsc_false_sharing_bench.cpp  # Exp 02 Phase 3A: same-line vs separated
│   │                                 #   cursor placement (--impl=...; one impl per process)
│   ├── spsc_remote_cursor_bench.cpp  # Exp 02 Phase 3B: direct vs cached remote
│   │                                 #   cursor reads (--impl=... --instrument=...;
│   │                                 #   one impl per process; canonical throughput
│   │                                 #   comes only from --instrument=0)
│   ├── tail_stats.h            # Phase 4 distribution metrics / percentile definitions
│   ├── spsc_tail_latency_bench.cpp  # Exp 02 Phase 4: producer_ready -> consumer_received
│   │                                #   distribution over the separated-cursor baseline
│   │                                #   (ONE queue; no treatment; even intervals rejected)
│   └── spsc_tail_harness.h     # Exp 02 Phase 4: sequence-keyed sparse sample
│                               #   schedule, exact tick->ns conversion and the timer
│                               #   calibration, shared by the bench and its tests
├── scripts/
│   ├── bench.sh                # Phase 2 canonical per-process run
│   ├── tail-bench.sh           # Phase 4 canonical matrix runner (one invocation per cell)
│   ├── spsc-throughput.sh      # Exp 02 Phase 2 canonical matrix runner
│   │                           #   (18 cells x 4 balanced AB/BA sessions, one impl per process)
│   ├── spsc-false-sharing.sh   # Exp 02 Phase 3A canonical matrix runner
│   │                           #   (2 layouts x 18 cells x 4 balanced AB/BA sessions)
│   ├── spsc-remote-cursor.sh   # Exp 02 Phase 3B canonical matrix runner (2 treatments
│   │                           #   x 18 cells x 4 balanced AB/BA sessions + a separate
│   │                           #   instrumented mechanism leg)
│   ├── verify-tail-summary.sh  # Phase 4 raw<->summary verification
│   ├── perf-profile.sh         # Linux perf profiling harness (Phase 3L, per-cell)
│   ├── phase3m-instruments.sh  # Phase 3M xctrace/Instruments recorder (needs full Xcode)
│   ├── collect-macos-profile-metadata.sh  # host/chip/toolchain metadata (Phase 3M and Phase 4)
│   ├── spsc-tail-latency.sh    # Exp 02 Phase 4 canonical runner: 9 cells x 4 sessions,
│   │                           #   fresh Release build, load pre-flight, then raw->summary
│   │                           #   verification and the DERIVED tables; writes source
│   │                           #   digests into HOST.md
│   ├── verify-spsc-tail-summary.py  # Exp 02 Phase 4: recomputes every statistic from
│   │                                #   raw/, checks the sparse-instrumentation counters,
│   │                                #   and reports the per-repetition starvation diagnostic
│   └── analyze-spsc-tail.py    # Exp 02 Phase 4 DERIVED tables; refuses to run on a
│                               #   dataset whose invariants.txt is not a passing one
├── cmake/
│   └── assert_nonzero_exit.cmake  # ctest guard for the test exit-code self-test
├── docs/
│   ├── results/             # committed datasets (phase2-m3max/, phase3-macos-apple-silicon/,
│   │   │                    #   phase4-macos-tail/ canonical + -pre4.1-invalid/ archived,
│   │   │                    #   orderbook-bitmap-optimization/ study, and
│   │   │                    #   spsc-throughput/ canonical Exp 02 Phase 2 + its
│   │   │                    #   two superseded passes (fixed-order,
│   │   │                    #   single-session), and spsc-false-sharing/
│   │   │                    #   canonical Exp 02 Phase 3A + its superseded
│   │   │                    #   payload-offset-confounded pass, and
│   │   │                    #   spsc-remote-cursor/ canonical Exp 02 Phase 3B,
│   │   │                    #   spsc-tail-latency/ canonical Exp 02 Phase 4
│   │   │                    #   (sparse instrumentation) + its superseded
│   │   │                    #   per-message-timestamp pass; its
│   │   │                    #   CONTAMINATED-concurrent-load/ first pass is
│   │   │                    #   deliberately not committed; see README.md)
│   │   └── README.md        # layout + honesty rule
│   ├── ORDERBOOK_BITMAP_OPTIMIZATION.md  # Optimization Study analysis (item 12)
│   ├── SPSC_MEMORY_MODEL.md  # Exp 02: happens-before + memory-order argument
│   ├── SPSC_THROUGHPUT.md    # Exp 02 Phase 2: throughput methodology + analysis
│   ├── SPSC_FALSE_SHARING.md # Exp 02 Phase 3A: controlled cursor-placement
│   │                         #   methodology + analysis (false vs true sharing;
│   │                         #   equal-footprint control; runtime layout proof)
│   ├── SPSC_REMOTE_CURSOR_CACHE.md  # Exp 02 Phase 3B: remote-cursor caching
│   │                         #   WHY/WHAT/HOW, correctness + counter-wrap
│   │                         #   argument, mechanism vs performance split
│   ├── SPSC_TAIL_LATENCY.md  # Exp 02 Phase 4: the measurement contract and what it
│   │                         #   is not, the sparse sampling/clock rules, two hazards
│   │                         #   found by running it, and the results with claim labels
│   ├── MARKET_DATA_PIPELINE.md  # Exp 03 Phase 1A: the seam, the full transition
│   │                         #   table, the accounting identity, the two inherited
│   │                         #   asymmetries, the six verification layers and what
│   │                         #   each cannot establish
│   ├── MARKET_DATA_PROTOCOL.md  # Exp 03 Phase 1B: the wire layout byte by byte,
│   │                         #   decoder status semantics, the consumed invariant,
│   │                         #   stream decoding, and the wire-vs-sequence-vs-book
│   │                         #   validation layers
│   └── profiling/           # Phase 3 guides (README.md, MACOS_INSTRUMENTS.md) + Phase 4
│                            #   tail-latency methodology (PHASE4_TAIL_LATENCY.md)
├── market-data-pipeline/    # Experiment 03 (own top-level dir; header-only, llmd)
│   ├── include/
│   │   ├── md_message.h     # MdKind / MdMessage: the typed feed vocabulary
│   │   ├── market_data_pipeline.h  # Phase 1A: the sequencer — 4 states, 10 outcomes,
│   │   │                    #   counters, recovery episodes — templated on the sink
│   │   ├── md_wire_protocol.h  # Phase 1B: byte offsets, message IDs, version, and
│   │   │                    #   the big-endian read/write helpers
│   │   ├── md_decoder.h     # Phase 1B: DecodeStatus / DecodeOutcome / decode_one
│   │   ├── md_encoder.h     # Phase 1B: test-and-fixture encoder (allocates; not
│   │   │                    #   the hot path, and not on it)
│   │   └── md_stream_gen.h  # deterministic base scenarios + the 15-entry named
│   │                        #   mutation catalogue + seeded composition grammar
│   ├── tests/
│   │   ├── md_oracle.h      # independently written reference implementation
│   │   │                    #   (shares the vocabulary, none of the logic)
│   │   ├── market_data_pipeline_tests.cpp  # 6 suites: sketch vectors, scenario
│   │   │                    #   vectors, accounting, corpus coverage, differential
│   │   │                    #   fuzz vs the oracle, sink agreement
│   │   └── md_decoder_tests.cpp  # 6 suites: literal expected bytes, endian
│   │                        #   helpers, stream decoding, exhaustive truncation,
│   │                        #   malformed frames, bytes -> decoder -> pipeline
│   └── CMakeLists.txt       # header-only INTERFACE lib + both test targets + their
│                            #   exit-code guards; deliberately no benchmark target
├── CMakeLists.txt
└── README.md
```

## Build & test

Requires a C++20 compiler and CMake ≥ 3.16.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Design contract (identical for both books)

Prices are **integer ticks**; doubles never appear. An update is
`(seq, side, price, qty)`:

- `qty > 0` sets the absolute size of the level (creating or replacing it);
- `qty == 0` deletes the level;
- `seq` must advance by exactly `+1` for every applied update while synced; a
  skipped `seq` marks the book **unsynced** and returns `GapDetected`;
- while unsynced, updates are rejected (`Stale`) until `load_snapshot()`
  re-synchronizes the book from full state;
- a `qty < 0` is corrupt (`InvalidUpdate`) and desyncs the book until a
  snapshot;
- a price outside the configured band is `OutOfRange` — ignored, but the seq
  is consumed so the view stays contiguous.

`best_bid()` / `best_ask()` return cached best prices in O(1) and report `0`
for an empty side.

## Correctness

`order_book_tests` runs every scenario against both books and requires
identical observable state after each step:

adding a level, updating quantity (best and non-best), deleting a non-best
level, deleting the best level, deleting the last level on a side, empty
book/sides, a new best price appearing, snapshot loading, sequence-gap
desync, recovery via a fresh snapshot, and best-price discipline across the
spread. The suite also covers negative quantity, out-of-range updates,
malformed-snapshot rejection, best-price deletion with an adjacent next-best
over a large (1M-tick) price domain, and a differential fuzzer that applies
thousands of random well-formed updates (including periodic snapshot resyncs)
to both books and asserts parity at every checkpoint.

## Benchmark (Experiment 01, Phase 2)

`benchmark/order_book_bench.cpp` measures **steady-state `apply()` throughput**
of one book implementation over pre-recorded, well-formed update streams. It is
deliberately deterministic and honest about what it excludes:

- **All updates are generated before the timed section.** A fixed-seed generator
  emits the entire stream (fill snapshot + steady ops) before any clock starts;
  RNG cost, stream construction and snapshot load never reach the timed region.
- **The timed region is a pure `apply()` replay.** Sequence numbers advance by
  exactly 1 and the book starts snapshot-loaded (the only way a fresh book
  becomes usable), so every timed op is `Applied` — the benchmark never times a
  rejected/`Stale` update. `--check` replays the identical stream through **both**
  books at once and requires they agree on every op's result, sync state,
  sequence, and best bid/ask (price and qty); any divergence exits non-zero and
  is never timed.
- **Each cell reports the best of `reps` blocks.** Every block cold-starts a
  fresh book from the same snapshot and replays the identical op stream, so the
  only difference between two books' timings is the book implementation. Best-of
  discounts scheduling noise (which only ever adds latency).
- **Canonical runs use one book design per process.** `orderbook_bench map …`
  and `orderbook_bench flat …` are separate invocations, never the two designs
  interleaved in a shared address space. Per-process runs buy clean
  process/address-space isolation, no mixed implementation state, and clean
  profiling/perf attribution (Phase 3) — but they do **not** buy thermal
  isolation: thermal state and system-level load survive process exit, so
  back-to-back long runs can still drift. The `both` mode (map then flat in one
  process) is only a quick local sanity check and is never used for reported
  results.

Price-domain model: the configured domain is `[1, 2N]` where `N` is the scale in
price levels. Each side starts with `N` live levels (bids at `N+1..2N`, asks at
`1..N`). Workloads differ in how close to `N` they hold that count afterward —
A holds exactly `N` throughout, B/C/D maintain it near `N` (each delete is later
restored, so density returns toward `N`; transient holes may exist in a finite
prefix before the restore catches up), E may drift below `N` — but all keep the
side far from empty, so each cell measures steady state, never a draining book.
See the notes under the workload table.
Workloads:

| Workload | What it does | Live levels over a run |
|----------|--------------|------------------------|
| A update-only | every op re-quantifies a random present level; the level set never changes | exactly N throughout |
| B 10% deletes | ~10% of ops delete a random present level; the rest add at a random absent level (restoring what was deleted) | ~N: maintained near N (each delete is later restored, density returns toward N); exactly N at every prefix where the restore has caught up, transient holes possible before it does |
| C frequent best deletion | ~45% delete the current best level (forces the flat book's inward re-scan); the rest refill the most recently vacated level, which restores it just below the current best | ~N: restored toward N (refill rate ≥ delete rate); exactly N whenever the side is full with no pending hole — a finite stream may end with a small pending-hole deficit |
| D concentrated top-of-book | ops touch only a small window at the best end; ~15% delete a present window level, ~85% add at an absent window level | ~N inside the window: maintained near full (each delete is later restored; transient holes possible); the levels below the window never move |
| E uniformly random | fair side coin, price uniform over the whole region, 50% delete / 50% add | ≤ N: NOT conserved — random deletes/adds let occupancy drift below N; the exact finite-run value is not hard-coded (see below) |

So A is the only workload whose live level count is exactly `N` for the whole
timed block; B/C/D hold it approximately `N` (maintained near `N` — restored
toward it after each delete, with transient holes possible in any finite
prefix); E does not conserve it — it starts at `N`, and since deletes and
adds are chosen at random the occupancy may drift below `N`. All keep the side
far from empty, so every cell measures steady state — and because each workload
replays the identical stream through both books, any of these level-count
profiles is exactly matched between map and flat.

For E, how far below `N` occupancy drifts over a finite run depends on the
scale, the update count, the RNG stream, and the generator's retry/fallback
behavior — so no single equilibrium value is claimed here. `--check` replays
each stream and prints the actual ending level counts, so the generated stream
can be inspected directly rather than assumed.

### Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build            # builds orderbook_tests and orderbook_bench
scripts/bench.sh               # full matrix: map/flat x A-E x 1k..1M levels
# or by hand:
./build/orderbook_bench both all all            # full matrix, default 2M ops, 3 reps
./build/orderbook_bench flat C 1000000          # one impl, one workload, one scale
./build/orderbook_bench --check updates=100000  # validate streams, no timing
```

The benchmark target always compiles with `-O3 -DNDEBUG` (CMake forces it
regardless of build type, so a debug build cannot accidentally time unoptimized
code). CPU/arch tuning is **opt-in and free-form** via `BENCH_ARCH_FLAGS`, and a
flag the compiler does not support is detected at configure time and dropped
with a warning instead of failing the build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS="-march=native"  # gcc/clang on Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS="-mcpu=apple-m3"  # Apple clang on M3
```

The canonical results below were built with **no** arch flag (the compiler
default for the target).

Phase 3 profiling uses a dedicated `build-perf/` directory (fresh Release, no
arch flag). On Apple it adds an opt-in os_signpost interval (`LLOB_SIGNPOSTS=1`)
around the timed loop for Instruments; on Linux an opt-in perf-control gate
(`LLOB_PERF_CONTROL`) does the same for `perf`. Both are no-ops in normal runs.
See `docs/profiling/README.md` (Phase 3M/3L) and
`docs/profiling/MACOS_INSTRUMENTS.md`.

### Results

Canonical measurement environment:

| | |
|---|---|
| Machine | Apple MacBook Pro (Apple M3 Max), model identifier `Mac15,10` |
| CPU | Apple M3 Max, `arm64` |
| OS | macOS 14.2.1 (Build 23C71) |
| Compiler | Apple clang 15.0.0 (`clang-1500.1.0.2.5`), target `arm64-apple-darwin23.2.0` |
| Build | CMake 4.4.3; `-O3 -DNDEBUG` forced on the benchmark target; **no** arch flag (compiler default) |
| Stream | 2,000,000 steady `apply()` ops per timed block |
| Reps | 3 per cell; reported = **best of 3** (min wall time) |
| Process isolation | map and flat measured in **separate process invocations** (process isolation, not thermal isolation) |
| Date | 2026-09-07 |

Units: ns per `apply()`. Rows are ns/update; the full raw CSV and machine
metadata are committed under `docs/results/phase2-m3max/`.

| levels | impl | A upd-only | B 10% del | C best-del | D top-of-bk | E uniform |
|--------|------|-----------:|----------:|-----------:|------------:|----------:|
| 1,000  | map  |    32.5    |    36.1   |    42.8    |    33.1     |   61.6    |
| 1,000  | flat |    4.4     |    4.4    |    5.9     |    4.7      |    5.1    |
| 10,000 | map  |    60.8    |    72.3   |    48.1    |    49.2     |  134.9    |
| 10,000 | flat |    4.3     |    4.5    |    6.1     |    4.5      |    5.0    |
| 100,000| map  |    85.1    |   159.7   |    61.0    |   104.2     |  502.3    |
| 100,000| flat |    4.4     |    4.5    |    6.1     |    4.5      |    5.0    |
| 1,000,000 | map |  182.7    |   181.5   |    70.6    |   128.1     |  383.5    |
| 1,000,000 | flat |   4.3    |    4.5    |    5.9     |    4.5      |    5.1    |

**What these measure.** All numbers are for a book that **starts** at `N` live
levels per side (A keeps it at exactly `N`; B/C/D keep it approximately `N`;
E may drift below `N`); snapshot loading, stream generation and the clock read
are excluded. The timed loop is a pure `apply()` replay of pre-recorded updates
with a per-iteration compiler barrier (so the optimizer cannot prove the flat
book's stores dead or reorder across `apply()` calls).

**Reading the results.**

- **Map cost scales with book size.** `std::map` is a red-black tree: each op
  walks tree nodes allocated across the heap, so as the book grows the pointer
  chain spans more cache lines and misses mount. update-only (A) goes ~32 ns →
  ~183 ns as levels go 1k → 1M. The uniformly-random workload (E) is the worst
  case — deletes/adds land anywhere in the tree, maximizing pointer chasing
  (~62 ns → ~384 ns).
- **Flat cost stays nearly flat over the tested range — on this machine.** The
  dense book is a direct array store: a price maps to one slot, so an update
  touches a single array element (plus the cached-best bookkeeping) and follows
  no pointers. That is an O(1) *algorithmic* bound; the measured ~4.3–6.1 ns at
  every scale is an empirical result for this machine and memory layout. At the
  largest scale the active working set is ~8 MB per side (~16 MB across both
  sides; each side's live levels occupy a contiguous half of its 2N-slot
  array), far beyond the stream's private cache, yet an update still touches
  just one randomly chosen cache line of it — which is why the number moves so
  little from 1k to 1M levels. Do not read "nearly flat here" as a guarantee at
  other scales, on other hardware, or under a memory layout where the update's
  cache line is not already resident.
- **Best-price deletion (C) costs the flat book extra but is comparatively
  cheap for the map — an observation, not yet explained.** Deleting the best
  forces the flat book to rescan inward from the adjacent slot (~6 ns vs ~4.4
  ns update-only). On the map side, C is *empirically* cheaper than several
  other map workloads (~71 ns at 1M, versus ~128 ns for D and ~183 ns for A).
  Why is not established here: the map still does a keyed `erase()` with
  red-black lookup and rebalancing, and repeated near-touch access may improve
  locality, branch predictability, or tree-path locality — but Phase 2 only
  records the observation. Determining the actual reason is a Phase 3 perf
  job, not something to assert from these throughput numbers.
- **Speedups are large and grow with scale.** At 1,000 levels flat is ~7–12×
  faster; at 1,000,000 levels the update-only (A) speedup is ~42× and the
  uniformly-random (E) speedup is ~75× — the largest in the matrix. Where the
  map's structure is heavily exercised (random full-book churn, E), flat is
  ~75× faster; on the flat book's own stress test (best deletion, C) the gap
  narrows to ~12×.

**Methodology notes / limitations.**

- **Live level counts are exact only for A.** Only workload A holds its live
  count at exactly `N` for the whole block. B/C/D maintain it near `N` — each
  delete is restored by a later refill, so density returns toward `N`, but a
  finite prefix may contain transient holes (the count returns to `N` once the
  deletes are restored). E does not conserve levels: it starts at `N` and may
  drift below it — how far depends
  on scale, update count, the RNG stream, and generator fallback behavior, so
  no single value is asserted. Because every workload replays the *identical*
  stream through both books, whatever level-count profile a workload has is
  exactly matched between map and flat, so the comparison stays fair.
- **One implementation per process, for process isolation — not thermal
  isolation.** map and flat were measured in separate process invocations so no
  single address space mixes the two designs and profiling/perf attribution
  stays clean (Phase 3). Separate processes do not guarantee thermal isolation
  or eliminate system-level drift: thermal state and background load survive
  process exit, and the two long, CPU-saturating runs still happened
  back-to-back on one machine. The `both` mode (back-to-back in one process) is
  only a quick local sanity check and is not used for reported numbers.
- **Scope.** These are single-core, single-writer mean throughput reads of
  `apply()` on an otherwise quiet machine; they are not latency percentiles
  (Phase 4), and best-of-3 was chosen to discount scheduler noise. Running on a
  shared/heterogeneous machine would add variance. Snapshot load and stream
  generation are excluded by construction.

## Experiment 01 Optimization Study (internal, complete, control-isolated)

A post-Phase-4 internal study evaluated a hierarchical-occupancy-bitmap twin of
the flat book (`include/bitset_flat_order_book.h`) — same semantics, no
inheritance, bitmap maintained only on occupancy transitions. Because that
candidate also carries a **transition-aware positive path**, it was later
control-isolated with a no-bitmap control (`include/transition_aware_flat_order_book.h`)
so the two changes are attributed separately. **Status: complete.** It is a
regime-dependent *alternative*, not a replacement, and no frozen implementation
or result was changed. Measured on the same M3 Max: correctness (four-way
differential vs `MapOrderBook`, `FlatOrderBook`, the control, and the candidate;
sanitizer- and strict-warning-clean), memory overhead of the occupancy hierarchy
≈ 1.59 % of the quantity arrays at 1M levels, and a best-delete crossover gap of
**4–8 price ticks, reproducibly crossing at g = 8 across 16 independent
rounds** — a regime the frozen A–E workloads never reach (only C deletes the
best in volume, always at distance ≤ 1). The isolation result: the frozen A/D
"bitmap faster" rows were **transition-aware control flow, not the bitmap** —
the control alone is robustly −10 % (A) and −15/−18 % (D), +25/+28 % (B), while
the bitmap's marginal effect over the control is no robust win on any frozen
workload (neutral-to-harmful on B/C/D/E, ambiguous on A). The control flow is
worth adopting independently for requantify-heavy streams; the bitmap is
reserved for sparse best-delete-gap regimes. See
`docs/ORDERBOOK_BITMAP_OPTIMIZATION.md` for the full analysis and
`docs/results/orderbook-bitmap-optimization/` for the raw data.

## Experiment 02 — SPSC Ring Buffer

**Status: Phase 1 — Correctness / Memory Model: COMPLETE / FROZEN.
Phase 2 — Throughput Baseline: COMPLETE / FROZEN.
Phase 3A — Controlled Cursor Placement: COMPLETE / FROZEN (its first,
payload-offset-confounded dataset is retained unedited but superseded; see
below).
Phase 3B — Remote-Cursor Caching: COMPLETE / FROZEN (the mechanism reduced
remote cursor loads as designed; the throughput did not follow).
Phase 4 — Tail Latency / Jitter: COMPLETE / FROZEN.** A **measurement** phase
over the frozen separated-cursor baseline queue on the Apple M3 Max: 9 cells
(3 message sizes × 3 capacities), 36 processes, 180 measured repetitions,
1,745,280 sampled latencies, with **sparse** timestamp instrumentation — 9,696
clock reads per thread per repetition, not 10,000,000, proven per repetition by
the harness's own counters. It compares **no treatments** — the cached Phase-3B
variant, `MutexBoundedQueue` and the `same_line` layout are absent by design —
and it opens no new optimization. **Result: a stable bimodality** — six cells
(all 16 B and 32 B) have a session-blocked **P50 of exactly 125 ns in all 120 of
their repetitions**, and the three 64 B cells sit at 30.0 µs / 120.4 µs /
1.95 ms, stable to 1.05–1.09× across sessions; the two groups differ in which
thread waits — consumer-starved/producer-limited in the fast cells,
producer-blocked/consumer-limited in the 64 B cells, with the backlog described
only as *consistent with* empty/near-full since occupancy is not recorded — and
within the slow band a larger capacity means a *longer* measured latency
(≈ 0.95 of one full ring's drain time, a one-ring drain/backlog model the numbers
are consistent with rather than a proven
mechanism). The fast band is **timer-resolution-limited** (125 ns is three
41.7 ns quanta; 92–95% of its samples are within four quanta of zero) — not
"the timer, not the queue". See
`docs/SPSC_TAIL_LATENCY.md` and `docs/results/spsc-tail-latency/`.
A bounded, single-producer / single-consumer message queue with no mutex and no
CAS, modeling `Feed / Decoder → SPSC → OrderBook / Strategy`. Two header-only
types in `lltl`:

- `include/mutex_bounded_queue.h` — `MutexBoundedQueue<T, Capacity>`: the
  correctness **reference** baseline (one `std::mutex`, fixed storage). Thread
  safe for any number of producers/consumers by construction; deliberately not
  optimized; not a candidate.
- `include/spsc_ring_buffer.h` — `SpscRingBuffer<T, Capacity>`: the candidate.
  Power-of-two capacity enforced by `static_assert`; unsigned `std::size_t`
  monotonic counters (64-bit on the canonical hosts) index slots by
  `counter & (Capacity - 1)`; full capacity usable
  (`full: head - tail == Capacity`, `empty: head == tail`). A second
  `static_assert` requires `std::atomic<std::size_t>::is_always_lock_free`, so
  the lock-free claim is checked at compile time rather than assumed; there is
  deliberately **no mutex fallback**.

Both expose the same conceptual `try_*` API:
`bool try_push(const T&)`, `bool try_push(T&&)`, `bool try_pop(T&)`,
`bool empty()`, `static constexpr std::size_t capacity()`. Neither `try_*` ever
waits for the queue *state* to change, and neither queue spins or sleeps
internally. Allocation and throwing are properties of the payload, not the
queue: the queue's own storage is preallocated and the implementation makes no
allocator calls in the hot path, but `T`'s copy/move assignment is user code and
may allocate, throw, or block. `MutexBoundedQueue` is additionally **neither
noexcept nor truly non-blocking** — acquiring its mutex may block under
contention and `std::mutex::lock()` may throw `std::system_error`. `T` must be
default-constructible and assignable (Phase-1 simplification); intended
low-latency message types are fixed-size, nothrow, allocation-free values.

The lock-free claim is scoped to the **cursor protocol** on platforms where the
cursor atomic is always lock-free — it says nothing about `T`'s copy/move
assignment, which may allocate or block. A throwing payload assignment is not
recoverable: the cursor is not advanced (the queue stays structurally
consistent), but a throwing move may already have partially moved-from the
stored payload, so no strong value guarantee is offered.

Threading: exactly one producer may call `try_push`, exactly one consumer
`try_pop` (documented contract, not enforced by locks). The producer is the only
writer of `head_`, the consumer the only writer of `tail_`, which is why no CAS
is needed. Ordering is the minimum the protocol requires: **relaxed** loads of
one's own cursor, **acquire** on the remote cursor at the reuse/availability
gate, **release** when publishing a payload or releasing a slot. The full
happens-before argument (producer publication and slot reuse edges), the
counter-wrap reasoning, and the WHY-NOT sections (`volatile`, CAS, `seq_cst`)
live in `docs/SPSC_MEMORY_MODEL.md`.

Correctness (`tests/spsc_ring_buffer_tests.cpp`, registered in CTest): new queue
empty; pop-empty false; push/pop one; FIFO order; fill exactly `Capacity`;
push-when-full false; pop-after-full; repeated physical wrap-around; interleaved
fill/drain vs a `std::deque` model; payload sequence preservation; structured
messages copied/moved correctly (overloads exercised by a copy/move-counting
type); a 2^20-message one-producer/one-consumer stress where the consumer
verifies every value arrives exactly once in exact order; a **rapid slot-reuse**
stress at `Capacity = 2` (400k messages, so nearly every push overwrites a slot
the consumer just released) checking no loss, no duplication, exact FIFO; a
**multi-field payload** stress at `Capacity = 4` pushing a fixed-size
allocation-free `MarketMessage {seq, price, qty, checksum}` and validating every
field against deterministic functions of `seq`, so a torn or reordered payload
would fail rather than pass; and a differential run feeding identical logical ops
to `MutexBoundedQueue` and `SpscRingBuffer`, checked against each other and the
model at every step. Same exit-code-self-test guard as the Experiment 01
runners.

Phase 1 is correctness only — no false-sharing padding and no remote-cursor
cache are present (both are deliberately deferred to Phase 3 as controlled
optimizations), and Phase 1 itself reported no throughput numbers. The unpadded
`head_`/`tail_` layout *permits and is likely to exhibit* false sharing; **Phase
3A later built a packed same-line control and a separated control and verified
their actual cursor addresses / cache-line placement rather than assuming it
(§ Phase 3A below) — it did not re-use this frozen type as a control, because
adjacency is not proof of same-line placement and no addresses were recorded
here.**

### Phase 2 — Throughput Baseline (COMPLETE / FROZEN)

**Research question:** *how much steady-state end-to-end message-transfer
throughput does the Phase-1 SPSC protocol provide relative to a mutex-serialized
bounded queue under controlled message sizes and capacities?* — asked and
answered by measurement, with no answer assumed in advance.

`benchmark/spsc_throughput_bench.cpp` (built with `-O3 -DNDEBUG` forced by its
CMake target, `BUILD_BENCHMARKS=ON`) runs two threads over one queue at a time:
8/32/64-byte trivially-copyable, nothrow, allocation-free payloads, capacities
1024/4096/65536 selected by a compile-time switch, 10,000,000 messages per
repetition. Thread creation, queue allocation and the expected-checksum pre-pass
all happen **outside** the timed interval; the consumer records its own
completion timestamp after receiving the final message. Both queues stay
non-spinning APIs — the harness supplies backpressure
(`while (!try_push(msg))` / `while (!try_pop(msg))`, busy retry with an
occasional `yield`, no sleep) identically for both implementations, and counts
`producer_full_retries` / `consumer_empty_retries` as observable metrics rather
than failures. Every run must consume exactly N messages, in strictly increasing
order with no gaps or duplicates, with a correct final checksum, or it exits
non-zero and is not published.

`scripts/spsc-throughput.sh` runs the canonical 18-cell matrix (2 impls × 3
sizes × 3 capacities) with **one implementation per process**: 5 measured
repetitions per process after a warm-up repetition that is excluded from every
published figure. The design is a **balanced AB/BA** one — the two
implementations of the same `message_bytes + capacity` run as *adjacent*
processes, and across 4 sessions every cell gets 2 mutex-first and 2 SPSC-first
comparisons, with forward/reverse traversal balancing position in the cell sweep
along the time axis. Each repetition launches a *fresh* producer/consumer thread
pair, so a session is a grouping of repetitions inside one process lifetime and
**not** a fixed thread placement; macOS may migrate threads, and the runner makes
no placement claim. The balance is verified, not assumed: the runner parses the
execution order back out of `command.txt` and fails the dataset unless every cell
got exactly two of each order and every row is `correctness=PASS`. Nothing is
filtered and the best repetition is never selected.

**Measured (Apple M3 Max, 2026-09-11; 72 processes, 360 measured repetitions,
3.6 billion message transfers, all `correctness=PASS`).** Implementation
direction is taken from the **paired per-session** comparison (SPSC session
median ÷ mutex session median), which compares neighbouring processes rather than
pooling correlated repetitions; the pooled matrix is retained as a secondary
descriptive view.

| direction | cells | median paired ratio |
|---|---|---|
| SPSC faster in 4/4 sessions | 64 B at capacity 1024, 4096, 65536 | 0.877, 0.543, 0.649 |
| mutex faster in 4/4 sessions | 8 B / 65536, 32 B / 4096, 32 B / 65536 | 1.970, 1.375, 2.226 |
| inconclusive — direction flipped between sessions | 8 B / 1024 (3–1), 8 B / 4096 (3–1), 32 B / 1024 (2–2) | 0.722, 0.887, 0.958 |

So **6 of 9 cells hold one direction across all four balanced sessions (3 each
way) and 3 are inconclusive** — the frozen unpadded SPSC does **not** dominate
the mutex baseline, and the mutex baseline does not dominate it. Sign consistency
across 4 paired observations is descriptive, not a significance test, and
overlapping observed ranges are reported as *not separable*, never as equivalent.
The two implementations fail in opposite directions: mutex is producer-side
full-queue dominated in 7 of 9 cells, SPSC consumer-side starvation dominated in
8 of 9. Session-to-session movement is larger for SPSC than for mutex in all 9
cells (up to 2.07× between two sessions of the same binary); Phase 2 does not
identify the cause. **Nothing here is attributed to false sharing** — Phase 2 has
no packed/separated control and no cache-line verification.

Full methodology, the matrix, the derivation and the limitations (macOS
scheduling, run-to-run variation, the consumer's message-size-dependent
validation fold, retry overhead, unoptimized baseline) are in
`docs/SPSC_THROUGHPUT.md`; the canonical data, with host/toolchain metadata, the
exact commands in execution order, and the invariant check results, is in
`docs/results/spsc-throughput/`. Two earlier passes are retained, clearly labeled
SUPERSEDED and not to be cited: the fixed-order pass (implementation confounded
with run order) in `docs/results/spsc-throughput-superseded-fixed-order/`, and
the single-session pass in
`docs/results/spsc-throughput-superseded-single-session/`.

### Phase 3A — Controlled Cursor Placement (COMPLETE / FROZEN)

**Research question:** *how much of the SPSC's throughput behaviour changes when
the producer-owned `head` cursor and the consumer-owned `tail` cursor are forced
to share one cache line, compared with the same cursors on distinct cache lines,
holding the algorithm, the payload, **the object footprint and the payload's
offset within the object**, the memory orders, the harness and the machine
constant?* — asked and answered by measurement, with no answer assumed in
advance.

The measurement is a **controlled cursor-placement / coherence-layout**
comparison, not a measurement of "pure false-sharing cost": the two cursors are
not independent write-only state (the producer writes `head` and reads `tail`,
the consumer writes `tail` and reads `head`), so placing them on separate lines
removes one unwanted line-granularity interaction and simultaneously splits two
*legitimately* shared values across two coherence units. Both effects are real
and they point in opposite directions, so a measured difference is consistent
with the placement change without isolating a single mechanism.

`include/spsc_cursor_layout_ring_buffer.h` defines the two Phase-3A controls as
aliases over **one** algorithm body, `CursorLayoutRingBuffer<T, Capacity,
CursorLayoutPolicy>`:

- `SpscSameLineRingBuffer` — both cursors in **one** interference block.
- `SpscSeparatedCursorRingBuffer` — each cursor in its **own** interference
  block.

The single-algorithm-body structure is the point, not an implementation detail.
"Only cursor placement differs" is easy to state and easy to break, and a
copy-pasted second implementation drifts invisibly — both copies still compile
and both still pass their correctness tests. So the claim is made structural:
the layout policy contributes **storage and nothing else**, with no callable
members, so it cannot alter control flow even by accident. The algorithm,
payload storage and indexing, full/empty semantics, memory orders, `noexcept`
specifications, retry policy and message types are identical by construction, and
a 200,000-operation differential test drives both layouts through identical input
and requires observation-for-observation agreement.

**Layout is guaranteed by construction and then verified at runtime.** Each
policy's block size and alignment are `static_assert`ed against
`kAssumedCacheLineSize`, so "both cursors are in one block" is a property of the
type rather than of a lucky allocation. Static layout only guarantees the
*assumed* line size, though, so **every measured repetition** measures the actual
cursor addresses on the queue object it is about to time — outside the timed
interval — and records the reported cache-line size, both addresses, their line
indices, and a `layout_ok` verdict. A `same_line` row that did not measure
same-line, or a `separated` row that did not measure separated, fails the run
rather than being published: an unverified layout is not evidence about layout.

**The two cursor policies have equal footprint, so the payload cannot move.**
The first Phase-3A dataset changed two things at once: the same-line policy was
128 bytes and the separated policy 256, and because the payload array follows the
cursors in the object, the payload began at object offset 128 in one variant and
256 in the other. That is a second object-layout/address-mapping variable on top
of cursor placement, so the dataset is real and self-validating but cannot support
a cursor-placement attribution. (Phase 3A records object addresses and cursor
placement; it does not measure the hardware's cache-set indexing function and
makes no claim about which cache set either offset landed in.)
The hardened design gives **both** policies a
`2 * kAssumedCacheLineSize` footprint: same-line keeps *both* cursors in the first
block and reserves an inert second block that nothing reads or writes, whose only
purpose is to equalize the footprint. Both the policy `sizeof` and the queue
object's `object_size` / `payload_offset_from_object_base` are checked — by
`static_assert` at compile time for the policies, and by a runtime gate that runs
**before any timing** and aborts the process if the two variants disagree. The
canonical runner re-checks the same invariant four independent ways over the
published raw data and fails rather than writing its result files.

**The cache line is 128 bytes on the canonical host, not 64.** This matters more
than it looks. Assuming 64 would put the "separated" blocks inside one real line,
silently turning the separated control into a same-line control and reporting the
**opposite** of the truth with no assertion firing anywhere. The benchmark
therefore queries the host at runtime (`sysctlbyname("hw.cachelinesize")`) and, if
the reported size exceeds the compile-time assumption, exits non-zero **before
timing anything** — there is no warn-and-continue path, and no cell is published.
The compile-time assumption only creates the *candidate* layout; the authoritative
statement is always the measured address relationship on the running host, and a
compile-time assumption larger or smaller than the host's real block does not by
itself imply any particular outcome.

`benchmark/spsc_false_sharing_bench.cpp` reuses the proven Phase-2 harness
(one producer, one consumer, fixed-size messages, timing outside thread creation,
correctness/checksum gating, consecutive-miss yield policy, raw repetitions
preserved, one implementation per process), and
`scripts/spsc-false-sharing.sh` runs the canonical matrix: 2 layouts × 3 message
sizes × 3 capacities = 18 cells per session, 4 sessions, 72 processes. The design
is **balanced AB/BA** — the two layouts of a pair run as adjacent processes, with
both layout order and traversal direction balanced so every cell gets 2
same-line-first and 2 separated-first comparisons, and the balance is **verified**
by parsing the execution order back out of `command.txt`, not asserted from the
design constants.

The primary figure is the paired per-session ratio (**separated median ÷
same_line median**; `< 1` means separated was faster).

**Measured (Apple M3 Max, 2026-09-12; 72 processes, 360 measured repetitions,
3.6 billion message transfers, `correctness=PASS` and `layout_ok=PASS` on every
row, equal object size and equal payload offset across the two layouts in all 9
cells).** The direction is **not uniform**, and the headline result is *not*
"padding is faster":

| region | result |
|---|---|
| 8 B, capacities 1024 / 4096 / 65536 | separated faster **4/4 sessions** each; ~2.8×–4.0× (median ratios 0.248 / 0.329 / 0.353) |
| 64 B, capacities 4096 / 65536 | separated faster **4/4**; ~1.6×–1.7× (0.615 / 0.588) |
| 32 B, capacity 4096 | **same-line faster 4/4**; ~1.4× (1.426) |
| 32 B, capacities 1024 / 65536 | **inconclusive** — direction flipped between sessions (3–1, 2–2) |
| 64 B, capacity 1024 | **inconclusive** — direction flipped (3–1, median 0.915, all four ratios within ±18% of parity) |

So **6 of 9 cells hold one direction across all four balanced sessions — 5
separated-faster and 1 same-line-faster — and 3 are inconclusive**; across all 36
paired observations, 28 favour separated and 8 favour same-line. The balanced
AB/BA order is what makes this readable: in every stable cell the ratio ranges
from the same-line-first sessions and from the separated-first sessions overlap,
so the direction tracks the layout rather than the run position — and in the
split cells the same check exposes that the direction does not survive the swap.
Under the
Phase-3A wording rule, a separated-faster result may be described as **consistent
with** reduced false-sharing interference — which covers the five stable
separated cells — but not as proving that false sharing caused the whole measured
difference: the harness has full/empty retry feedback, occasional `yield()` and
OS scheduling between the two processes, so a smaller low-level effect can be
amplified into a larger end-to-end difference, and the cursors also carry
*required* true sharing that separating them splits across two coherence units.
The 32 B / 4096 same-line-faster cell is a real, reproducible result that the
false-sharing story does not predict; Phase 3A does **not** identify what drives
it and collects no profiling evidence that could.

The first dataset is retained unedited at
`docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/` with a
`SUPERSEDED.md`, and **must not be cited**: there the same-line cursor policy was
128 bytes and the separated policy 256, so the payload array's relative offset
within the object differed by 128 bytes between the variants — an uncontrolled
object-layout shift that rode along with the cursor placement.
Equalizing the footprint moved per-cell median ratios by up to ~24% in both
directions and changed one cell's sign, but the two datasets also differ by run,
so that comparison is reported only as a secondary methodology observation and is
not a correction that can be applied to the older numbers.

Full methodology, the derivation and the limitations are in
`docs/SPSC_FALSE_SHARING.md`; the canonical data — raw repetitions, per-process
summaries, runtime layout verification for every measured repetition, the
equal-footprint evidence, the invariant report and `PROVENANCE.md` — is in
`docs/results/spsc-false-sharing/`.

**Phase 3A changes exactly one program-layout treatment: cursor placement.** That
is what "one variable" means here, and it is a real control — the two queue types
are one algorithm body parameterised by a storage-only cursor policy, so the
algorithm, payload storage and indexing, memory orders, retry policy and message
types are identical by construction. It does **not** mean the two legs' processes
are identical in every respect: they are independent processes with independently
allocated objects, so absolute addresses, scheduler placement, DVFS and thermal
state and background-system state all differ and are **not** eliminated by
construction. Those nuisance variables are addressed by the design — adjacent
process pairing, balanced AB/BA ordering and four repeated sessions — and by the
directional-stability criterion, not by construction. No cached `head`/`tail`, no
batching, no memory-order change, no CAS, no affinity: those are Phase 3B and
later, and combining any of them here would destroy the attribution this dataset
exists to support. The frozen Phase-1 `SpscRingBuffer` was not modified and is **not** one
of the two controls: it is unpadded, but adjacency is not proof of same-line
placement and Phase 2 recorded no cursor addresses, so it can make no cache-line
claim. It is available as `--impl=natural` for observational use only.

### Phase 3B — Remote Cursor Caching (COMPLETE / FROZEN)

**Research question:** *how much does SPSC throughput change when each thread
caches the opposite thread's cursor and refreshes it only when the cached value
is insufficient to prove that progress is safe?* — asked and answered by
measurement.

Phase 3B re-runs the **separated** layout only — the layout Phase 3A verified —
and adds one thing: a thread-owned, **non-atomic** cached copy of the remote
cursor. The producer owns `cached_tail`, the consumer owns `cached_head`, and
neither is synchronized; each is read and written by exactly one thread.
`include/spsc_remote_cursor_ring_buffer.h` defines both variants as aliases over
**one** algorithm body, `RemoteCursorRingBuffer<T, Capacity, RemoteCursorMode>`,
selected by `if constexpr`. A stale cached value can only ever produce a **false
full** (producer) or a **false empty** (consumer) — it can never make the
producer believe more capacity was released than really was, nor the consumer
believe unpublished data exists. The producer still publishes with a release
store to `head` and the consumer still reads the payload only after an acquire
observation of `head`: **the release/acquire edge is not removed, only made less
frequent.** Both cached values start at 0, matching the initial queue state, so
construction performs no remote acquire load.

**Cursor placement, footprint and payload offset are held identical by
construction, not by convention.** Both variants use the Phase-3A separated
placement (`head` and `tail` on distinct verified cache lines) and both are
`2 * kAssumedCacheLineSize` = 256 bytes of cursor state, so the payload offset
is 256 in both. The cached values are stored *with their owner's state* — the
producer's `cached_tail` on the producer's line, the consumer's `cached_head` on
the consumer's — so the addition introduces no new cross-thread line-sharing
relationship. `static_assert`s check size, alignment, cursor offsets and
owner-line placement; a runtime gate **before any timing** re-checks the same
properties on the actual object and aborts rather than publishing. Every
measured repetition then records the addresses it is about to time, outside the
timed interval: **all 360 canonical rows report `layout_ok=PASS`,
`cached_placement_ok=PASS` and `cursors_same_line=no`.**

**Instrumentation is compile-time eliminated, and the canonical numbers are
uninstrumented.** The two counters (`producer_remote_tail_loads`,
`consumer_remote_head_loads`) are ordinary non-atomic members owned by one
thread each — no global atomic enters the hot path to count anything — and the
increment sites are behind `if constexpr (Instrumented)`. The counter storage
stays in the type so the object layout is identical either way, but the
uninstrumented build emits no increment at all, and the runner fails the run if
any canonical process reports non-zero counters. Mechanism counting therefore
comes from a **separate 18-process instrumented leg** that is a different binary
instantiation and is excluded from every canonical figure.

**Methodology is the proven Phase-2 / Phase-3A design, unchanged:**
`scripts/spsc-remote-cursor.sh` runs 2 variants × 3 message sizes × 3 capacities
= 18 cells per session, 4 sessions, **72 processes**, 10M messages, 1 excluded
warm-up, 5 measured repetitions; the two variants of a pair run as adjacent
processes with variant order and traversal direction balanced (2
baseline-first + 2 cached-first per cell), and the balance is **verified by
parsing the execution order back out of `command.txt`**. The retry/yield policy
is the hardened Phase-2 one, byte for byte.

The primary figure is the paired per-session ratio (**cached median ÷ baseline
median**; `< 1` means cached was faster).

**Measured (Apple M3 Max, 2026-09-12; 72 canonical processes, 360 measured
repetitions, `correctness=PASS` on every row, equal object size and equal
payload offset across the two variants in all 9 cells).** The result is
**negative**, and in the way that matters most for the causal language:

| region | result |
|---|---|
| 8 B, capacities 1024 / 4096 / 65536 | baseline faster **4/4 sessions** each (median ratios 1.087 / 2.005 / 1.103) |
| 64 B, capacities 1024 / 4096 / 65536 | baseline faster **4/4** each (1.270 / 1.406 / 1.569) |
| 32 B, all three capacities | **inconclusive** — direction flipped between sessions (3–1, 3–1, 2–2) |

So the counts are **baseline faster 6 / 9, cached faster 0 / 9, inconclusive
3 / 9**: six cells hold one direction across all four balanced sessions — all six
baseline-faster — and the three 32 B cells are inconclusive. Across all 36 paired
observations, 32 favour the baseline and 4 favour the cached variant. **No cell
is stably cached-faster.**

The mechanism, by contrast, did exactly what it was designed to do. The primary
mechanism metric is **remote loads per try attempt** — `remote_loads /
(message_count + retries_on_that_side)` — because that is the quantity the
treatment actually changes: how often a *try* must look at the remote cursor. On
that metric the baseline is **exactly 1.000000** on both sides in every cell, by
construction, and **the cached variant is below 1.000000 on both sides in every
cell** — it never refreshes more often per attempt than the baseline (worst case
**0.996582**, 32 B/65536 consumer). End-to-end **loads per message** is a
secondary metric and is confounded, because `loads/message = loads/attempt ×
attempts/message`; where it rises, the cause is **retry volume**, not a higher
per-attempt refresh rate:

- At **8 B** the *producer's* loads/message fall by 17×–70,102× while the
  *consumer's* rise 1.9×–4.1×.
- At **32 B** the producer's fall (3×–65,806×), but the consumer's change is
  **not one-directional**: its loads/message *decrease* at 1024 (0.72×) and 4096
  (0.84×) and *increase* slightly at 65536 (1.08×).
- At **64 B** it reverses: the *consumer's* loads/message fall by
  1,156×–44,910× while the *producer's* rise 2.7×–3.1×.

Which side is waiting flips with message size — and **the side that accumulates
retries is the side being held up, so the pace-limiting side is the other one.**
At 8 B and 32 B the consumer's `empty` retries dominate, so the consumer is
waiting and the **producer** is pace-limiting; at 64 B the producer's `full`
retries dominate, so the producer is waiting and the **consumer** is
pace-limiting. The mechanism leg also shows why caching cannot help the waiting
side: a thread that keeps finding the queue genuinely empty (or genuinely full)
sees a remote cursor that has not moved, so every failed attempt refreshes anyway
— the baseline's count, plus a comparison. That is a property of the design,
reported rather than smoothed over, and it is what makes those seven cells'
*loads/message* rise on one side.

**The emphasized case is real and it is not small — but the size of the load
reduction does not predict the size of the penalty.** 8 B/65536 has one of the
largest reductions (~70,102×, producer) and only a 1.103× regression; 8 B/4096
has a far smaller reduction (~57×) and the **largest regression in the dataset,
~2.005×**; 64 B/65536 reduces consumer loads ~44,910× for a 1.569× regression.
Reduction magnitude alone therefore does not order the throughput outcome. The
message-size relationship is likewise **not monotonic in message size**: 32 B is
small and directionally unstable across sessions; 64 B is consistently moderate
(1.270× / 1.406× / 1.569×) across all three capacities; 8 B is stable per
capacity but strongly capacity-dependent (1.087× / **2.005×** / 1.103×) and
contains the largest single regression. Nor is capacity irrelevant — it
materially changes the magnitude at every message size, even though it does not
determine the verdict. Under the Phase-3B causal rule, the valid statement is
that **remote-cursor caching reduced explicit remote cursor observations per try
and was associated with *worse* end-to-end throughput under this workload**. No
cache-miss, coherence-transaction or cache-line-transfer count was measured, and
none is claimed.

Two limits must travel with these numbers. First, the mechanism counts are
end-to-end totals for the whole run, so part of the losing side's *increase* is a
consequence of the slowdown rather than a cause of it, and the instrumented leg
is a different instantiation whose regime can differ from the canonical leg's —
so no "loads saved per nanosecond" arithmetic is performed anywhere. Second, as
in Phase 3A, **this cell shape exhibits strong run-to-run and build-to-build
regime variation on the development host**. The observed variation is large
enough that absolute `ns/message` values from independently built phases must not
be interpreted as treatment effects, and it is **comparable to or larger than
several of the within-phase treatment differences reported above** — which is why
the direction calls here rest on the balanced paired design and its per-session
agreement, never on a difference in absolute level. A separate diagnostic, run
outside this dataset, suggested code-layout sensitivity as **one possible
contributor** to that variation, but **Phase 3B does not isolate its cause**, and
no reproducible diagnostic package is preserved alongside the data. Both Phase-3B
variants do come from **one executable built under the same compiler and the same
options**, which supports build/toolchain comparability — but each variant is a
**distinct template instantiation with its own emitted machine code**, and each
leg runs in an **independent process** whose scheduler placement, core type,
migration history, DVFS, thermal state and background load are not guaranteed to
match. That is neither "same compiled code placement" nor "same machine regime".
The balanced adjacent AB/BA design mitigates temporal ordering bias; it does not
guarantee identical machine state. See `docs/SPSC_REMOTE_CURSOR_CACHE.md` §7.5.

Full methodology, the correctness argument, the counter-wrap reasoning and the
limitations are in `docs/SPSC_REMOTE_CURSOR_CACHE.md`; the canonical data — raw
repetitions, per-process summaries, paired session summaries, per-repetition
layout verification, the separate mechanism leg, the invariant report and
`PROVENANCE.md` — is in `docs/results/spsc-remote-cursor/`.

### Phase 4 — Tail Latency / Jitter (COMPLETE / FROZEN)

A **measurement** phase over **one** queue: the frozen separated-cursor baseline
(monotonic cursors, separated head/tail cache lines, acquire/release publication,
baseline remote-cursor loads with **no** remote-cursor cache). It
compares **no treatments** — the Phase-3B `cached` variant, `MutexBoundedQueue`
and the Phase-3A `same_line` layout are absent by design — and it opens **no new
optimization**. Every number in it is attributable to a single configuration.

- **Shape.** 9 cells (16/32/64 B × 1024/4096/65536), 10M messages per
  repetition, a 100,000-message settling prefix, a **sequence-keyed sparse**
  sample schedule at the deliberately **odd** interval **1021** (coprime with
  every power-of-two capacity, so samples rotate through all ring positions — it
  is not 1024), **9,696 samples per repetition**, one excluded warm-up plus 5
  measured repetitions per process, 4 sessions in
  forward/reverse/forward/reverse order, **36 processes, 180 measured
  repetitions, 1,745,280 sampled latencies**.
- **The instrumentation is sparse, and the dataset proves it.** The producer
  stamp is taken **once per sampled message**, immediately before that message's
  `try_push` retry loop, and the consumer reads the clock **once per sampled
  message**, immediately after a successful `try_pop` and before any validation.
  Every repetition records `expected_samples == sample_count ==
  producer_sample_clock_reads == consumer_sample_clock_reads == 9,696` and
  `stamp_contract_failures == 0`, so **1,745,280** sampled clock reads per thread
  for the whole dataset instead of **1,800,000,000** — 0.097%. A harness that
  timestamped every message would report ~10,000,000 in those counters and
  **fail**, not silently produce the same 9,696 latencies. The stamp travels
  **inside** the sampled message along the SPSC payload path; there is no
  side array and therefore no second, capacity-dependent memory footprint in the
  measured path. The advantage is **structural, not that the stamp is free**: a
  sampled message still has its `ready_ticks` field written by the producer and
  read by the consumer. What the in-message transport buys is that the stamp
  **reuses the existing payload publication path** instead of adding a second
  independently addressed producer-to-consumer shared-memory path.
- **The measured quantity** is `producer_ready → consumer_received` on
  `steady_clock`, stamped immediately **before** the producer's `try_push` retry
  loop and immediately **after** a successful `try_pop`. It therefore
  **includes** backpressure, the release/acquire edge, the payload copy, the
  consumer's empty-retry loop and both clock reads. It is **not** pure queue
  residence time, **not** a per-call cost and **not** a one-way handoff latency,
  and it is **not comparable** to any `ns/message` figure in Phase 2, 3A or 3B.
  Because the stamp precedes the push, the interval depends on how far ahead of
  the consumer the producer has run: this is a **producer-relative service
  latency including backpressure**.
- **Aggregation is nested, and the PRIMARY figure is session-blocked**:
  repetition → median of a session's 5 repetitions → **median of the 4 session
  medians**. The median across all 20 repetition-level statistics is kept as a
  **labelled diagnostic only**. These are **two different estimands computed by
  two different aggregation rules** — a median of four session medians against a
  median of twenty repetition-level statistics — so they **need not be equal**,
  and neither is a weighted median of the other's inputs. They must never be
  presented as one number. `CELL_SESSION_BLOCKED.csv` carries both plus the
  min/max session median each blocked value sits inside.
- **Session blocking is a process/time grouping, not a thread-placement
  claim.** The five measured repetitions inside one session share one process and
  address-space lifetime, one binary/build and a short temporal block; they do
  **not** share a verified fixed CPU or thread placement. Each repetition
  launches a fresh producer/consumer thread pair, **no affinity is set anywhere**
  in this experiment, and the OS may migrate either thread mid-run. Sessions
  collapse process-and-time clustering; they do not pin cores.
- **Result — a stable bimodality, not an unstable mode.** The nine cells split
  cleanly. Six (all 16 B and 32 B, every capacity) have a session-blocked
  **P50 of exactly 125 ns — constant in all 120 of their repetitions**, spread
  1.00×. Three (all 64 B) sit at **30.0 µs / 120.4 µs / 1.95 ms**, one per
  capacity, stable to 1.05–1.09× across sessions. No cell straddles the two
  groups. P50 is now the **most** reproducible statistic (worst-case spread over
  a cell's 20 repetitions: 1.11×) and P99.9/max the least (up to 19,271×),
  because the latter are single observations.
- **The fast band is timer-resolution-limited.** The clock quantum is **41.7 ns**
  and 125 ns is three quanta; 92–95% of the six fast cells' samples lie within
  four quanta of zero and 53–72% are exactly 125 ns, while 0.00% of the three
  slow cells' samples are within a microsecond. Timer **calibration** (200,000
  clock pairs per process) provides scale and context, **never a correction
  factor** — it is never subtracted from a latency, and the tick→ns conversion is
  exact or the build fails. What is claimed is "timer-resolution-limited", never
  "the latency is the timer, not the queue": the queue is doing real work at a
  scale this clock bounds but does not resolve.
- **Capacity.** The two groups differ in **which thread waits**, and the retry
  counters establish that directly: the side that accumulates retries is the side
  being held up, so the *other* side is pace-limiting. In the fast (16 B / 32 B)
  cells the **consumer** spins empty 0.93–2.5 **billion** times while the producer
  almost never finds the ring full — dominant consumer-empty retries mean the
  consumer is waiting for the producer, so the **producer** is pace-limiting, and
  the latencies are **consistent with an often-empty / low-backlog regime**. In
  the 64 B cells it inverts — the producer is blocked 26–204 **million** times
  and the consumer spins empty only 12 thousand to 3.9 million times, so the
  **consumer** is pace-limiting and the latencies are **consistent with a full /
  near-full backlog regime**. "The producer is blocked" is **not** the same
  statement as "the producer is the bottleneck". **Occupancy is not directly
  recorded** — there is no queue-depth or producer-lead instrument — so those two
  regime descriptions are readings the counters and latency levels support, not
  measured occupancies. Within the slow band a larger capacity means **fewer**
  producer stalls but a **longer** measured latency — P50 is **≈ 0.95 of one full
  ring's drain time** (`capacity × ns_per_message`, 0.946–0.959) in all three,
  which is **consistent with** a one-ring drain/backlog model in which the
  producer is able to run ahead and a sampled message waits behind roughly one
  ring of queued work. It **neither proves that model nor is proved by it**: the
  model is a hypothesis the numbers fit, occupancy and producer lead are not
  instrumented, and **no** cache, coherence, scheduler or core-placement cause is
  asserted for it.
- **The size axis is a message *shape*, not a payload byte count.** 16 / 32 / 64 B
  are three distinct message types that differ in their per-message deterministic
  construction and validation work as well as in width, and this design does
  **not** separate the two — size and work are one **bundled workload
  dimension**. So no claim is made that a 64-byte *payload size* causes the slow
  regime; what is claimed is that **the 64-byte benchmark message shape
  consistently produced the consumer-limited regime in this harness**.
- **Extremes are mostly isolated, with one named exception.** In seven of nine
  cells 1–3 of the 20 repetitions have a maximum above 5× the cell's own median
  maximum. **32 B / 65536** is the exception — **7 of 20**, including three
  consecutive repetitions inside a single process (1.85 ms, 3.62 ms, 20.4 ms).
  Reported and **not censored**.
- **Session order** balances a cell's temporal position only. It is **not** an
  AB/BA crossover (there is no treatment pair), and it removes neither scheduler
  variation nor core migration, DVFS or thermal drift — none of which was
  measured.
- **No causal attribution is made anywhere.** Nothing was profiled: no
  cache-miss, coherence-event, preemption, scheduler, core-migration,
  P-core/E-core, frequency or thermal quantity was measured or may be inferred.
- **Verified.** All 180 repetitions `correctness=PASS` with strict sequence and
  payload validation, **0** timestamp inversions, **0** stamp-contract failures,
  the per-message-size checksum identical across all 20 repetitions of each size,
  cursor layout verified on the measured object in every process, and
  **5,240,804** raw→summary checks passed with 0 failures.
- **Two hazards** are documented in the methodology because a methodology that
  lists only what passed is not one. **H1** — a timestamp-slot aliasing race that
  produced an impossible negative sample. It belonged to the earlier
  side-array transport, which no longer exists: the current design has no second
  address to alias, and the historical reasoning is preserved only in the
  superseded dataset's `SUPERSEDED.md`. What replaced it as the invariant is
  `stamp_contract_failures`, **0** across all 180 repetitions. **H2** — an
  earlier complete run that passed **every** gate while being measured
  concurrently with other work on the same host. Since Phase 4.1 the
  per-repetition `ns_per_message` check **reports rather than invalidates**: a
  repetition above 5× its cell's median is raised as a prominent diagnostic
  WARNING / contamination *candidate* and does not by itself fail the dataset,
  because Phase 4 studies latency and jitter and a legitimately rare stall must
  not be censored for being extreme — **magnitude alone is not proof of
  invalidity**. Independent evidence (a known competing workload) is what
  disqualified that run; it is preserved, labelled, as
  `docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/` with its derived
  tables deleted. That directory is **not committed** — 106 MB of raw data for a
  run that cannot be used — so a clone keeps only the description of it in
  `docs/results/README.md`. On the canonical dataset this warning **did not
  fire** (worst cell spread 3.01×).
- **Superseded.** An earlier canonical pass used the same nine cells but
  timestamped **every** message and kept stamps in a separate
  `ready_ticks[2 × Capacity]` array — a capacity-dependent second memory path,
  and instrumentation cost on all 10M messages instead of the 1-in-1021 that is
  sampled. It is preserved real and unedited at
  `docs/results/spsc-tail-latency-superseded-per-message-timestamp/`. **Its
  percentiles are not Phase-4 results and must not be cited as such**, its
  findings are not assumed to reproduce, and the difference between the two
  datasets is not attributable to instrumentation as a controlled factor.

Methodology: `docs/SPSC_TAIL_LATENCY.md`. Canonical data, provenance and derived
tables: `docs/results/spsc-tail-latency/`.

Phases NOT STARTED: none for Experiment 02.

## Experiment 03 — Market Data Pipeline

**Status: Phase 1A — Sequencer / Snapshot / Recovery Correctness: COMPLETE /
FROZEN. Phase 1B — Binary Protocol / Decoder Correctness: COMPLETE / FROZEN.
Phase 1 overall: COMPLETE / FROZEN. Phase 2 — Decoder Thread → SPSC → Book
Thread: NOT STARTED.** No clocks, no timing, no benchmark binary. Methodology:
`docs/MARKET_DATA_PIPELINE.md`; wire protocol and decoder contract:
`docs/MARKET_DATA_PROTOCOL.md`.

Experiments 01 and 02 built a correct L2 book and a correct SPSC transport.
Neither answers the question a feed handler faces on every session: **what does a
consumer do when the sequenced stream it is reading is lossy?** A book that has
silently missed a message is not a slightly stale book, it is a *wrong* book, and
no amount of careful updating afterwards repairs it. Phase 1A is the policy for
detecting that, refusing to pretend otherwise, and accounting for it. Phase 1B
is the layer beneath it: the bytes arrive on a socket, and something has to turn
them into messages before any sequencing question can be asked at all.

Phase 1A is implemented against the specification sketch it was given, verbatim:

```
SnapshotBegin seq=1 / Bid 100 qty=10 seq=2 / Ask 101 qty=20 seq=3 / SnapshotEnd seq=4
Bid 100 qty=15 seq=5 / Ask 101 qty=0 seq=6
seq=9   <- gap
seq=10  <- rejected while GAP
SnapshotBegin seq=20 / ... / SnapshotEnd  -> LIVE again
```

### The seam: framing and sequencing vs snapshot validity

**The pipeline owns framing and sequencing. The book owns snapshot validity and
application.** Snapshot content accumulates into a staging `llob::BookSnapshot`;
at `SnapshotEnd` the pipeline hands it to the sink's own `load_snapshot()`, so
`validate_snapshot()` governs snapshots here exactly as it does on every other
snapshot path in the repository. **There is one snapshot contract, not two.**

In-order levels are **delegated to `book.apply()`**, not re-decided by hand. This
is not a convenience: two of the book's five outcomes are invisible to a sequence
comparison alone. `InvalidUpdate` (`qty < 0`) makes the book *unsync itself*, and
`OutOfRange` *consumes the sequence* while the book stays synced. A pipeline that
only compared sequence numbers would report `Live` forever over an unsynced
book — a silent permanent stall with no gap ever firing to reveal it.

### States and outcomes

Four states — `NotSynced` / `Live` / `Snapshot` / `Gap`. `NotSynced` is kept
distinct from `Gap` because the two cost different things: a cold start that
never synced has an outage of unknown length, while losing a live view has a
measurable one. `Gap → NotSynced` is impossible (`ever_synced_` is never
cleared). Ten outcomes — `Applied`, `OutOfRange`, `Staged`, `SnapshotCommitted`,
`SnapshotAbandoned`, `Stale`, `GapDetected`, `Malformed`, `Rejected`,
`ProtocolViolation` — deliberately richer than `ApplyResult`, which conflates
"you sent me a duplicate" with "my view is unusable" into one `Stale`.

Two details the sketch exists to pin, both asserted literally: the offending
`seq=9` is `GapDetected` and **not applied** — the sequence is never consumed, so
`expected()` is still 7 and the gap cannot be silently papered over — and the
`seq=10` that follows is `Rejected` because the pipeline is in `Gap`.

### Stale snapshots never rewind

`load_snapshot()` has no staleness check of its own and will happily rewind a
book to an older sequence; committing a stale bracket would move `last_applied`
backwards and then double-apply every message in between, with no gap firing to
reveal it. So the pipeline carries its own gate, `snapshot_fresh(seq) = seq >=
expected()` — **against the watermark, not against the last sequence consumed.**
Those differ by exactly one while `Live`, and the difference is the whole point:
`last_applied_seq()` is the last sequence already *used*, so gating on it accepts
a `SnapshotBegin` numbered at a sequence we have already consumed, and a frame
behind the watermark is stale whatever its kind. Because `expected()` is derived,
this is one rule with one meaning in every state, and it is applied in **every**
state — including `Gap`, where a repair bracket older than the view that was lost
is not a repair at all. One consequence looks surprising and is pinned by
scenario vectors: because a gap does not advance the book's cursor, a repair
bracket must begin at `cursor + 1` or later, and one beginning *exactly at* the
lost cursor is refused.

### Recovery accounting (counts, never durations)

One `MdRecoveryEpisode` per outage: cause, `first_missing_seq`, the sequence that
detected it, the span it covered, the highest sequence seen while it was open,
how many levels that cost (`discarded`), how many repair brackets were accepted
(`attempts`), and the committing `End`. Outage size is in **messages and sequence
numbers**; there is no duration anywhere in this phase. `first_missing_seq` is
read from the **book**, never from the bracket that failed — a mid-stream refresh
bracket carries its own cursor that runs ahead of the book's, so reporting that
one would understate the outage by every sequence staged inside it. An outage
already open is never reopened: it keeps the position and cause of the loss that
opened it and only accumulates state.

**The accounting identity holds by construction**, not by discipline:
`messages == the sum of the ten outcome counters`, asserted over the whole
corpus. Every result is built in one private `note()` function, and that function
is the only place an outcome counter moves, with a switch exhaustive over
`MdOutcome`.

### Verification, and what each layer cannot do

Six layers, in `market-data-pipeline/tests/`:

- **The specification sketch verbatim** as a literal expected-outcome table — 13
  messages, 13 expected outcomes read off the sketch rather than off the
  implementation. This is the **only** layer that can catch a *specification*
  error, because its expectations were written by hand.
- **28 hand-written scenario vectors**, one per failure hypothesis, each with a
  full expected outcome vector *and* an expected final state, `expected()` and top
  of book: the sketch; an empty snapshot; `SnapshotEnd` as the very first message;
  a snapshot that never ends; a gap inside a bracket; a nested `SnapshotBegin`;
  replays and duplicates while `Live`; a duplicate price inside snapshot content;
  an out-of-domain price on both paths; negative `qty` on both paths; a stale
  `SnapshotBegin` while `Live`; a bracket below the lost cursor and one exactly at
  it; and a `ProtocolViolation` leaving a healthy book intact.
- **Accounting**: the identity, the episode fields by hand, and — the layer that
  keeps the fuzz honest — a **corpus coverage assertion** that every one of the
  ten outcomes, nine diagnostics, four states and four loss causes is actually
  reached. Differential agreement on a counter that is permanently zero proves
  nothing about it.
- **Differential fuzz**: 1550 seeded traces against
  `tests/md_oracle.h`, an independently written reference implementation. It
  shares the vocabulary and **none of the logic** — a different book, different
  staging, hand-derived snapshot validity rather than `validate_snapshot()`, the
  `apply()` precedence chain written inline, and one flat `if`/`else if` chain.
  **It catches implementation divergence, not specification error**: if both
  misread the contract they agree and are both wrong. That is what the vectors
  above are for, and it is not hypothetical — see below.
- **Sink agreement**: 300 traces through `MapOrderBook` and `FlatOrderBook`,
  requiring identical outcomes, counters, position, top of book **and full
  level-set parity**, because agreement on the top of book is not agreement on
  the book.
- **Generator determinism**: `std::mt19937_64` is exactly specified and is used;
  `std::uniform_int_distribution` is **not** — its mapping is
  implementation-defined, so the same seed can differ between libstdc++ and
  libc++ — and is therefore avoided in favour of a hand-rolled `uniform_below`.
  Failures print the recipe, so any divergence can be frozen into a named
  regression.

The harness was itself checked by deliberately sabotaging the oracle and the
pipeline and confirming each break is caught (an off-by-one on the freshness
gate; `first_missing_seq` taken from the bracket; a wrong staging-cap comparison;
the historical counting omission).

### Defects found and fixed during this phase

Recorded because they are evidence about the verification, not just the code.

- **The accounting identity did not hold.** `SnapshotBegin` returned `Staged`
  without incrementing `staged`, and three stale paths returned `Stale` without
  incrementing `stale`.
- **The differential fuzz could not see it** — the oracle had been written from
  the pipeline and inherited the same omission, so all 1550 traces agreed while
  both were wrong. This is the layer-4 limitation, caught in practice by layer 3.
  It is why outcomes are now counted in exactly one place.
- **`attempts` double-counted**, so a two-bracket outage reported three.
- **`first_missing_seq` was taken from the bracket, not the book** — 7 where the
  book's real position was 5.
- **`discarded` did not implement its own documented definition**: staged levels
  thrown away by an abandoned repair were never charged to the outage.
- **Two generator mutations tested nothing.** `NegativeQty` and
  `OutOfDomainPrice` targeted the first `Level` in a trace, which is always
  inside the opening snapshot — so they exercised snapshot refusal while
  `malformed` and `out_of_range` stayed permanently at zero. The coverage
  assertion found this; the differential fuzz could not have.
- **The freshness gate was one sequence too lenient.** `snapshot_fresh` compared
  against `book.last_applied_seq()` — the last sequence *consumed* — instead of
  `expected()`, the watermark. Those differ by exactly one while `Live`, and the
  gate therefore accepted a `SnapshotBegin` numbered at a sequence already used.
  Found in review, after the phase had been declared complete.

  **The differential fuzz reported full agreement — 1550 of 1550 traces — with
  the defect present**, because the oracle had been written from the pipeline and
  inherited the same off-by-one. That is the first bullet's failure mode
  occurring a *second* time, in a phase whose own documentation already named it,
  and it is why the freshness boundary is now pinned by paired scenario vectors
  in `Live` and in `Gap` rather than left to the corpus. The fix was
  re-validated by reverting both implementations together and confirming the fuzz
  still reports `[ok]` while the vectors fail 7 of 112 — the harness proving its
  own limits rather than asserting them.

### Two inherited asymmetries, documented rather than unified

- **A repeated price inside a snapshot rejects the whole snapshot**
  (`validate_snapshot` treats it as ambiguous state) — including a price listed
  twice where one listing is `qty == 0`. The same price on *opposite* sides is
  ordinary.
- **An out-of-domain price behaves differently on the two paths**: inside a
  snapshot it rejects the run; as an in-order incremental it is ignored with the
  sequence consumed (`ApplyResult::OutOfRange`). Both are correct for their own
  path; they are not unified, and this section does not claim they are.

### What cannot be claimed

- **No performance claim of any kind.** Nothing is timed, no benchmark binary
  exists, and no number in this section or in the methodology doc is a duration.
  Phase 1 is not evidence about throughput or latency and must never be quoted as
  such.
- **No claim about a real venue's feed protocol.** Real feeds have per-instrument
  sequence channels, heartbeat and timeout semantics, and cancel/replace ordering
  rules — none modelled. **The pipeline does not request a resync**; it detects a
  gap and waits for a snapshot. That is a stated policy gap, not an oversight.
- **No claim of bounded recovery time.** It waits for a snapshot; how long that
  takes is a property of the venue.
- **No allocation or memory measurement.** Staging is bounded and exceeding the
  bound **abandons** the bracket rather than truncating it, because a truncated
  snapshot is a wrong snapshot — but nothing was measured.
- **The fuzz corpus is not exhaustive.** It is a fixed catalogue plus seeded
  composition; it explores combinations, it does not prove absence.

### Phase 1B — the wire decoder: bytes -> typed messages

Phase 1A's input was a typed `MdMessage` built by a fixture. A real feed handler
starts one layer lower, with a byte span from a socket read that may hold three
messages, two and a half messages, or zero. Phase 1B is that layer and **nothing
else**: raw bytes in, the same `MdMessage` values Phase 1A already consumes out.
No Phase-1A file was modified, no counter, state or transition was added to the
sequencer, and no Phase-1A test was changed — the frozen state machine cannot
tell whether its input came from a fixture or from bytes.

#### The synthetic protocol

Deliberately simple, and not FIX, ITCH or SBE. A fixed **12-byte header** —
`message_type` (1 byte), `version` (1 byte, currently 1), `payload_length`
(2 bytes) and `sequence` (8 bytes) — followed by an optional payload. **All
multi-byte integers are big endian**, two's complement for signed fields. Three
message types: `SnapshotBegin` (1, 12 bytes), `Level` (2, 29 bytes, with a
17-byte payload of side / `price_tick` / `quantity`) and `SnapshotEnd` (3, 12
bytes).

**The wire format is byte offsets, not a struct.** Padding, alignment and member
order are compiler implementation details, and a protocol should not inherit
them; a packed struct would trade that for unaligned loads, which are undefined
behaviour on strict-alignment targets. Every field is assembled by explicit
shifts and masks — no `reinterpret_cast` to an integer pointer, no packed struct,
no unaligned load, no host-endian assumption.

#### The one invariant that matters

```
consumed != 0   if and only if   status == Ok
```

On `Ok`, `consumed` is exactly the encoded size of the message decoded — never
more, so the caller can advance without re-deriving the length; never less, so it
cannot stall on a message it already consumed. On **every** failure, `consumed`
is 0 and the output message is **untouched**. A partial `MdMessage` is never
published. That matters more than it looks: `NeedMoreData` — the ordinary answer
for a partial socket read — is the case that happens constantly in production,
and a caller that trusted a partially written message would apply half a level
update to a live book.

`NeedMoreData` is therefore **not an error**; it is the only status meaning "call
me again". Of the eight statuses, the six beyond `Ok` and `NeedMoreData`
(`InvalidVersion`, `InvalidType`, `InvalidLength`, `InvalidSide`, `InvalidPrice`,
`InvalidQuantity`) are terminal properties of the bytes: re-reading them will not
help. Keeping "wait for more" distinct from "give up" is the whole difference
between a decoder that works on a socket and one that discards valid data
whenever a read lands mid-message.

A message type's declared `payload_length` is checked **before** waiting for that
many bytes, so a corrupt header cannot stall the decoder indefinitely. And extra
bytes after a complete message are **not** `InvalidLength` — this reads at most
one message from the front of a span, because a decoder that demanded the span
hold exactly one message could not be fed by a socket read at all.

#### Three validity layers, and the boundary between them

Each layer rejects things the others accept, and keeping them apart is what lets
the system distinguish "the venue sent me a price I do not trade" from "the venue
sent me garbage":

| Layer | Question | Owner | Example rejection |
|---|---|---|---|
| **Wire** | Are these bytes a well-formed message? | `md_decoder.h` | `InvalidSide`, `InvalidLength` |
| **Sequence** | Is it in order for this stream? | `market_data_pipeline.h` | `Stale`, `GapDetected` |
| **Book** | Is it applicable to this book? | `types.h` | `OutOfRange`, `InvalidUpdate` |

A **positive** `price_tick` outside the book's configured tick range is
**wire-valid**: `price_tick = 999999999` decodes `Ok` with `consumed == 29`, and
becomes `ApplyResult::OutOfRange` one layer up — the sequence is consumed and the
book stays synced. Different fact, different consequence. The decoder does not
enforce the tick range because it has no book and no configuration, and a decoder
that did would be reaching across the seam.

#### Verification, and what it cannot do

Six suites in `md_decoder_tests.cpp`:

- **Literal expected bytes** for `SnapshotBegin`, `Level` Bid, `Level` Ask and
  `SnapshotEnd`, written out by hand from the protocol document, plus signed
  fields pinned as literal two's-complement bytes and the three message IDs
  pinned by number. This is the layer that makes the rest meaningful: an encoder
  and a decoder written by the same hand can agree on a **wrong byte order
  forever**, and every round-trip test would stay green. That is not
  hypothetical — reversing both the read and the write helpers consistently left
  *stream decoding*, *truncation*, *malformed frames* and the end-to-end
  integration all passing, and failed only the literal-byte suites. Round-trip
  tests can only catch a disagreement, never a shared mistake.
- **Endian helpers** directly, including `-1`, `0`, all-ones and a ±300
  round-trip sweep.
- **Stream decoding**: five messages in one contiguous span, exact per-message
  `consumed`, trailing bytes tolerated.
- **Truncation, exhaustively**: size 0, every header truncation 1–11, and
  **every** `Level` truncation 12–28. All `NeedMoreData`, all `consumed == 0`,
  and each asserted not to have published output. Sampling prefix lengths would
  miss the interesting boundary — a `Level` truncated to 20 bytes has already
  read a valid header declaring 17 payload bytes.
- **Malformed frames**: every rejection path, each asserting `consumed == 0`
  **and** that the output was not modified.
- **Bytes → decoder → pipeline**: one focused fixture run through both
  `MarketDataPipeline<FlatOrderBook>` and `<MapOrderBook>`, requiring agreement
  on decoder statuses, exact consumed bytes, pipeline outcomes, state,
  `last_applied_seq` and top of book.

**The 1550-trace Phase-1A differential corpus is deliberately not run through the
byte decoder.** That corpus exists to exercise the sequencer; putting an encoder
in front of it would test the encoder against the decoder rather than either
against its contract. One integration fixture establishes that the pieces
compose, and the sequencer's own verification is unchanged.

### Running it

```sh
cmake -S . -B build-exp03 -DCMAKE_BUILD_TYPE=Release
cmake --build build-exp03 -j 8
ctest --test-dir build-exp03 --output-on-failure     # 33/33 incl. both _exitcode guards
./build-exp03/market-data-pipeline/market_data_pipeline_tests
./build-exp03/market-data-pipeline/md_decoder_tests
```

Runs in well under a second, so it is not conditioned on anything and runs in the
default CTest set. Output is byte-identical across two runs. Clean under ASan and
UBSan, warning-free under `-Wall -Wextra`. Each target is paired with an
exit-code guard that drives the binary through `LLDB_SELFTEST_FAIL=1` and asserts
a non-zero exit, so a regression that stopped propagating failures fails the
suite rather than silently passing it. Both Experiment 03 targets build under
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`; that flag set
is deliberately **not** global and **not** applied to the frozen Experiment 01/02
targets, which carry published measurements.

### Status

Phase 1A and Phase 1B are **COMPLETE / FROZEN** and publish no results dataset,
because they measure nothing.

Phase 1A — sequencer / snapshot / recovery:

- `market-data-pipeline/include/md_message.h` — the typed feed vocabulary, shared
  by both phases. The decoder produces these values; nothing in the vocabulary
  knows about bytes.
- `market-data-pipeline/include/market_data_pipeline.h` — the sequencer,
  `MdResult`, `MdCounters`, `MdRecoveryEpisode`.
- `market-data-pipeline/include/md_stream_gen.h` — deterministic base scenarios,
  the 15-entry named mutation catalogue, and the seeded composition grammar.
- `market-data-pipeline/tests/md_oracle.h` — the independent reference
  implementation.
- `market-data-pipeline/tests/market_data_pipeline_tests.cpp` — six suites, all
  green.

Phase 1B — binary protocol / decoder:

- `market-data-pipeline/include/md_wire_protocol.h` — the byte layout, message
  IDs, version, offsets and the big-endian read/write helpers.
- `market-data-pipeline/include/md_decoder.h` — `DecodeStatus`, `DecodeOutcome`
  and the allocation-free `decode_one()`.
- `market-data-pipeline/include/md_encoder.h` — the test-and-fixture encoder. It
  allocates and is not the hot path; `append_raw` sets every header field
  verbatim, which is how the malformed frames are built.
- `market-data-pipeline/tests/md_decoder_tests.cpp` — six suites, all green.
- `docs/MARKET_DATA_PROTOCOL.md` — the wire format byte by byte, the decoder's
  status semantics, the stream contract and the three validity layers.

- `market-data-pipeline/CMakeLists.txt` — one header-only `INTERFACE` target
  carrying both phases, both test targets, both exit-code guards; **deliberately
  no benchmark target** in Phase 1.

**No Experiment 01 or 02 file is modified.** The only change outside the new
directory is one `add_subdirectory(market-data-pipeline)` line in the root
`CMakeLists.txt`. There is no `BENCH_ARCH_FLAGS` entry, because there is nothing
yet to time.

Phases NOT STARTED for Experiment 03: **Phase 2 — decoder thread → SPSC → book
thread.** Nothing in Phase 1 threads, measures or times anything, and it makes no
claim about how the decoder behaves once a transport and a second thread are
placed under it. A later measurement phase — ingress-to-book latency, sequencer
cost per message, snapshot-commit cost — may be added on top of the frozen
contract. No such phase is opened here.

## Next phases

Experiment 01: Phase 3M per-function call-tree symbolization (an Instruments GUI
pass over the six committed recordings); Phase 3L (Linux `perf` measurement)
when a Linux host is available; Phase 5 (engineering write-up).
Experiment 02: **all phases COMPLETE / FROZEN** — Phases 1, 2, 3A, 3B and
**Phase 4** (tail latency / jitter, a measurement phase that compares no
treatments and opens no optimization). No further Experiment 02 phase is
planned; in particular Phase 4 does **not** start a new SPSC optimization phase.
No Phase 5, no MPMC, no disruptor and no futex work is opened here.

Experiment 03: **Phase 1A (Sequencer / Snapshot / Recovery Correctness)
COMPLETE / FROZEN; Phase 1B (Binary Protocol / Decoder Correctness) COMPLETE /
FROZEN; Phase 1 overall COMPLETE / FROZEN** — a correctness-only phase that reads
no clock, ships no benchmark binary and publishes no results dataset.
**Phase 2 — Decoder Thread → SPSC → Book Thread — is NOT STARTED**, and is not
opened here. Beyond it, a measurement phase (ingress-to-book latency, sequencer
cost per message, snapshot-commit cost) may be added later on top of the frozen
contract; nothing is committed to, and Phase 1 makes no claim about any of them.

Experiments 01 and 02 are **COMPLETE / FROZEN**; Experiment 03 Phase 1 is
**COMPLETE / FROZEN**. **No next engineering task is currently opened.**
