# low-latency-orderbook

A small but serious C++20 experiment: two designs for an aggregated L2 order
book, implemented and benchmarked head-to-head.

- **MapOrderBook** — `std::map`-based baseline (node tree, heap allocation per
  level).
- **FlatOrderBook** — integer-tick prices addressed directly into preallocated
  contiguous storage, with cached best bid / best ask and an allocation-free
  steady-state hot path.

Status: **Phase 1 (core data structures + correctness tests) complete.**

## Layout

```
low-latency-orderbook/
├── include/
│   ├── types.h              # Side, L2Update, Level, BookSnapshot, apply contract
│   ├── map_order_book.h     # std::map baseline
│   └── flat_order_book.h    # dense tick-addressed book
├── tests/
│   └── order_book_tests.cpp # every scenario run against BOTH books
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
- `seq` must advance by exactly `+1` for every applied update while synced;
  a skipped `seq` marks the book **unsynced** and returns `GapDetected`;
- while unsynced, updates are rejected (`Stale`) until `load_snapshot()`
  re-synchronizes the book from full state.

`best_bid()` / `best_ask()` return cached best prices in O(1) and report `0`
for an empty side.

## Correctness

`order_book_tests` runs every scenario against both books and requires
identical observable state after each step:

adding a level, updating quantity (best and non-best), deleting a non-best
level, deleting the best level, deleting the last level on a side, empty
book/sides, a new best price appearing, snapshot loading, sequence-gap
desync, recovery via a fresh snapshot, and best-price discipline across the
spread. A differential fuzzer additionally applies thousands of random
well-formed updates (including periodic snapshot resyncs) to both books and
asserts parity at every checkpoint.

## Next phases

Phase 2 (deterministic benchmark, updates generated before the timed
section), Phase 3 (Linux `perf` tooling), Phase 4 (latency percentiles),
Phase 5 (engineering write-up).
