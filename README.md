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
> Memory Model) COMPLETE / FROZEN.** A bounded single-producer / single-consumer
> ring buffer (`include/spsc_ring_buffer.h`) and a mutex reference queue
> (`include/mutex_bounded_queue.h`) are implemented, correctness is green
> (single-thread semantics, one-producer / one-consumer deterministic stress,
> rapid slot reuse at Capacity = 2, a multi-field fixed-size payload stress, and
> a differential run against the mutex queue), strict-warning clean, and
> sanitizer-clean (ASan/UBSan/TSan where the host supports them — see
> `docs/SPSC_MEMORY_MODEL.md` §10 for the exact commands). The memory-model
> argument (happens-before, ordering choices, counter wrap) is documented in
> `docs/SPSC_MEMORY_MODEL.md`. Phase 2 (throughput), Phase 3 (false sharing +
> cursor caching), and Phase 4 (tail latency) are NOT STARTED.

## Experiments

| # | Experiment | Status |
|---|------------|--------|
| 01 | L2 Order Book: `std::map` vs Flat Representation | Phase 1, 2, 4 COMPLETE / FROZEN; Phase 3M tooling COMPLETE + recordings COLLECTED (attribution analysis deferred); Phase 3L tooling READY (native Linux measurement deferred) |
| 02 | SPSC Ring Buffer / Concurrency | Phase 1 (Correctness / Memory Model) COMPLETE / FROZEN; Phase 2, 3, 4 NOT STARTED |

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
│   └── spsc_ring_buffer.h     # Exp 02 candidate: SPSC ring buffer; lock-free
│                              #   cursor protocol where atomic<size_t> is
│                              #   always lock-free (static_assert, no fallback)
├── tests/
│   ├── order_book_tests.cpp # every scenario run against BOTH books
│   ├── bitset_order_book_tests.cpp  # Optimization Study: four-way differential
│   │                                 #   (map/flat/tuned/bitset) correctness suite
│   ├── phase4_stats_tests.cpp  # Phase 4 distribution/percentile regression tests
│   └── spsc_ring_buffer_tests.cpp  # Exp 02: semantics + SPSC stress +
│                                   #   differential vs the mutex queue
├── benchmark/
│   ├── stream_gen.h            # single source of truth for the A/B/C/D/E op streams
│   ├── order_book_bench.cpp    # Phase 2 steady-state apply() throughput benchmark
│   ├── order_book_bitmap_bench.cpp  # Optimization Study: flat/tuned/bits steady
│   │                                 #   throughput + --check/--gaps/--memory/--inproc
│   ├── order_book_gap_bench.cpp     # Optimization Study: next-best-gap ladder sweep
│   ├── order_book_tail_bench.cpp  # Phase 4 fixed-batch tail-latency sampler
│   └── tail_stats.h            # Phase 4 distribution metrics / percentile definitions
├── scripts/
│   ├── bench.sh                # Phase 2 canonical per-process run
│   ├── tail-bench.sh           # Phase 4 canonical matrix runner (one invocation per cell)
│   ├── verify-tail-summary.sh  # Phase 4 raw<->summary verification
│   ├── perf-profile.sh         # Linux perf profiling harness (Phase 3L, per-cell)
│   ├── phase3m-instruments.sh  # Phase 3M xctrace/Instruments recorder (needs full Xcode)
│   └── collect-macos-profile-metadata.sh  # host/chip/toolchain metadata (Phase 3M and Phase 4)
├── cmake/
│   └── assert_nonzero_exit.cmake  # ctest guard for the test exit-code self-test
├── docs/
│   ├── results/             # committed datasets (phase2-m3max/, phase3-macos-apple-silicon/,
│   │   │                    #   phase4-macos-tail/ canonical + -pre4.1-invalid/ archived, and
│   │   │                    #   orderbook-bitmap-optimization/ study; see README.md)
│   │   └── README.md        # layout + honesty rule
│   ├── ORDERBOOK_BITMAP_OPTIMIZATION.md  # Optimization Study analysis (item 12)
│   ├── SPSC_MEMORY_MODEL.md  # Exp 02: happens-before + memory-order argument
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

**Status: Phase 1 — Correctness / Memory Model: COMPLETE / FROZEN.**
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
optimizations), and no throughput numbers are reported. The unpadded
`head_`/`tail_` layout *permits and is likely to exhibit* false sharing; Phase 3
will verify the actual cursor addresses / cache-line placement for a packed
same-line control and a separated/padded control rather than assume it.

Planned next phases (NOT STARTED):

- **Phase 2** — Throughput baseline (steady-state SPSC vs mutex transfer).
- **Phase 3** — False sharing (`head_`/`tail_` packed vs separated/padded) and
  remote-cursor caching, as controlled experiments.
- **Phase 4** — Tail latency / jitter under load.

## Next phases

Experiment 01: Phase 3M per-function call-tree symbolization (an Instruments GUI
pass over the six committed recordings); Phase 3L (Linux `perf` measurement)
when a Linux host is available; Phase 5 (engineering write-up).
Experiment 02: Phase 2 (throughput baseline), Phase 3 (false sharing + cursor
caching), Phase 4 (tail latency) — see the Experiment 02 section above.
