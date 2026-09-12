# low-latency-trading-lab

A lab for experiments in low-latency C++ trading infrastructure. This
repository currently hosts two experiments at the repo root: **Experiment 01**
— L2 order book (`std::map` vs flat representation) — and **Experiment 02** —
SPSC ring buffer / concurrency. Experiment 02's files sit alongside
Experiment 01's in the shared `include/`, `tests/`, and `docs/` trees, clearly
separated by file name and by namespace (`llob` = order book, `lltl` = queue);
if more experiments land later they will be organized into their own top-level
directories.

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
> in any cell, on either side**; end-to-end **loads per message** fell by up to
> **~44,910×** on one side and *rose* on the other, because the side that was
> blocked simply attempted more times, not because it refreshed more often. Yet
> the cached variant was **slower in 6 of 9 cells, stably across all four balanced
> sessions, by 1.09×–2.00×**, with 3 cells inconclusive — and how far the loads
> fell did **not** predict how much throughput suffered (8 B/4096 has a far smaller
> reduction than 8 B/65536 but the largest penalty, ~2.005×). The release/acquire
> publication edge still exists in both variants; caching reduced its *frequency*,
> it did not replace it. Phase 4 (tail latency) is NOT STARTED, and **no Phase-2
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

## Experiments

| # | Experiment | Status |
|---|------------|--------|
| 01 | L2 Order Book: `std::map` vs Flat Representation | Phase 1, 2, 4 COMPLETE / FROZEN; Phase 3M tooling COMPLETE + recordings COLLECTED (attribution analysis deferred); Phase 3L tooling READY (native Linux measurement deferred) |
| 02 | SPSC Ring Buffer / Concurrency | Phase 1 (Correctness / Memory Model) COMPLETE / FROZEN; Phase 2 (Throughput Baseline) COMPLETE / FROZEN; Phase 3A (Controlled Cursor Placement) COMPLETE / FROZEN; Phase 3B (Remote Cursor Caching) COMPLETE / FROZEN; Phase 4 NOT STARTED |

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
│   └── spsc_remote_cursor_tests.cpp  # Exp 02 Phase 3B: the same protocol against
│                                   #   BOTH remote-read treatments + cached-state
│                                   #   placement evidence + counter-wrap tests
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
│   └── tail_stats.h            # Phase 4 distribution metrics / percentile definitions
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
│   └── collect-macos-profile-metadata.sh  # host/chip/toolchain metadata (Phase 3M and Phase 4)
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
│   │   │                    #   spsc-remote-cursor/ canonical Exp 02
│   │   │                    #   Phase 3B; see README.md)
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
│   └── profiling/           # Phase 3 guides (README.md, MACOS_INSTRUMENTS.md) + Phase 4
│                            #   tail-latency methodology (PHASE4_TAIL_LATENCY.md)
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
Phase 4 — Tail Latency: NOT STARTED.**
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

Phases NOT STARTED:

- **Phase 4** — Tail latency / jitter under load.

## Next phases

Experiment 01: Phase 3M per-function call-tree symbolization (an Instruments GUI
pass over the six committed recordings); Phase 3L (Linux `perf` measurement)
when a Linux host is available; Phase 5 (engineering write-up).
Experiment 02: Phase 4 (tail latency) — see the Experiment 02 section above.
Phases 1, 2, 3A and 3B are COMPLETE / FROZEN.
