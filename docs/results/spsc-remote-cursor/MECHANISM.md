# Experiment 02 Phase 3B — MEASURED MECHANISM

> **This file is not the canonical result.** The numbers here come from
> `--instrument=1` processes, which are a DIFFERENT instantiation from the
> one the canonical throughput figures come from. The `ns_per_message`
> values are deliberately not reproduced here and **must not be quoted as the
> Phase-3B throughput result**.

## What is measured

How many times each thread ACTUALLY read the opposite thread's cursor. In
the baseline this equals the `try_push`/`try_pop` call count by construction.
In the cached variant it counts real refresh loads — which is
the mechanism the experiment is about.

The counters are **ordinary non-atomic members owned by one thread each**,
incremented on the hot path and read only AFTER both threads have been
joined. No global atomic was added to the timed hot path to count these.

## Two normalizations, and why the attempt-normalized one is primary

The counters can be divided two ways, and they answer different questions.

| metric | definition | what it answers |
|---|---|---|
| **loads per ATTEMPT** (primary) | `remote_loads / (message_count + retries_on_that_side)` | Did the cache actually elide remote loads from a try operation? |
| loads per MESSAGE (secondary) | `remote_loads / message_count` | How many remote loads did the run issue per message *delivered*? |

The second is an end-to-end workload metric, and it mixes two effects:

```
loads/message  =  loads/attempt  ×  attempts/message
```

`attempts/message` is retry behaviour — how many times the thread had to
retry before the queue let it through. **A change in loads/message can
therefore be produced entirely by a change in retry volume, with
loads/attempt unchanged.** The attempt-normalized figure is the one that
speaks to the caching mechanism; loads/message is retained below precisely
because it shows the confound rather than hiding it.

Definitions used throughout:

```
producer_attempts = message_count + producer_full_retries
consumer_attempts = message_count + consumer_empty_retries
```

These are DERIVED from the existing `mechanism/raw/*.csv`; nothing was
re-measured. Every value below is reproduced in `mechanism/ATTEMPTS.csv`,
including the per-repetition min/max of each cached rate. No raw CSV was
edited.

## Check: the baseline must be exactly 1.000000000 loads per attempt

The baseline performs one remote load per try operation by construction, so
this is a check on the derivation, not a result. It holds **exactly, in
integer arithmetic, on all 27 baseline repetitions** (3 reps × 9 cells):

```
producer_remote_tail_loads == message_count + producer_full_retries
consumer_remote_head_loads == message_count + consumer_empty_retries
```

Zero violations. Nothing below is a ratio computed against an assumed
denominator.

## PRIMARY — remote loads per try attempt

| bytes | capacity | producer L/attempt (cached) | consumer L/attempt (cached) | producer refresh rate | consumer refresh rate | producer reduction | consumer reduction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 0.083036475 | 0.951795198 | 8.30365% | 95.17952% | 12.0x | 1.1x |
| 8 | 4096 | 0.017692214 | 0.971932123 | 1.76922% | 97.19321% | 56.5x | 1.0x |
| 8 | 65536 | 0.000015200 | 0.994196748 | 0.00152% | 99.41967% | 65,789.5x | 1.0x |
| 32 | 1024 | 0.269508600 | 0.960854406 | 26.95086% | 96.08544% | 3.7x | 1.0x |
| 32 | 4096 | 0.007858894 | 0.992188752 | 0.78589% | 99.21888% | 127.2x | 1.0x |
| 32 | 65536 | 0.000015200 | 0.996582097 | 0.00152% | 99.65821% | 65,789.5x | 1.0x |
| 64 | 1024 | 0.982862322 | 0.001078262 | 98.28623% | 0.10783% | 1.0x | 927.4x |
| 64 | 4096 | 0.990389493 | 0.000277291 | 99.03895% | 0.02773% | 1.0x | 3,606.3x |
| 64 | 65536 | 0.985637423 | 0.000022267 | 98.56374% | 0.00223% | 1.0x | 44,909.5x |

Baseline is **1.000000000** on both sides in every cell, verified above. The
"reduction" columns are therefore just the reciprocal of the cached rate.

The single most important line in this table:

> **No cached cell exceeds 1.000000000 loads per attempt on either side. The
> maximum anywhere in the matrix is 0.996582097.**
>
> Caching never increases the probability that a try operation performs a
> remote cursor load. That is what the treatment was designed to do, and it
> is directly measured — not inferred from a throughput difference.

## What the attempt-normalized data says about the mechanism

Read the two refresh-rate columns together, and the pattern is not "the
cache helps one side and hurts the other". It is:

> **The side that can make sustained progress reuses its cached remote cursor
> across many attempts. The side blocked on genuinely-full or
> genuinely-empty state reaches the may-fail path on nearly every failed
> attempt, and therefore refreshes the remote cursor on nearly every one of
> them.**

Why the blocked side must refresh: a cached value can only tell you that the
queue *may* be full/empty. If the real remote cursor has not moved, no amount
of re-reading the cache can change that answer — the only way to learn
anything new is to go and look at the real cursor. So a thread whose every
attempt fails refreshes on essentially every attempt, which is exactly the
baseline's behaviour plus one comparison.

The data shows this cleanly, and **the side it lands on flips with message
size**, matching the retry balance in the canonical leg (§4.7 of
`docs/SPSC_REMOTE_CURSOR_CACHE.md`):

* **8 B and 32 B** — the consumer is the blocked side (it posts 5–20 empty
  retries per message against ≈0 full retries, so the **producer** is the
  pace-limiting side). The consumer's refresh rate stays at **95.2%–99.7%**
  of its attempts, essentially unchanged from the baseline's 100%, while the
  producer's collapses to **8.3% → 1.77% → 0.00152%** at 8 B and
  **26.95% → 0.786% → 0.00152%** at 32 B as capacity grows.
* **64 B** — the roles swap: the producer is the blocked side (3–7 full
  retries per message against ≈0 empty retries, so the **consumer** is the
  pace-limiting side). The producer's refresh rate stays at **98.3%–99.0%**,
  essentially the baseline's 100%, while the consumer's collapses to
  **0.108% → 0.0277% → 0.00223%**.

The magnitude on the progressing side is striking and tracks how much slack
the queue has: at 8 B / 65536 the producer refreshes on about **1 attempt in
66,000**, and at 64 B / 65536 the consumer refreshes on about **1 attempt in
45,000**. The per-repetition spread in `mechanism/ATTEMPTS.csv` shows these
rates are not knife-edge artefacts of one run — the 8 B / 1024 producer rate,
the loosest of the low ones, ranges over 0.0123–0.1162 across its three
repetitions, an order of magnitude of spread on a number that is still two
orders of magnitude below the baseline's 1.0.

**8 B, 32 B and 64 B are three different stories and are not collapsed into
one here.** In particular, on the *secondary* metric the 32 B cells do not
all move the same way: the consumer's loads/message **decrease** at 32 B /
1024 (10.71 → 7.76) and 32 B / 4096 (12.14 → 10.24) and **rise slightly** at
32 B / 65536 (14.82 → 15.99). A single sentence covering 8 B and 32 B
together would be wrong.

## SECONDARY — loads per delivered message, and its confound

Kept because the increase cases are real and worth showing. The last two
columns expose why they happen.

| bytes | capacity | side | baseline L/msg | cached L/msg | baseline L/attempt | cached L/attempt | baseline attempts/msg | cached attempts/msg |
|---|---|---|---|---|---|---|---|---|
| 8 | 1024 | producer | 1.5451 | 0.0888 | 1.000000 | 0.083036 | 1.5451 | 1.0688 |
| 8 | 1024 | consumer | 3.2340 | 6.1503 | 1.000000 | 0.951795 | 3.2340 | 6.4617 |
| 8 | 4096 | producer | 1.0284 | 0.0179 | 1.000000 | 0.017692 | 1.0284 | 1.0144 |
| 8 | 4096 | consumer | 3.0115 | 7.4259 | 1.000000 | 0.971932 | 3.0115 | 7.6403 |
| 8 | 65536 | producer | 1.0655 | 0.0000 | 1.000000 | 0.000015 | 1.0655 | 1.0000 |
| 8 | 65536 | consumer | 2.7666 | 11.4139 | 1.000000 | 0.994197 | 2.7666 | 11.4805 |
| 32 | 1024 | producer | 1.0080 | 0.3409 | 1.000000 | 0.269509 | 1.0080 | 1.2649 |
| 32 | 1024 | consumer | 10.7136 | 7.7647 | 1.000000 | 0.960854 | 10.7136 | 8.0810 |
| 32 | 4096 | producer | 1.0007 | 0.0079 | 1.000000 | 0.007859 | 1.0007 | 1.0061 |
| 32 | 4096 | consumer | 12.1395 | 10.2435 | 1.000000 | 0.992189 | 12.1395 | 10.3242 |
| 32 | 65536 | producer | 1.0002 | 0.0000 | 1.000000 | 0.000015 | 1.0002 | 1.0000 |
| 32 | 65536 | consumer | 14.8210 | 15.9883 | 1.000000 | 0.996582 | 14.8210 | 16.0431 |
| 64 | 1024 | producer | 2.3188 | 6.2414 | 1.000000 | 0.982862 | 2.3188 | 6.3502 |
| 64 | 1024 | consumer | 1.2465 | 0.0011 | 1.000000 | 0.001078 | 1.2465 | 1.0001 |
| 64 | 4096 | producer | 1.9971 | 6.1547 | 1.000000 | 0.990389 | 1.9971 | 6.2144 |
| 64 | 4096 | consumer | 1.1903 | 0.0003 | 1.000000 | 0.000277 | 1.1903 | 1.0000 |
| 64 | 65536 | producer | 2.0273 | 6.0278 | 1.000000 | 0.985637 | 2.0273 | 6.1157 |
| 64 | 65536 | consumer | 1.0000 | 0.0000 | 1.000000 | 0.000022 | 1.0000 | 1.0000 |

### Cells where the cached variant performs MORE remote loads per message

Seven cells, all on one side. These are real and are not removed.

| bytes | capacity | side | baseline L/msg | cached L/msg | baseline L/attempt | cached L/attempt | attempts/msg change |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | consumer | 3.2340 | 6.1503 | 1.000000 | 0.951795 | 3.23 → 6.46 |
| 8 | 4096 | consumer | 3.0115 | 7.4259 | 1.000000 | 0.971932 | 3.01 → 7.64 |
| 8 | 65536 | consumer | 2.7666 | 11.4139 | 1.000000 | 0.994197 | 2.77 → 11.48 |
| 32 | 65536 | consumer | 14.8210 | 15.9883 | 1.000000 | 0.996582 | 14.82 → 16.04 |
| 64 | 1024 | producer | 2.3188 | 6.2414 | 1.000000 | 0.982862 | 2.32 → 6.35 |
| 64 | 4096 | producer | 1.9971 | 6.1547 | 1.000000 | 0.990389 | 2.00 → 6.21 |
| 64 | 65536 | producer | 2.0273 | 6.0278 | 1.000000 | 0.985637 | 2.03 → 6.12 |

**None of these is evidence that the cached algorithm performs more remote
loads for a given try operation.** In every one of the seven, the
loads/attempt column goes *down* (1.000000 → 0.95–0.996), i.e. the cache is
doing its job per attempt. The loads/message column goes up because
`attempts/message` rose by more than the per-attempt rate fell — the thread
retried far more often. Decomposed with `loads/message = loads/attempt ×
attempts/message`:

* 8 B / 65536 consumer: per-attempt rate **fell 0.6%** (1.000000 →
  0.994197) while attempts/message **rose 4.1×** (2.77 → 11.48). The 4.1×
  dominates, so loads/message rises 4.1× (2.7666 → 11.4139).
* 64 B / 65536 producer: per-attempt rate **fell 1.4%** (1.000000 →
  0.985637) while attempts/message **rose 3.0×** (2.03 → 6.12), so
  loads/message rises 3.0× (2.0273 → 6.0278).

A reader who saw only the loads/message column would conclude the opposite of
what the mechanism data shows. That is why the attempt-normalized figure is
the primary one here, and why this decomposition is printed rather than
described.

## Where the mechanism does not pay

A cache can only elide a load when the cached value is *sufficient* — when it
already proves progress is possible. The blocked side's cached value proves
the opposite, and the real cursor is not moving, so it must refresh on
essentially every failed attempt: the baseline's load count plus a
comparison. This is a real property of the design and is reported as such, not
smoothed over.

**A reduction in remote loads did not imply a reduction in wall clock.** The
canonical result (`PAIRED_COMPARISON.md`) is that the cached variant was
slower in 6 of 9 cells, stably 4/4, by 1.09×–2.00× — while this file shows
the per-attempt load reduction working exactly as designed on the progressing
side. The two facts are compatible and both are measured; no causal link
between them is claimed here, because no hardware counters were collected
(see below).

## What this can and cannot show

- It CAN show that the cached variant performs fewer remote cursor loads per
  try attempt, and by how much, per cell. That is directly counted.
- It CAN show that on the *end-to-end* metric some cells do more total remote
  loads, and decompose that into its two factors.
- It CANNOT show why any throughput difference exists: there are no hardware
  performance counters here. This file does not measure cache misses,
  coherence transactions, or line invalidations, and nothing here may be
  described in those terms.
- **The mechanism counts and the canonical throughput come from different
  processes.** The instrumented leg is a separate instantiation whose regime
  need not match the canonical leg's (see §7.5 of
  `docs/SPSC_REMOTE_CURSOR_CACHE.md`). And within the instrumented leg the
  counts are end-to-end totals for a whole run, so part of any *increase* on
  the losing side is a consequence of that run's retry behaviour rather than
  a cause of it. For both reasons, **no "loads saved per nanosecond"
  arithmetic is performed anywhere in this dataset.**
