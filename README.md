# low-latency-trading-lab

A lab for experiments in low-latency C++ trading infrastructure. Each
experiment lives in its own directory under `experiments/`.

> **Status — Experiment 01 Phase 2 (benchmark) complete:**
> **Experiment 01 — L2 Order Book: `std::map` vs Flat Representation** is
> implemented, its correctness tests are green, and the deterministic
> benchmark has measured steady-state `apply()` throughput across both
> implementations, five workloads, and four book sizes. Results are below.

## Experiments

| # | Experiment | Status |
|---|------------|--------|
| 01 | L2 Order Book: `std::map` vs Flat Representation | correctness done; benchmark done |

## Layout

```
low-latency-orderbook/
├── include/
│   ├── types.h              # Side, L2Update, BookSnapshot, apply contract
│   ├── map_order_book.h     # std::map baseline
│   └── flat_order_book.h    # dense tick-addressed book
├── tests/
│   └── order_book_tests.cpp # every scenario run against BOTH books
├── benchmark/
│   └── order_book_bench.cpp # deterministic steady-state apply() benchmark
├── scripts/
│   └── bench.sh             # build + run the full benchmark matrix
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
over a very large price domain, and a differential fuzzer that applies
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
  rejected/`Stale` update. `--check` replays every stream against a reference
  book and prints the resulting shape so no degenerate stream is measured.
- **Each cell reports the best of `reps` blocks.** Every block cold-starts a
  fresh book from the same snapshot and replays the identical op stream, so the
  only difference between two books' timings is the book implementation. Best-of
  discounts scheduling noise (which only ever adds latency).
- **Two book designs never share one process.** Run one implementation per
  process (`orderbook_bench map …` / `orderbook_bench flat …`) — required for
  profile runs (Phase 3) so perf counters are never attributed to a mixed run.

Price-domain model: the configured domain is `[1, 2N]` where `N` is the scale in
price levels. Each side starts with `N` live levels (bids at `N+1..2N`, asks at
`1..N`), and every workload keeps its side at ~`N` levels, so each cell measures
steady state, never a draining book. Workloads:

| Workload | What it does |
|----------|--------------|
| A update-only | every op re-quantifies a random present level |
| B 10% deletes | every 10th op deletes a present level; the rest restore a random absent one |
| C frequent best deletion | ~45% delete the current best level (forces the flat book's inward re-scan); the rest refill the vacated level, so the best churns at the touch while the side stays full |
| D concentrated top-of-book | ops touch only a small window at the best end; ~15% deletes inside it |
| E uniformly random | fair side coin, price uniform over the whole region, half delete / half restore |

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
regardless of build type, so a debug build cannot accidentally time
unoptimized code). `-march=native` is opt-in via
`cmake -DENABLE_NATIVE_ARCH=ON` and only applied when the compiler supports it
(Apple clang uses `-mcpu=apple-m1`).

### Results

Measured on this machine (Apple M3 Max / arm64, macOS, Apple clang 15,
Release `-O3 -DNDEBUG`, no `-march=native`). Reported time is the **best of 3**
timed blocks per cell (each block cold-starts a fresh book from the same
snapshot and replays the identical 2,000,000-update stream). Units: ns per
`apply()`. **Map and flat were measured in separate processes** (each book
design alone in its own process) so a 6-minute CPU-saturating run of one design
cannot thermally throttle the other; this is why the numbers below are
internally consistent (flat is ~flat across scales, map grows with scale).

| levels | impl | A upd-only | B 10% del | C best-del | D top-of-bk | E uniform |
|--------|------|-----------:|----------:|-----------:|------------:|----------:|
| 1,000  | map  |    32.5    |    36.1   |    42.8    |    33.1     |   61.6    |
| 1,000  | flat |    4.5     |    4.4    |    5.9     |    4.7      |    5.1    |
| 10,000 | map  |    60.8    |    72.3   |    48.1    |    49.2     |  134.9    |
| 10,000 | flat |    4.4     |    4.5    |    6.1     |    4.5      |    5.0    |
| 100,000| map  |    85.1    |   159.7   |    61.0    |   104.2     |  502.3    |
| 100,000| flat |    4.4     |    4.5    |    6.1     |    4.5      |    5.0    |
| 1,000,000 | map |  182.7    |   181.5   |    70.6    |   128.1     |  383.5    |
| 1,000,000 | flat |   4.3    |    4.5    |    5.9     |    4.5      |    5.1    |

(Rows are ns/update; full CSV in `results/results_2M_reps3_isolated.csv`.)

**What these measure.** All numbers are for a book that stays at ~`N` live
levels per side for the whole timed block; snapshot loading, stream generation
and the clock read are excluded. The timed loop is a pure `apply()` replay of
pre-recorded updates with a per-iteration compiler barrier (so the optimizer
cannot prove the flat book's stores dead or reorder across `apply()` calls).

**Reading the results.**

- **Map cost scales with book size.** `std::map` is a red-black tree: each op
  walks tree nodes allocated across the heap, so as the book grows the pointer
  chain spans more cache lines and misses mount. update-only (A) goes ~32 ns →
  ~183 ns as levels go 1k → 1M. The uniformly-random workload (E) is the worst
  case — deletes/adds land anywhere in the tree, maximizing pointer chasing
  (~62 ns → ~384 ns).
- **Flat cost is essentially constant.** The dense book is a direct array
  store; the 1M-level working set is ~16 MB, larger than L2 but the update
  touches one random cache line regardless, so cost stays ~4.3–6.1 ns at every
  scale. This is the flat representation's whole point: `apply()` latency is
  independent of how many levels are in the book.
- **Best-price deletion (C) costs both designs extra but is cheap for flat.**
  Deleting the best forces the flat book to rescan inward from the adjacent
  slot (~6 ns vs ~4.4 ns update-only); the map just erases the root node, so C
  is actually map's *cheapest* workload at scale (~71 ns at 1M). The structural
  difference shows in D (top-of-book churn): map ~128 ns, flat ~4.5 ns at 1M.
- **Speedups are large and grow with scale.** At 1,000 levels flat is ~7–12×
  faster; at 1,000,000 levels it is **~40–90×** faster on update-only and
  uniformly-random workloads. Where the map's structure is heavily exercised
  (random full-book churn, E), flat is ~75× faster; on the flat book's own
  stress test (best deletion, C) the gap narrows to ~12×.

**Methodology notes / limitations.** Workloads A/B/D hold their side at exactly
`N` levels; E drifts slightly (density ~0.9N in steady state) but identically on
both books, so the comparison stays fair. Best-of-3 blocks was chosen to
discount scheduler noise; running on a shared/heterogeneous machine would add
variance. These are single-core, single-writer throughput numbers — the books
are intentionally lock-free because they are single-writer by design.

## Next phases

Phase 3 (Linux `perf` tooling), Phase 4 (latency percentiles),
Phase 5 (engineering write-up).
