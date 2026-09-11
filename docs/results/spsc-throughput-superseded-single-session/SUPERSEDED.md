# SUPERSEDED — first Phase-2 pass, one process per cell

**Do not use these numbers as the canonical Phase-2 result.** The canonical
Phase-2 dataset is `../spsc-throughput/`.

## What this run is

A complete, real, self-validating pass over the canonical 18-cell matrix
(2 implementations x 3 message sizes x 3 capacities) at `message_count=10000000`,
`reps=5`, `warmup=1`, run as **one process per cell**. Every cell reported
`correctness=PASS` and every summary was re-verified against its own raw
per-repetition CSV. Nothing here is fabricated and no repetition was discarded.

## Why it was superseded

Pooling only one process per cell samples exactly one OS placement of the two
threads and one allocator placement of the queue, and that placement is fixed
for the life of the process. On this host that single sample turned out to be
strongly bimodal for the SPSC cells: the very same cell measured ~13-17 ns per
message in some processes and ~28-47 ns per message in others.

That is not a repetition-to-repetition effect. It was isolated to the process:

- The `__TEXT` sections of a "fast" and a "slow" binary are byte-identical.
  All 1116 differing bytes are `LC_UUID` / code-signature metadata; zero bytes
  differ across offsets 4096-98304. The difference is therefore not codegen.
- Both modes use ~199% CPU: two threads really are running in parallel, so the
  difference is not a core-count or serialization effect.
- The slow mode burns ~3.35x more user time and shows the *consumer* as the
  bottleneck (more `producer_full_retries`, fewer `consumer_empty_retries`).
- Per-binary results are reproducible (fast binary: 13.1 / 13.7 / 16.1 / 16.8 /
  10.5 ns per message; slow binary: 44-47), and running the cells in a different
  order does not change which mode a given launch lands in.

So the spread is a memory-system / address-placement effect that a single
process can only ever sample once. The canonical runner therefore now measures
`SESSIONS` independent processes per cell (default 3) and **pools** all of their
measured repetitions, reporting the median, min, max and spread of the pooled
distribution plus the per-session medians (see `SESSIONS.md` in the canonical
directory) so that placement-to-placement spread is visible rather than hidden
behind one launch.

Crucially, this was fixed by *sampling more placements*, not by changing the
measured code. The queue stays the frozen Phase-1 unpadded SPSC implementation:
no padding, no cursor caching, no batching, no memory-order change, and no CPU
affinity or pinning. See `docs/SPSC_THROUGHPUT.md`.

## Contents

Identical to the canonical layout, minus the per-session dimension:

- `raw/<impl>_b<bytes>_c<capacity>.csv` — one row per measured repetition
- `summaries/<...>.txt` — per-cell summary as reported by the benchmark
- `stderr/<...>.txt` — benchmark stderr
- `command.txt` — the exact invocations, one process per line
- `summary.csv`, `MATRIX.md` — derived matrix
- `HOST.md` — host and toolchain metadata
