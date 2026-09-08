# Phase 3 — profiling the Experiment 01 benchmark with Linux `perf`

Phase 3 provides the tooling to answer the "why" questions that Phase 2's
throughput numbers raised but could not answer. Phase 2 measured *how fast*
`apply()` is for each design; Phase 3 profiles *where the time goes*.

## Status & honesty box

**This phase is tooling only, written to run on a Linux host with `perf`. It was
authored on an Apple M3 Max (macOS) that has no `perf`, no full Xcode, and no
Linux VM, so the scripts and this guide have NOT been executed end-to-end and no
profile has been captured yet.** No perf numbers are presented anywhere in this
repository — none have been measured. Any illustrative perf output you may see
in other docs is a labeled placeholder, never a measured claim. When Phase 3 is
run for real on Linux, its measured `perf stat`/`perf report` output belongs in
`docs/results/` alongside the Phase 2 CSV, with the same provenance discipline.

The benchmark CLI and hot path are frozen and are NOT modified by this phase.

## Why profile? — the open questions from Phase 2

Phase 2 measured steady-state `apply()` throughput (ns/update) and documented a
few observations it deliberately did not try to explain:

1. **Workload C is cheap for the map** (~71 ns at 1M levels) versus map A (~183
   ns) and map D (~128 ns). C repeatedly deletes the current best price and
   refills it. Is the map cheap because the near-touch access pattern improves
   locality, branch predictability, or tree-path locality? Or something else?
2. **The flat book stays ~4.3–6.1 ns at every scale** (1k → 1M levels). It does
   a direct array store plus cached-best bookkeeping. Where does its per-op time
   actually go — and what does workload C's inward best re-scan cost it?
3. **The map grows with scale** (~32 → ~183 ns on update-only A as levels go 1k →
   1M). Is the growth cache misses from pointer-chasing, allocator churn on
   insert, or both?

These are hypotheses to test with counters, not conclusions.

## Prerequisites (Linux)

- A Linux host (the code is portable C++20; `orderbook_bench` builds the same
  way — see the top-level README Build & test).
- `perf` from `linux-tools-<kernel>` / `linux-perf`, on PATH.
- Counter access: `perf stat`/`perf record` need a low `perf_event_paranoid`.
  As root or with `kernel.perf_event_paranoid <= 1` (often 2 by default blocks
  some events). Check with `cat /proc/sys/kernel/perf_event_paranoid`.
- Release build, one implementation per process (the same methodology as
  `scripts/bench.sh`): profile a single `(impl, workload, scale)` cell at a time.

## How to run

Build and profile one cell (Linux only):

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench
scripts/perf-profile.sh --run map C 1000000          # map, workload C, 1M levels
```

The harness runs two passes per cell:

1. **`perf stat`** — aggregate hardware counters over the whole (long, timed)
   process: `cycles`, `instructions`, `branches`, `branch-misses`,
   `cache-references`, `cache-misses`, `L1-dcache-load-misses`,
   `LLC-load-misses`, `dTLB-load-misses`.
2. **`perf record -g`** — a call-graph profile saved to `perf.data`, for
   `perf report` to show where sampled time lands (should concentrate in
   `MapOrderBook::apply` / `FlatOrderBook::apply` and the map allocator / the
   flat scan).

Output lands in `results/perf_<impl>_<wl>_<scale>_<timestamp>/` (see the script
header). To inspect commands without executing, omit `--run` (dry-run default).

### Suggested cells to answer the open questions

| Question | Profile | Compare against |
|---|---|---|
| Why is map C cheap? | `map C 1000000` | `map A 1000000`, `map D 1000000` |
| Where does flat's time go; cost of C's re-scan? | `flat C 1000000` | `flat A 1000000` |
| Does map's scale growth = cache misses? | `map A 1000000` | `map A 1000` |

Profile each cell in its own process (per-process, not `both`), matching
`scripts/bench.sh`.

## What the counters mean for these two designs

- **`instructions`** — total retired instructions. Compare map vs flat per op to
  separate "does more work" from "same work, slower memory". The flat book's
  `apply()` is a tight array store; the map's does a red-black search per op.
- **`cycles` / `IPC`** (`instructions` ÷ `cycles`) — a low IPC hints memory
  stalls or branch mispredicts rather than raw instruction count. The map's
  pointer chasing tends to stall on cache misses; the flat's random single-cache-
  line store may still miss L1 but stream well.
- **`cache-misses` / `LLC-load-misses`** — a level is an 8-byte slot (flat) or a
  heap node (map). Map growth with scale should show rising LLC/cache misses if
  pointer-chasing is the cause; flat's ~flat time should show ~scale-invariant
  misses per op.
- **`branch-misses`** — workload C's repeated best-delete may mispredict the
  flat's inward re-scan exit, or the map's tree walk. Near-touch repetition may
  *reduce* map mispredicts — a candidate explanation for "map C is cheap".
- **`dTLB-load-misses`** — the flat's 1M-level working set is ~16 MB across both
  sides; the map's nodes scatter. TLB pressure is part of the flat-vs-map gap.

Read each counter **relative to the same cell's Phase 2 ns/update** in
`docs/results/results_2M_reps3_isolated.csv` — counters explain the time; they
do not replace it.

## Recording real results

When run on Linux, commit the measured artifacts under `docs/results/` with a
metadata block in the style of `RESULTS_METADATA.md`:

- exact `perf stat` text / `perf report` top frames,
- the cell (`impl`, workload, scale) and the Phase 2 ns/update it pairs with,
- machine + kernel + `perf` version, `perf_event_paranoid`, compiler/flags,
- date. Never invent numbers; only what a real `perf` reported.

## Scope notes

- This profiles the **steady-state timed region** of the benchmark: the stream
  is pre-generated, the book is snapshot-loaded, and the timed loop is a pure
  `apply()` replay with a per-iteration compiler barrier. perf sees the whole
  process including the untimed snapshot/stream setup, so filter the profile to
  the timed `apply()` frames when reading `perf report`.
- Single-core, single-writer, mean-throughput (same scope caveats as Phase 2).
- Phase 4 (latency percentiles) and Phase 5 (write-up) are not started.
