# Experiment 01, Phase 2 — canonical benchmark results

This directory holds the **one canonical result dataset** for the Phase 2
steady-state `apply()` benchmark. It was produced exactly as described here; no
numbers were invented or adjusted after measurement. The CSV is the source of
truth for the summary table in the README.

## Data file

- `results_2M_reps3_isolated.csv` — 40 rows (20 `map` + 20 `flat`, best-of-3
  per cell). Column order: `impl,wl,scale_n,updates,best_ms,
  best_ns_per_update,best_updates_per_s`.

## How it was measured

Each cell is one `orderbook_bench` invocation of one implementation against one
workload and one scale. The two implementations were measured in **separate
processes** so a long, CPU-saturating run of one design could not thermally
throttle the other. For every cell:

1. The full deterministic stream was generated up front, off the clock
   (fixed-seed, well-formed, sequence-continuous).
2. Each of 3 timed blocks cold-started a **fresh** book via an untimed
   `load_snapshot()` of a full N-levels-per-side snapshot, then replayed the
   identical steady stream (2,000,000 `apply()` ops) with a per-iteration
   compiler memory barrier.
3. Reported time is the **best (minimum) of the 3 blocks**.
4. The same streams were validated to produce identical externally visible
   state in both books (`orderbook_bench --check`), so no cell measures a
   degenerate or diverging stream.

## Environment (2026-09-07)

| | |
|---|---|
| Machine | Apple MacBook Pro (Apple M3 Max), model identifier `Mac15,10` |
| CPU | Apple M3 Max, `arm64` |
| OS | macOS 14.2.1 (Build 23C71) |
| Compiler | Apple clang 15.0.0 (`clang-1500.1.0.2.5`), target `arm64-apple-darwin23.2.0` |
| Build | CMake 4.4.3, `-O3 -DNDEBUG` forced on the benchmark target |
| Arch flags | **none** — compiler default (no `-march=native` / `-mcpu=…`) |
| Workloads | A update-only, B 10% deletes, C frequent best deletion, D concentrated top-of-book, E uniformly random |
| Scales (`N` live levels/side) | 1,000 / 10,000 / 100,000 / 1,000,000 |

## Notes

- Domain is `[1, 2N]` ticks; bids occupy `N+1..2N`, asks `1..N`.
- Live level count over a run is *approximately* N for every workload except A
  (exactly N); E settles near ~0.9N. Each workload replays the identical stream
  through both books, so profiles are matched between the two implementations.
- These are single-core, single-writer **mean** throughput reads of `apply()`,
  not latency percentiles (Phase 4). Snapshot load and stream generation are
  outside the timed region.
