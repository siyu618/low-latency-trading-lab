# Experiment 02 Phase 4 — SPSC Tail Latency and Jitter

## Why

Phase 2 measured **throughput**: end-to-end `ns/message`, a mean. Phase 3A and
3B measured a **treatment effect** on that mean. A mean hides the shape of the
distribution, and a latency-sensitive consumer — a matching engine, a feed
handler — does not experience the mean. It experiences the message that took
100× longer than usual.

Phase 4 characterizes that distribution for the frozen Experiment 02 queue: one
queue, over a matrix of message sizes and ring capacities, reporting the full
per-repetition distribution rather than a single number.

Phase 4 does **not** compare treatments. It has one queue in it. It does not
introduce, tune or re-open any queue optimization, and it does not revisit
Phase 3A's or 3B's questions.

## What is measured

**`producer_ready → consumer_received`** on `std::chrono::steady_clock`.

- `producer_ready` is stamped **immediately before the producer's `try_push`
  retry loop**, so it precedes every push attempt for that message.
- `consumer_received` is stamped **immediately after a successful `try_pop`**,
  before any sequence check, checksum fold or payload validation.

The interval therefore **includes**:

- the producer's retry / backpressure wait when the ring is full,
- the queue's release/acquire synchronization,
- the payload copy,
- the consumer's empty-retry loop when the ring is empty,
- both clock reads.

### What this quantity is NOT

- **It is not "pure queue residence time."** A message is stamped *before* it is
  pushed, so part of the interval is time the message spent not yet in the queue.
- **It is not a per-call queue cost.** It contains two clock reads and a
  retry loop.
- **It is not a one-way handoff latency**, and it is not comparable to the
  `ns/message` figures in Phase 2, Phase 3A or Phase 3B. Those are end-to-end
  rates over 10M messages with different message shapes (8/32/64 B, not
  16/32/64 B). Nothing in Phase 4 may be compared against them, absolutely or
  as a ratio.

### The property that follows, and that governs how the results must be read

Because the stamp is taken before the push and includes backpressure, the
measured interval depends on **how far ahead of the consumer the producer has
run**. The producer in this harness is an unpaced spin loop, so it will run
ahead whenever it can. When it does, a message stamped for index `s` waits for
the ring to drain down to `s`, and the interval approaches the drain time of the
backlog ahead of it — which scales with capacity. When it does not, the interval
is the handoff itself.

This is not a defect in the harness; it is what the specified contract measures.
It does mean the distribution is a **producer-relative service latency including
backpressure**, and that the *median of a cell* partly reports which regime that
cell spent its measured messages in. The results section states this explicitly
rather than presenting a single "tail latency of the queue."

## The queue under test

**Separated-cursor baseline SPSC only**: monotonic unsigned cursors, separated
head and tail cache lines, acquire/release publication, and **baseline remote
cursor loads with no remote-cursor cache**.

`SpscSeparatedBaselineRingBuffer<T, Capacity>` is
`RemoteCursorRingBuffer<T, Capacity, RemoteCursorMode::Direct, false>` — the
frozen Phase 3A separated layout with the Phase-3B caching treatment switched
off.

Deliberately absent from this matrix, each for its own reason:

| absent | why |
|---|---|
| the Phase-3B `Cached` variant | Phase 4 is not a treatment comparison; using it would make every number ambiguous between "the queue" and "the cache treatment" |
| `MutexBoundedQueue` | ditto — a second implementation is a second variable |
| the Phase-3A `same_line` layout | ditto |
| any new optimization | Phase 4 is a measurement phase, not an optimization phase |

Every number in this dataset is therefore attributable to **one** queue
configuration.

The ring's cursor placement is not assumed. Every repetition begins with a
runtime gate that aborts the process if the measured object's cursor addresses
do not land in distinct cache lines under the host's *reported* line size. A
failed gate exits non-zero and publishes nothing.

## Matrix and shape

3 message sizes × 3 capacities = **9 cells**:

| | 1024 | 4096 | 65536 |
|---|---|---|---|
| **16 B** | ✓ | ✓ | ✓ |
| **32 B** | ✓ | ✓ | ✓ |
| **64 B** | ✓ | ✓ | ✓ |

Per process: **10,000,000 messages**, an excluded warm-up repetition, then
**5 measured repetitions**. One `(cell, session)` per process — no two cells
share an address space, a warmed-up cache state or a process lifetime.

Message payloads are deterministic functions of the sequence number, and each
carries a validator: a corrupted, stale or partially published payload is
detected rather than silently measured.

## Sampling

Latency is sampled by a **deterministic countdown** with interval **1021**,
after skipping a settling prefix of 100,000 messages.

The countdown — not `seq % interval` — and an **odd** interval are both load
bearing, and they are the same decision:

- A countdown's phase is fixed at the start of the transfer, so the sampled
  message indices are `settling + k·interval`, a set that does not depend on
  when the scheduler happens to run the consumer.
- An odd interval is coprime with every power-of-two capacity, so as `k`
  advances the sampled slot index `(settling + k·1021) mod Capacity` **visits
  every ring position**. With 1024, `seq % 1024` would sample exactly one slot
  forever — and that slot's distance from the producer's cursor is exactly the
  quantity most likely to differ from the average.

The number of samples per repetition is **derived, not observed**:

```
expected_samples = (messages − settling_prefix) / sample_interval
                 = (10,000,000 − 100,000) / 1021
                 = 9,696
```

A repetition whose observed sample count differs from this is a **FAILURE**,
not a smaller dataset. 180 measured repetitions × 9,696 = **1,745,280 sampled
latencies** at the canonical shape.

Percentiles are **nearest-rank on observed values only**:
`index(p) = ceil(p · N) − 1`, no interpolation. P99.99 is deliberately not
reported: at 9,696 samples it would be the maximum wearing a percentile's
label.

## Clock and calibration

One clock domain: `std::chrono::steady_clock`, never `system_clock`. Raw tick
counts are stored in the raw CSV and converted to nanoseconds outside the timed
transfer.

The conversion is **exact or the build fails**. `ns_per_tick(num, den)` returns
0 unless the clock period divides into whole nanoseconds, and the benchmark
`static_assert`s that it is non-zero. On this host `ns_per_tick == 1`. A
platform whose clock could not be converted exactly would fail to compile rather
than round every sample.

A **timer calibration** runs in every process: 200,000 back-to-back
`steady_clock::now()` pairs, taken outside the timed transfer. On this host the
pair cost has a median of 0 ns, a mean of ~14 ns and a P99 of 42 ns — i.e. the
clock's own quantum is roughly 41.7 ns and back-to-back reads frequently return
the same value.

**The calibration is descriptive only.** It is reported next to the results and
is **never subtracted** from any latency; it is not a correction factor. It
establishes the scale at which differences are meaningful — in particular that
the sub-100 ns region is only a few clock quanta wide — and nothing more.

## Session design

Four sessions, traversing the 9 cells in **forward / reverse / forward /
reverse** order.

| session | traversal | first cell | last cell |
|---|---|---|---|
| 1 | forward | 16 B / 1024 | 64 B / 65536 |
| 2 | reverse | 64 B / 65536 | 16 B / 1024 |
| 3 | forward | 16 B / 1024 | 64 B / 65536 |
| 4 | reverse | 64 B / 65536 | 16 B / 1024 |

**What it buys:** no cell is always measured first, and none is always measured
last, so a cell's temporal position within a session is not confounded with its
identity.

**What it does NOT buy**, and what must not be claimed for it:

- It is **not** an AB/BA crossover. There is one queue in this matrix, so there
  is no treatment pair to counterbalance. The two directions balance cell
  *position*, and nothing more.
- It does **not** eliminate scheduler variation, core migration, DVFS or thermal
  drift, and it does not make the four sessions independent of one another.
- Thread placement is **not controlled**: no affinity, no pinning, no thread
  priority anywhere in this experiment. macOS may migrate either thread
  mid-run, and the two threads may land on different core types.

No measurement in this dataset observes which cores were used or what the CPU
frequency was, so **no result here may be attributed to a core type or to a
frequency state.**

## Correctness gating

Every repetition must satisfy all of the following or the process exits
non-zero and the cell is not published:

| gate | condition |
|---|---|
| delivery | every message delivered, `delivered == messages` |
| sequence | strict, no gaps, no duplicates, no reordering |
| payload | every payload passes its own validator |
| timestamp | no negative latency |
| sample count | observed == derived |
| checksum | the fold of the delivered sequence |
| cursor layout | verified on the measured object |

The **checksum** deserves a note because it is a stronger check than it looks:
it folds messages `1..N` in order, so it is a pure function of the message shape
and `N`. Every repetition of every cell of one message size must therefore
produce the **identical** checksum. A dropped, duplicated or reordered message
changes it, and so does any difference in payload construction. The verifier
asserts this equality across all 20 repetitions of each message size.

Finally, a **raw → summary verifier** recomputes every repetition's count, min,
mean, P50, P90, P99, P99.9 and max from its own raw samples, on the raw tick
values, and compares them to the summary the benchmark wrote. It also checks the
sample index sequence, the tick→ns identity, the derived sample count, the
checksum equality above, and the cross-file dataset coverage. At the canonical
shape this is **5,240,246 checks over 1,745,280 raw samples**. A dataset is not
published unless all of them pass, and the verifier's output is recorded as the
dataset's `invariants.txt`.

## Two hazards that had to be fixed or ruled out

Both of these were found by running the thing, and both are recorded because a
methodology section that lists only the checks that pass is not a methodology
section.

### H1 — A timestamp slot race that produced impossible samples

**Symptom.** A repetition failed its own timestamp gate: one sample had a
*negative* latency. Everything else passed — full delivery, sequence, payload
validation, exactly the derived sample count.

**Cause.** The timestamp transport was an array of `Capacity` `int64_t`, indexed
by ring slot (`s & (Capacity − 1)`). The producer must stamp message `s` *before*
it pushes `s`, so with the consumer's head at `h` it can stamp index
`h + Capacity` — one past the last message it has managed to publish. With
`Capacity` slots, that write lands on exactly the slot holding the stamp for
index `h`, i.e. the stamp the consumer is about to read for the message it just
popped. The consumer then subtracts a *later* timestamp from its own and
computes a negative latency.

It is rare — roughly one sample in 5×10⁷ on this host — because the window
between the pop and the stamp read is a few instructions. Rare is what makes it
dangerous: it survives smoke runs and appears once in a canonical run, where it
is easy to dismiss as noise.

**Fix.** Index the stamps over `2 × Capacity` slots with a power-of-two mask.
Let the consumer have just popped index `c`, so the shared head is `c + 1`. The
producer can publish at most index `head + Capacity − 1` and can therefore stamp
at most index `head + Capacity = c + 1 + Capacity`. The next index sharing `c`'s
slot is `c + 2 × Capacity`, which is unreachable because
`c + 1 + Capacity < c + 2 × Capacity` for every `Capacity ≥ 2`. The stamp for
index `c` is therefore intact from the moment it is written until the moment it
is read. Visibility remains the ring's own release/acquire pair, exactly as for
the payload.

**Evidence.** Matched control, `16 B / 4096 B`, 31 repetitions of 10M messages:

| stamp array | outcome |
|---|---|
| `Capacity` slots (old) | 1 inversion in 31 repetitions → process exits 4 |
| `2 × Capacity` slots (fixed) | 0 inversions in 31 repetitions → exits 0 |

and a further 31 repetitions at `16 B / 1024 B` with 0 inversions.

**What did not catch it.** AddressSanitizer, UndefinedBehaviorSanitizer and
ThreadSanitizer are all clean on this harness, before *and* after the fix, and
the malformed run passed every other gate. The bug was not a data race — the
release/acquire edge orders the accesses correctly — it was a *logical* indexing
error that only the timestamp gate, and only occasionally, could see. Sanitizers
are not a substitute for an invariant that can fail.

### H2 — Host interference that no correctness check can detect

**Symptom.** A complete 36-process canonical run — every gate passing, every
checksum matching, all 5,240,246 verification checks green — whose numbers were
nonetheless meaningless.

**Cause.** The run was executed **concurrently with other work on the same
host**: sanitizer builds, sanitizer benchmark sweeps and analysis scripts. 8 of
its 180 repetitions account for **95%** of the experiment's total measured wall
time, six of them clustered within 25.48–25.66 s, with `ns_per_message` 50× to
2,000× their own cell's median.

**Why the gates could not see it.** Every gate listed above is an invariant of
the *queue*. A starved thread violates none of them; it simply runs slower. The
run's own `HOST.md` recorded a load average of 6.10 at start, and nothing
compared that against anything.

**What caught it.** `ns_per_message` — `elapsed_ns / messages` — is an
end-to-end rate that is *reproducible* for a given cell (a few percent between
clean repetitions) precisely because it is not a latency. Reading it per
repetition against its own cell's median exposes a repetition that ran slowly
for reasons the queue had nothing to do with. The verifier now fails a dataset
in which any repetition exceeds 5× its cell's median `ns_per_message`.

**Response.** The run is retained, unedited and clearly labelled, at
`docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/`, with its derived
aggregate tables **deleted** so that no quotable summary of it survives. That
directory is deliberately **not committed** — it is 106 MB of raw data for a run
that cannot be used — so a clone of this repository does not contain it. What the
repository keeps is this description and the automated gate the run motivated.
The canonical dataset was re-measured on a quiet host. A pre-flight load-average
check was added to the runner, but it is explicitly a coarse guard: that run
started at load 6.10 on 16 CPUs, below any sane threshold, and the interference
arrived during the run. The post-hoc per-repetition check is the authoritative
one.

## Results

See `docs/results/spsc-tail-latency/` for the dataset, its provenance, its
invariants and the derived tables. **Everything below is computed from
`summaries/*.csv`** — one row per measured repetition, 5 rows per session, 20 per
cell — by taking the median across the 20 rows of a cell.

`CELL_TAIL.csv`, `TAIL_MATRIX.md` and `TAIL_RATIOS.csv` are **derived** from
those same summaries by `scripts/analyze-spsc-tail.py`, but they aggregate one
level up: their unit is the **session** (the median of that session's 5
repetitions), and their per-cell figures are medians of the four session values.
They are the right files for asking how much a cell moves between sessions. They
are **not** interchangeable with the tables below, and they carry visibly
different numbers for the same cell — 32 B/4096's P50 is 12,646 ns here and
21,687 ns there. Neither is wrong; they are two summaries of a nested design, and
the difference between them is itself Finding 1.

**No distribution is pooled.** The benchmark records one distribution per
measured repetition and never merges them; the analysis aggregates the
*per-repetition statistics* (the median across the 5 measured repetitions of
that repetition-level percentile). "P99 = X" below means "the median repetition
exhibited a P99 of X", never "99% of all samples were under X". Pooling 5
repetitions would let a between-repetition regime shift masquerade as a
within-repetition tail, which is the one artifact a tail study must not create.

**Dataset.** 36 processes, 180 measured repetitions, 1,745,280 sampled latencies,
collected on the Apple M3 Max. Provenance in the dataset's `PROVENANCE.md`,
invariants in its `invariants.txt`.

Every claim below is labelled: **MEASURED** (an observed value), **DERIVED**
(computed from measured values), **INTERPRETATION** (a proposed cause, never
asserted as fact — nothing here is profiled), **LIMITATION** (what qualifies the
claim).

### The per-cell numbers

DERIVED — the median across the 20 measured repetitions of each repetition-level
statistic:

| cell | P50 | P90 | P99 | P99.9 | max | P99/P50 | ns/msg |
|---|---|---|---|---|---|---|---|
| 16 B / 1024 B | 125 | 47,250 | 53,271 | 62,416 | 97,833 | 426.2 | 51.43 |
| 16 B / 4096 B | 125 | 48,833 | 158,562 | 196,229 | 207,499 | 1268.5 | 48.76 |
| 16 B / 65536 B | 1,864,667 | 2,964,041 | 3,360,979 | 3,445,833 | 3,467,854 | 1.8 | 34.53 |
| 32 B / 1024 B | 125 | 3,062 | 49,312 | 69,750 | 126,438 | 394.5 | 55.80 |
| 32 B / 4096 B | 12,646 | 215,125 | 242,145 | 255,375 | 268,854 | 19.1 | 41.44 |
| 32 B / 65536 B | 1,631,062 | 2,731,312 | 3,486,604 | 3,590,937 | 3,615,000 | 2.1 | 37.70 |
| 64 B / 1024 B | 125 | 35,604 | 79,917 | 90,187 | 112,979 | 639.3 | 52.76 |
| 64 B / 4096 B | 141,645 | 312,250 | 335,458 | 355,208 | 388,771 | 2.4 | 42.52 |
| 64 B / 65536 B | 2,337,125 | 4,119,708 | 5,080,291 | 5,199,750 | 5,231,166 | 2.2 | 42.39 |

**A cell's P50 in that table is the median of 20 values that are not clustered
around it.** For 32 B/4096 the twenty repetition P50s span 125 → 88,625 ns
(709×); the tabulated 12,646 ns is a location summary of that set and does not
describe any repetition that was actually run. `TAIL_MATRIX.md` carries a P50
spread and a P99 spread beside every cell for the session-level version of this
question, where the same cell reads 7.29× — collapsing 20 repetitions to four
session medians first absorbs most of the movement. Finding 1 below gives the
repetition-level spreads.

### Finding 1 — these statistics are not equally reproducible

MEASURED. Spread is max/min: within one session (5 consecutive repetitions of an
**identical** configuration) and across all 20 repetitions of the cell.

| cell | P50 within / all 20 | P90 within / all 20 | P99 within / all 20 |
|---|---|---|---|
| 16 B / 1024 B | 1.49× / 1.49× | 4.96× / 4.96× | 1.55× / 1.58× |
| 16 B / 4096 B | 2.34× / 3.48× | **119.29× / 441.50×** | 1.96× / 2.43× |
| 16 B / 65536 B | 1.40× / 1.40× | 1.32× / 1.51× | 4.04× / 4.10× |
| 32 B / 1024 B | 1.49× / 1.49× | **11.38× / 102.85×** | 2.37× / 3.94× |
| 32 B / 4096 B | **378.67× / 709.00×** | 3.82× / 3.88× | **1.08× / 1.09×** |
| 32 B / 65536 B | 2.22× / 2.66× | 1.54× / 1.70× | 2.08× / 2.08× |
| 64 B / 1024 B | **236.61× / 236.61×** | **314.33× / 314.33×** | 5.00× / 5.00× |
| 64 B / 4096 B | 1.05× / 1.10× | 2.06× / 2.20× | 1.05× / 1.07× |
| 64 B / 65536 B | 1.20× / 1.23× | 1.58× / 1.71× | 1.72× / 1.82× |

This is the central result of the phase, and it is not the one the tables above
suggest on their own:

- **P50 and P90 are not reproducible properties of a cell.** Five consecutive
  repetitions of the identical binary, cell and message count produced P50s from
  333 ns to 61,500 ns in 32 B/4096 session 1 (185×), and P90s 119× apart within a
  single 16 B/4096 session and 442× apart across that cell's 20 repetitions. A
  cell-level P50 or P90 is therefore a property of *the runs that happened*, not
  of the configuration.
- **P99 is comparatively reproducible**, and in exactly the cells where P50 is
  least reproducible. In 32 B/4096, P50 spans 379× within a session while P99
  spans **1.08×**; in 64 B/1024, P50 spans 237× while P99 spans 5.00×. Across all
  nine cells, P99 spread never exceeds 5.00× and is ≤ 2.43× in six of them.
- **P99.9 and max are less reproducible than P99** (up to 10.20× and, being
  single samples, they are the statistics a scheduler interruption lands on).

LIMITATION: this does not establish *why* the median moves. The harness records
no thread placement, no core identity, no preemption count and no frequency, so
the movement is a measured property of the runs and nothing more.
INTERPRETATION, offered only as a hypothesis consistent with the contract above:
the median is the statistic most sensitive to producer lead, and producer lead is
the quantity most exposed to scheduling; P99 sits in a band whose height is set
by how much work is in flight, which is far less sensitive to it. **This is a
hypothesis. Nothing in this dataset tests it.**

### Finding 2 — the fast mode is the timer, not the queue

MEASURED. 81 of the 180 repetitions have P50 < 1 µs. Those P50s take only **ten
distinct values** in the entire dataset — 84, 125, 167, 291, 292, 333, 375, 417,
500, 625 ns — and every one is within 1 ns of an exact multiple of 41.7 ns, the
quantum the independent calibration reports (`clock_pair` P99 = 42 ns).

LIMITATION: the harness cannot resolve structure below ~42 ns, and the floor is
only three quanta wide. **No claim is made about the true handoff cost** — the
floor as measured is a property of the clock as much as of the queue. The
smallest sample in the dataset is 0 ns (11 samples of 1,745,280), for the same
reason the calibration's median clock-pair cost is 0 ns: two reads inside one
tick.

### Finding 3 — throughput is reproducible where latency is not

MEASURED. Across the same 20 repetitions per cell, `ns_per_message` — an
end-to-end rate over 10M messages, not a latency — varies by only **1.12× to
1.70×**:

| cell | ns/msg min..max | spread |
|---|---|---|
| 16 B / 1024 B | 47.08 .. 57.28 | 1.22× |
| 16 B / 4096 B | 44.91 .. 50.18 | 1.12× |
| 16 B / 65536 B | 32.23 .. 54.76 | 1.70× |
| 32 B / 1024 B | 50.70 .. 63.17 | 1.25× |
| 32 B / 4096 B | 38.10 .. 42.64 | 1.12× |
| 32 B / 65536 B | 33.78 .. 41.69 | 1.23× |
| 64 B / 1024 B | 48.64 .. 64.03 | 1.32× |
| 64 B / 4096 B | 35.89 .. 47.73 | 1.33× |
| 64 B / 65536 B | 36.73 .. 45.86 | 1.25× |

In 32 B/4096 the median moves 709× while the rate moves 1.12×. **The regime that
moves the median by two orders of magnitude barely moves the mean.** This is the
strongest argument in the dataset for reading a mean and a median as different
quantities rather than as two views of one; it is also why `ns_per_message` is
the one statistic here stable enough to serve as a whole-run sanity gate (H2).

### Finding 4 — the slow mode tracks capacity × rate

DERIVED. For the four cells whose P50 sits in the slow band, P50 divided by
`capacity × ns_per_message` is 0.66, 0.81, 0.82, 0.84 — i.e. the median
repetition of those cells held roughly three-quarters of a full ring of backlog.
INTERPRETATION: consistent with a producer that has run ahead and whose stamped
message waits for the ring to drain to it. LIMITATION: producer lead was not
instrumented, so the drain model is not confirmed by this dataset.

### Finding 5 — capacity and message size do not determine the band

MEASURED. At capacity 4096, the three message sizes land in three different
bands: 16 B at the floor (P50 = 125 ns), 32 B in between (12,646 ns), 64 B slow
(141,645 ns). At capacity 1024 all three sizes sit at the floor; at 65536 all
three are slow. The share of samples at or below 1 µs runs from 86.78% (32 B/1024)
to 0.00% (64 B/65536) and is not monotone in either variable alone.

LIMITATION: with one queue, three sizes and three capacities, this dataset cannot
separate capacity, message size and their interaction; it can only record that no
simple monotone relation holds. INTERPRETATION: the band is set by how far ahead
the producer runs, which is not a function of ring geometry alone. **Untested
here** — see Finding 1.

### The extremes

MEASURED. The largest single latency in the dataset is **33,867,000 ns (33.9 ms)**
in `b16_c65536_s2`. The largest per-cell max-of-repetition is 5,231,166 ns
(64 B/65536). Each is **one sample out of 9,696** in its repetition. LIMITATION:
a single context switch inside one sampled interval produces exactly this
signature, and the harness counts neither switches nor placements, so **no cause
is assigned to any extreme value in this dataset.**

### What the ratios in the derived tables do and do not mean

DERIVED, with a warning. `P99/P50` is 394–1268 in the four floor-band cells and
1.8–2.4 in the four slow-band cells. That ~500× difference in "tail ratio" is
produced almost entirely by the *denominator*: the P50 in those groups moves from
125 ns to 1.6 ms — four orders of magnitude — while the P99 moves from 53 µs to
3.4 ms, under two. **A tail ratio is not comparable between two cells whose
medians sit in different bands**, and none of these ratios is a property of the
queue. `TAIL_RATIOS.csv` is published for completeness, not for ranking.

### Correctness

MEASURED. All 180 repetitions delivered every message in strict sequence with
every payload passing its validator; the per-message-size checksum was identical
across all 20 repetitions of each size; and all **5,240,255** raw→summary
verification checks passed. No repetition in this dataset failed a queue
invariant.

## What cannot be claimed from this dataset

- **No cache-miss, coherence-event or cache-line-transfer attribution.** None
  was counted. A tail spike is not evidence of a cache miss.
- **No preemption, scheduler or core-migration attribution.** Which core each
  thread ran on, and whether either was preempted, was never observed.
- **No P-core / E-core attribution.** The host has both; the experiment does not
  record which was used.
- **No frequency, thermal or DVFS claim.** Neither was measured or controlled.
- **No absolute comparison with Phase 2, 3A or 3B.** Different message shapes
  and a different measured quantity.
- **No claim that any observed level is a property of the queue alone.** The
  contract includes backpressure, so the distribution depends on producer
  lead, which depends on scheduling.
- **No claim about `MutexBoundedQueue`, the same-line layout, or the cached
  remote-cursor variant.** None of them is in this matrix.

## Status

Experiment 02 Phase 4 is a **measurement** phase: it characterizes the frozen
queue and opens no new optimization. See the top-level `README.md` for the
canonical status line, and `docs/results/spsc-tail-latency/` for the dataset.
