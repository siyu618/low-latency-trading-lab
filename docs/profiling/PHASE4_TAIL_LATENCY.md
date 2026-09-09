# Phase 4 — Tail Latency and Jitter Analysis

## Why

Phase 2 measured **mean** steady-state `apply()` throughput (ns/update). A mean
hides the shape of the latency distribution. A latency-sensitive consumer cares
about the **tail**: how often does an update take 2×, 10×, 100× longer than the
typical one? Scheduling interruptions, cache misses, and allocator pauses do not
show up in an average the way they show up in P99/P99.9/MAX. Phase 4
characterizes that distribution and its jitter using the existing deterministic
workloads.

This is NOT a substitute for profiling. Phase 3 (macOS Instruments / Linux
perf) is where microarchitectural *evidence* lives; Phase 4 only *characterizes*
the distribution. An observed tail spike is never auto-explained as a cache
miss / allocator / branch / scheduling event without profiling evidence.

## What

A **fixed-batch latency sampler** and **post-measurement percentile analysis**
over one book, one workload, one scale, one seed, one update stream:

- Time fixed-size batches of `batch_size` contiguous `apply()` updates
  (default 512), store the raw integer batch duration, then derive
  `batch-normalized ns/update = batch_duration / batch_size`.
- After ALL batches are measured, compute distribution metrics over the raw
  samples (mean, min, P50, P90, P99, P99.9, max) and jitter ratios
  (P99/P50, P99.9/P50, max/P50).
- Preserve every raw sample (CSV) before any summary.

### Why batch, not per-update timing

A `FlatOrderBook` update can take a few nanoseconds. `std::chrono` reads are
themselves many nanoseconds, so timing each individual `apply()` would let the
**timer overhead dominate the operation being measured**. Timing fixed-size
batches amortizes the clock reads and keeps the measurement honest at these
latencies.

### batch-normalized latency is NOT directly measured single-update latency

`elapsed_batch_ns / batch_size` is a **batch average**. Three consequences, and
all three are why the reports say *batch-normalized* everywhere rather than
claiming single-update latency:

1. **It understates true per-update tail spread.** A single slow update inside a
   batch is diluted by its fast neighbours. A per-update latency histogram would
   show more extreme tails than the batch-normalized one does.
2. **It carries the timer boundary cost divided across the batch** (see the
   calibration experiment below).
3. It is still the right tool for comparing **distributions and jitter between
   designs/workloads** — the same methodology runs against map and flat, so the
   comparison is fair even though neither number is a raw single-update latency.

## How

### Deterministic input

Phase 4 reuses the **exact** Phase 2 workload generator
(`benchmark/stream_gen.h`, the single source of truth shared with the Phase 2
benchmark). Under the default seed a Phase 4 stream for `(workload, scale,
updates)` is byte-for-byte the Phase 2 stream; `--seed N` yields a
different-but-equally-deterministic stream of the **same** A/B/C/D/E semantics.
The stream and the cold-start snapshot are generated **before** any timer
starts; no RNG runs inside a timed batch.

### Setup is outside measurement

Construct the book, `load_snapshot()`, generate the stream, reserve all sample
storage, and warm up — all before the first timed batch. No file IO or
formatting inside a timed batch.

### Warmup

Exactly what happens, in order: a **separate book instance** is constructed,
loads the original snapshot, and replays the whole stream **once**, untimed;
that warmup book is then **destroyed**; the **measured book** is constructed and
loaded from the **original full snapshot**; and the measured stream is replayed
**once** on the clock. So no part of the measured stream is ever consumed early.

What warmup does and does not claim: replaying the stream once on a separate
book warms the process — code/caches, the allocator's arenas, and thermal state
are nudged toward what a warmed steady-state process looks like. It is
**intended to approximate a warmed steady-state process**, not to *eliminate*
scheduler or thermal noise: macOS can still move the thread across P/E cores and
background activity can still push P99/P99.9/MAX on the measured run. No
additional warmup dimensions (reps, soak, pinning) are modeled.

### Preallocation

`ceil(updates / batch_size)` samples are reserved up front. Writing an
already-measured duration into the preallocated vector happens **after** that
batch's `t1`, so it is never inside the timed region.

### Partial final batch

If `updates % batch_size` is nonzero, the trailing partial batch is applied and
timed as **its own sample** so it is recorded with its **ACTUAL operation count**
(`batch_operations = updates % batch_size` in the CSV, never the full batch
size). It is **excluded from every distribution metric** — mean, min, percentiles,
and max are all computed over FULL batches only; a short batch would carry
disproportionate timer-boundary noise into the tail, so it must not shift even
the mean. It appears in the raw CSV as the final row (`partial_rows=1` in the
header) and in the summary as explicit `partial_batch_present`,
`partial_batch_ops`, and `partial_ns_per_update` keys. Every update is always
applied, so the measured book's final state is the true post-stream state. A
partial batch is **never normalized by the full batch size** — only by its actual
op count. (The exclusion-from-the-mean rule is covered by a unit test.)

> This is why the canonical commands below run `--updates 10000000` with
> `--batch-size 512` even though `10,000,000 % 512 == 128`: the 19,531 full
> 512-update batches form the distribution, and the final 128-update partial
> batch is recorded but excluded.

### Calibration (`--calibrate`) — terminology is deliberate

`--calibrate` measures **no book work**, many times, and reports **two distinct
groups** — neither is "pure timer-boundary overhead", so the keys say exactly
what each measures:

- **`clock_pair_*`** — pure clock cost: two `steady_clock` reads separated only
  by a compiler barrier. This is the timer floor of one clock-read pair (on
  macOS the median is typically below the clock's tick resolution → ~0 ns, with
  the observed p99/max being the real per-read cost).
- **`empty_batch_harness_*`** — the exact **batch-sized empty timing skeleton**:
  clock read + `batch_size` empty iterations with the optimizer barrier and sink
  arithmetic + clock read. This is the *whole* timing skeleton with no book work
  — it is **NOT just the clock cost** (it carries the loop/barrier/sink
  overhead), so it is the honest fixed per-batch cost to compare against a full
  batch's duration.

Both groups report `median` / `p99` / `max` ns. **Neither is auto-subtracted**
from the measured book samples (subtracting an unverified constant would be a
correction we cannot justify). `empty_batch_harness` is a **small but non-zero
benchmark-harness overhead** that is part of every absolute number here — on the
M3 Max its median of ~125 ns per 512-update batch is ~6% of a typical flat_A
median batch (~2,000 ns) and ~4% of a flat_C median batch (~3,084 ns).

The harness cost has two parts with different batch-scaling behaviour, so a
larger batch does NOT amortize all of it:

- **Clock boundary overhead** — the two `steady_clock` reads. This part is
  amortized as `batch_size` grows.
- **Per-update harness work** — loop control, the optimizer barrier, and the
  sink arithmetic. This repeats once per update, so it grows with the update
  count and is not eliminated by increasing `batch_size`.

Raising `batch_size` therefore shrinks only the clock-boundary fraction of the
harness. The measured benchmark keeps the barrier/harness skeleton identical
across implementations for optimizer safety and fair comparison, which means the
absolute Flat numbers include this small harness contribution. If a smaller
relative clock-boundary fraction is wanted, pick a larger batch; the per-update
part is fixed by design. The benchmark lets you choose the batch size rather
than silently changing it.

### Distribution metrics & percentile definition

Percentiles are computed **only after all measurement completes**, over the raw
integer batch durations (source of truth, never prematurely rounded), using the
**nearest-rank** definition: for a fraction q, rank = `ceil(q * N)` and the
percentile is the observed sample at that 1-based rank. P50 is the
`ceil(N/2)`-th smallest sample; P100 is the max. No interpolation — a reported
percentile is a duration a batch actually took.

### Raw result output

With `--samples-out FILE` the benchmark writes, after measurement:

```
sample_index,batch_operations,elapsed_ns,normalized_ns_per_update
```

`elapsed_ns` is the raw integer batch duration (the source of truth); the
normalized column is derived. The CSV is written only after measurement
completes.

## Canonical Phase 4 cells

Suggested starting matrix (defaults: `--updates 10000000 --batch-size 512
--seed <Phase 2 seed>`, one process per book — see Process isolation). The
commands below run **as written**: each produces 19,531 full 512-update batches
for the distribution plus one recorded-and-excluded 128-update partial batch
(see [Partial final batch](#partial-final-batch)).

| Cell | What it probes |
|---|---|
| `map  A 1000` | map tail at small working set |
| `map  A 1000000` | map tail growth with tree size |
| `map  C 1000000` | map tail under best-price churn |
| `map  E 1000000` | map tail under full-book random churn (worst case) |
| `flat A 1000000` | flat tail (array store) at full scale |
| `flat C 1000000` | flat tail under best-deletion re-scan |

Reproduction commands (each a SEPARATE process, run to completion before the
next):

```sh
./build-perf/orderbook_tail_bench --book map  --workload A --levels 1000 \
    --updates 10000000 --batch-size 512 --samples-out results/map_A_1000_raw.csv
./build-perf/orderbook_tail_bench --book map  --workload A --levels 1000000 --updates 10000000 --batch-size 512 --samples-out results/map_A_1M_raw.csv
./build-perf/orderbook_tail_bench --book map  --workload C --levels 1000000 --updates 10000000 --batch-size 512 --samples-out results/map_C_1M_raw.csv
./build-perf/orderbook_tail_bench --book map  --workload E --levels 1000000 --updates 10000000 --batch-size 512 --samples-out results/map_E_1M_raw.csv
./build-perf/orderbook_tail_bench --book flat --workload A --levels 1000000 --updates 10000000 --batch-size 512 --samples-out results/flat_A_1M_raw.csv
./build-perf/orderbook_tail_bench --book flat --workload C --levels 1000000 --updates 10000000 --batch-size 512 --samples-out results/flat_C_1M_raw.csv
```

`results/` is git-ignored. `scripts/tail-bench.sh` automates the full matrix
above (fresh Release build, summary + raw CSV + `command.txt`/`host.txt`
provenance per cell into a dated `results/phase4_<ts>/`), or one cell:
`scripts/tail-bench.sh map C 1000000`. Each cell invokes the benchmark **exactly
once** with both `--stats-out` and `--samples-out`, then **verifies** the raw
CSV recomputes the summary. The canonical six-cell dataset measured this way on
2026-09-09 is committed under `docs/results/phase4-macos-tail/` (FROZEN); the
pre-fix cells from the Phase 4.1 hardening pass are archived under
`docs/results/phase4-macos-tail-pre4.1-invalid/` and are **not** canonical (see
[Status](#status)).

## Report categories (every claim is labeled)

- **MEASURED** — the observed latency distribution (raw samples, or a percentile
  that is a value a batch took).
- **DERIVED** — percentiles and jitter ratios computed from the measured samples.
- **INTERPRETATION** — a proposed cause. Never asserted as fact without
  profiling evidence (Phase 3's job).
- **LIMITATION** — scheduling noise, sample count, timer boundary overhead,
  batch-normalization dilution, anything that qualifies the claim.

## macOS experiment limitations (current canonical environment)

Record in each dataset's metadata: Mac model, chip, macOS version, Apple clang
version, compiler flags, date, update count, batch size, workload, seed.
Documented limitations:

- macOS schedules across Apple Silicon P/E cores freely; **strict CPU pinning is
  not assumed**.
- Background system activity can influence P99/P99.9/MAX.
- **MAX is especially sensitive to OS scheduling interruptions** — a single
  context switch inside one batch shows up as one large sample.
- A tail spike is therefore **not automatically attributed to order-book code**;
  Phase 3 profiling is where such an attribution would be tested.
- **Host timer quantization.** The calibration `clock_pair` p99 (~42 ns on this
  host) shows ~42 ns timing granularity for very short intervals. Fixed-batch
  timing reduces per-update timer contamination substantially, but the fastest
  Flat distributions still show this quantization — a ~42 ns quantum is ~2% of a
  ~2 µs (512 × ~3.9 ns/update) flat-A median batch — so tiny differences of a
  few hundredths of ns/update between fast Flat cells should not be
  over-interpreted.

## Status

- Phase 1 — COMPLETE / FROZEN
- Phase 2 — COMPLETE / FROZEN
- Phase 3L — tooling READY; native Linux PMU data DEFERRED (no Linux host)
- Phase 3M — tooling COMPLETE; six real recordings COLLECTED; call-tree /
  attribution analysis DEFERRED (needs an Instruments GUI pass over the
  recordings — see `docs/results/phase3-macos-apple-silicon/`)
- **Phase 4 — COMPLETE / FROZEN.** Tooling (as of the Phase 4.1 hardening pass:
  trailing-partial-batch exclusion fixed and covered by a regression test, the
  canonical runner issues ONE invocation per cell and verifies summary-from-raw,
  calibration terminology split into `clock_pair_*` / `empty_batch_harness_*`,
  and post-measurement validation refuses an invalid run) **and** measurement:
  the canonical six-cell dataset was measured 2026-09-09 on the Apple M3 Max and
  committed under `docs/results/phase4-macos-tail/` with provenance
  (`command.txt` / `host.txt` per cell, `RESULTS_METADATA.md`, `README.md`,
  `PHASE4_ANALYSIS.md`, `verify-all.log`). Every cell re-verifies summary-from-raw
  (13 metrics, 0 failures). The six cells previously committed under
  `docs/results/phase4-macos-tail/` from the **pre-4.1 tooling** are INVALID /
  NOT canonical and are archived under
  `docs/results/phase4-macos-tail-pre4.1-invalid/` (retained only as a labeled
  historical artifact).
