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

A **separate book instance** replays the whole stream once, untimed, before the
measured run. The measured run then starts from the original full snapshot — no
part of the measured stream is consumed early, and warm caches/`madvise`/clock
are representative of the measured region.

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

### Timer-boundary calibration

`--calibrate` measures the same timing skeleton (clock read, `batch_size` empty
iterations with the optimizer barrier, clock read) with **no book work**, many
times. It reports the **median and P99 boundary overhead** per batch and the
observed max. The overhead is **not auto-subtracted** from latency samples
(subtracting an unverified constant would be a correction we cannot justify);
instead it is reported so you can judge whether the batch is large enough. If
the boundary overhead is material relative to a typical flat batch duration at
`batch_size=512`, use a larger batch (e.g. 1024). The benchmark lets you choose
the batch size rather than silently changing it.

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

`results/` is git-ignored; real, inspected datasets are committed under
`docs/results/phase4-macos-tail/`.

`scripts/tail-bench.sh` automates the full matrix above (fresh Release build,
summary + raw CSV + `command.txt`/`host.txt` provenance per cell into a dated
`results/phase4_<ts>/`), or one cell:
`scripts/tail-bench.sh map C 1000000`.

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

## Status

- Phase 1 — FROZEN
- Phase 2 — FROZEN
- Phase 3L — READY / DEFERRED
- Phase 3M — READY / DEFERRED
- **Phase 4 tooling — COMPLETE**
- **Phase 4 real canonical measurement — PENDING** until real tail-latency
  datasets are collected and reviewed.
