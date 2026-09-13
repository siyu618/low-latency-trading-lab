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

On the canonical dataset that regime turned out to be **stable per cell** rather
than shifting within one: six of the nine cells sit at the handoff floor in every
one of their repetitions and three sit in the drain regime in every one. That is
a result, not an assumption — Q4 and Q7 in the results section establish it from
the retry counters and the measured latencies.

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

Latency is sampled by a **deterministic sequence-keyed schedule** with interval
**1021**, after skipping a settling prefix of 100,000 messages. The schedule
holds one number, `next_sample_seq`, initialised to `settling + interval − 1`
and advanced by `interval` each time it fires; a message is sampled when its
sequence number **equals** that value.

The schedule — not `seq % interval` — and an **odd** interval are both load
bearing, and they are the same decision:

- The schedule's phase is fixed at the start of the transfer, so the sampled
  message indices are `settling + k·interval`, a set that does not depend on
  when the scheduler happens to run the consumer.
- An odd interval is coprime with every power-of-two capacity, so as `k`
  advances the sampled slot index `(settling + k·1021) mod Capacity` **visits
  every ring position**. With 1024, `seq % 1024` would sample exactly one slot
  forever — and that slot's distance from the producer's cursor is exactly the
  quantity most likely to differ from the average.

It is **keyed on the sequence number rather than on a call count** so that the
producer and the consumer can each evaluate the *same* schedule independently:
the producer asks about the sequence it is about to push, the consumer about the
sequence it has just popped. A call-counting sampler would be correct only while
both sides happened to call it the same number of times in the same order, which
is precisely the silent coupling this design removes. Agreement is not assumed —
it is **checked**: a sampled sequence must arrive carrying a real producer stamp
and an unsampled one must carry the sentinel, so a disagreement between the two
schedules fails the repetition instead of quietly shrinking the sample set.

### Where the timestamp lives, and why it is sparse

**The stamp travels inside the message.** A sampled message carries the
producer's raw `steady_clock` tick count in its own `ready_ticks` field, and
that field reaches the consumer through the same SPSC payload path as the rest
of the payload: producer → SPSC message → consumer. There is deliberately **no
side array, map or shared metadata structure** holding timestamps: a side array
is a second, independently addressed memory working set whose footprint scales
with **capacity**, which contaminates exactly the capacity comparison this phase
exists to make. (The superseded dataset did use one; see
`docs/results/spsc-tail-latency-superseded-per-message-timestamp/SUPERSEDED.md`.)

**Instrumentation is sparse, and the dataset proves it.** `Clock::now()` is
called *only* for sampled messages — ~9,696 times per repetition per thread, not
10,000,000. Two thread-owned counters record how many sampled clock reads each
side actually took, and both are written into the per-repetition summary:

| counter | canonical value | would be, if per-message |
|---|---|---|
| `producer_sample_clock_reads` | 9,696 | 10,000,000 |
| `consumer_sample_clock_reads` | 9,696 | 10,000,000 |
| `sample_count` | 9,696 | — |
| `stamp_contract_failures` | 0 | — |

All four are **required** values, not observations: a repetition whose counters
disagree with the derived count is a **failure**, and the raw→summary verifier
re-derives them from the raw files. Across the canonical dataset that is
**1,745,280** sampled clock reads per thread over 180 repetitions, against
**1,800,000,000** if every message were stamped — 0.097 % of the naive count.
The counts are ordinary non-atomic thread-local counters; no shared atomic
instrumentation exists to produce them, because adding one would perturb the
measurement it is meant to describe.

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
checksum equality above, the derived sample count, the **sparse-instrumentation
counters** (`producer_sample_clock_reads`, `consumer_sample_clock_reads` and
`stamp_contract_failures`, each required to equal its expected value), and the
cross-file dataset coverage. At the canonical
shape this is **5,240,804 checks over 1,745,280 raw samples**. A dataset is not
published unless all of them pass, and the verifier's output is recorded as the
dataset's `invariants.txt`.

## Two hazards that had to be fixed or ruled out

Both of these were found by running the thing, and both are recorded because a
methodology section that lists only the checks that pass is not a methodology
section.

### H1 — A timestamp transport that produced impossible samples

**Symptom.** A repetition failed its own timestamp gate: one sample had a
*negative* latency. Everything else passed — full delivery, sequence, payload
validation, exactly the derived sample count.

**Cause.** The timestamp was transported in a **separate side array**, an array
of `int64_t` indexed by ring slot (`s & (Capacity − 1)`). The producer must
stamp message `s` *before* it pushes `s`, so with the consumer's head at `h` it
can stamp index `h + Capacity` — one past the last message it has managed to
publish. With `Capacity` slots, that write lands on exactly the slot holding the
stamp for index `h`, i.e. the stamp the consumer is about to read for the
message it just popped. The consumer then subtracts a *later* timestamp from its
own and computes a negative latency.

It is rare — roughly one sample in 5×10⁷ on this host — because the window
between the pop and the stamp read is a few instructions. Rare is what makes it
dangerous: it survives smoke runs and appears once in a canonical run, where it
is easy to dismiss as noise.

**The design no longer has this hazard, because it no longer has this
structure.** There is no stamp array. A sampled message carries its stamp in its
own `ready_ticks` field, and that field is copied into the ring by the producer
and read out of the ring by the consumer as part of the payload — under the same
release/acquire pair that already orders every other field. The stamp for index
`s` can therefore only be observed by a consumer holding index `s`, because it
*is* index `s`. There is no second address to alias, so there is no window in
which one message's stamp can be read as another's.

An earlier Phase-4 revision patched the side array by doubling it to
`2 × Capacity` slots. That patch was real and did suppress the inversion, but it
treated the symptom: it kept the second memory path, and it made that path's
footprint **twice** as capacity-dependent — the exact contamination the sparse
redesign removes. The proof, the matched control that established it, and the
side-array reasoning itself are preserved as history in
`docs/results/spsc-tail-latency-superseded-per-message-timestamp/SUPERSEDED.md`,
and deliberately appear nowhere in the final architecture.

**What did not catch it, then or now.** AddressSanitizer, UndefinedBehaviorSanitizer
and ThreadSanitizer are all clean on this harness, before *and* after every
revision, and the malformed run passed every other gate. The bug was not a data
race — the release/acquire edge ordered the accesses correctly — it was a
*logical* indexing error that only the timestamp gate, and only occasionally,
could see. Sanitizers are not a substitute for an invariant that can fail.

**What replaced it as an invariant.** The current transport has its own
failure mode, and it is gated: if the producer's and the consumer's sample
schedules ever disagreed about a sequence, the message's own `ready_ticks` field
would contradict the receiver's expectation. `stamp_contract_failures` counts
exactly that, requires zero, and the raw→summary verifier re-derives it from the
raw files — 0 across all 180 canonical repetitions.

### H2 — Host interference that no correctness check can detect

**Symptom.** A complete 36-process canonical run — every gate passing, every
checksum matching, every verification check green — whose numbers were
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

**What detects it.** `ns_per_message` — `elapsed_ns / messages` — is an
end-to-end rate that is *reproducible* for a given cell (a few percent between
clean repetitions) precisely because it is not a latency. Reading it per
repetition against its own cell's median exposes a repetition that ran slowly
for reasons the queue had nothing to do with. The verifier reports every
repetition exceeding 5× its cell's median.

**It reports; it does not invalidate.** A threshold that deletes a dataset is
the wrong instrument here, and getting this wrong was itself an error of the
first design. **Phase 4 studies latency and jitter**, so a rare, extreme
repetition is part of the phenomenon being measured, not automatically an
artifact; a run whose tail was *legitimately* produced by a scheduler stall
would have been censored by a hard 5× rule — the most interesting observation in
the dataset deleted by a threshold. **Magnitude alone is not proof of
invalidity.** What justifies rejecting or archiving a whole run is *independent*
evidence of contamination — concurrent sanitizer or benchmark processes, a known
competing workload, explicitly observed interference — and none of that is
visible in these files, which is precisely why the check cannot make the call.
So the verifier prints a prominent **CONTAMINATION CANDIDATE** warning naming the
offending repetitions, requires independent evidence before anything is
discarded, and **does not fail the run**; warnings are printed before failures
and affect no exit code. The pre-flight host-load metadata is kept for the same
reason. On the canonical dataset this warning did **not** fire: no repetition
exceeded 5× its cell's median rate (worst cell spread 3.01×).

**Response.** The run is retained, unedited and clearly labelled, at
`docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/`, with its derived
aggregate tables **deleted** so that no quotable summary of it survives. That
directory is deliberately **not committed** — it is 106 MB of raw data for a run
that cannot be used — so a clone of this repository does not contain it. What the
repository keeps is this description and the automated diagnostic the run
motivated. The canonical dataset was re-measured on a quiet host. A pre-flight
load-average check was added to the runner, but it is explicitly a coarse guard:
that run started at load 6.10 on 16 CPUs, below any sane threshold, and the
interference arrived during the run. The post-hoc per-repetition check is the
authoritative one.

## Results

See `docs/results/spsc-tail-latency/` for the dataset, its provenance, its
invariants and the derived tables. **No number from the superseded
per-message-timestamp dataset is reused here.** Every figure below comes from the
sparse-sampled canonical run described above.

### How these numbers are aggregated — read this first

The design is **nested**: a cell has **4 sessions**, and a session has **5
measured repetitions**. That makes "the cell's P99" ambiguous, and the two
readings are **not mathematically identical**:

- **PRIMARY — session-blocked.** repetition → the median of that session's 5
  repetitions → **the median of the 4 session medians.** This is the figure to
  quote, and it is the one tabulated below.
- **SECONDARY — all repetitions.** repetition → the median across all 20
  repetition-level statistics. Reported in `CELL_SESSION_BLOCKED.csv` and
  `TAIL_MATRIX.md` as an explicitly labelled **diagnostic only**.

A median of medians is not a median of all values: the blocked figure weights
each *session* equally while the all-20 figure weights each *repetition*
equally, so with 5 repetitions per session the blocked median is a **weighted**
median of the same 20 numbers. For an odd number of sessions the two coincide;
for 4 they generally do not. Where they differ, the PRIMARY column is the cell's
value and the SECONDARY column is context — never a competing headline. They do
differ: 16 B/1024's P99 is **437 ns** session-blocked and **417 ns** across all
20.

The session-blocked level is primary because of how the data was collected: the
5 repetitions inside one session share a process, a thread placement and a
moment in time, so a cell's 20 repetitions are not 20 exchangeable observations.
Collapsing each session first stops one session that behaved differently from
dominating the cell's summary.

`CELL_SESSION_BLOCKED.csv` also carries, for every cell and every metric, the
**min and max session median** — the spread the blocked median sits inside, and
the first thing to look at before quoting any single number. `CELL_TAIL.csv`,
`TAIL_MATRIX.md` and `TAIL_RATIOS.csv` are derived from the same summaries by
`scripts/analyze-spsc-tail.py`.

**No distribution is pooled.** The benchmark records one distribution per
measured repetition and never merges them; the analysis aggregates the
*per-repetition statistics*. "P99 = X" below means "the median repetition
exhibited a P99 of X", never "99% of all samples were under X". Pooling 5
repetitions would let a between-repetition regime shift masquerade as a
within-repetition tail, which is the one artifact a tail study must not create.

**Dataset.** 36 processes, 180 measured repetitions, 1,745,280 sampled
latencies, 1,745,280 sampled clock reads per thread, 0 stamp-contract failures,
collected on the Apple M3 Max. Provenance in the dataset's `PROVENANCE.md`,
invariants in its `invariants.txt`.

Every claim below is labelled: **MEASURED** (an observed value), **DERIVED**
(computed from measured values), **INTERPRETATION** (a proposed cause, never
asserted as fact — nothing here is profiled), **LIMITATION** (what qualifies the
claim).

### Q1 — What are the session-blocked P50 / P90 / P99 / P99.9 / max values?

DERIVED — PRIMARY (session-blocked) values in ns, with the median `ns_per_message`
for reference:

| cell | P50 | P90 | P99 | P99.9 | max | P99/P50 | ns/msg |
|---|---|---|---|---|---|---|---|
| 16 B / 1024 B | **125** | 167 | 437 | 5,896 | 79,062 | 3.5 | 32.88 |
| 16 B / 4096 B | **125** | 167 | 458 | 8,395 | 23,666 | 3.7 | 32.32 |
| 16 B / 65536 B | **125** | 125 | 645 | 7,000 | 26,396 | 5.2 | 36.14 |
| 32 B / 1024 B | **125** | 167 | 729 | 11,437 | 75,750 | 5.8 | 29.11 |
| 32 B / 4096 B | **125** | 166 | 708 | 20,312 | 54,833 | 5.7 | 41.54 |
| 32 B / 65536 B | **125** | 125 | 438 | 7,458 | 31,729 | 3.5 | 41.16 |
| 64 B / 1024 B | **30,042** | 31,437 | 71,687 | 103,104 | 212,437 | 2.4 | 30.74 |
| 64 B / 4096 B | **120,396** | 128,562 | 279,333 | 361,625 | 489,375 | 2.3 | 31.07 |
| 64 B / 65536 B | **1,952,833** | 2,025,083 | 3,771,208 | 4,055,666 | 4,088,416 | 1.9 | 31.07 |

DERIVED — the spread each PRIMARY value sits inside (max session median ÷ min
session median, over the 4 sessions). This is the qualifier on every number
above:

| cell | P50 spread | P90 | P99 | P99.9 | max |
|---|---|---|---|---|---|
| 16 B / 1024 B | 1.00× | 1.00× | 2.49× | 4.89× | 14.07× |
| 16 B / 4096 B | 1.00× | 1.01× | 1.57× | 2.32× | 3.40× |
| 16 B / 65536 B | 1.00× | 1.33× | 1.31× | 1.70× | 2.30× |
| 32 B / 1024 B | 1.00× | 1.01× | 5.99× | 2.90× | 12.10× |
| 32 B / 4096 B | 1.00× | 1.34× | 3.17× | 3.78× | 3.97× |
| 32 B / 65536 B | 1.00× | 4.66× | 1082.33× | 386.02× | 127.63× |
| 64 B / 1024 B | 1.09× | 2.26× | 1.45× | 1.39× | 8.87× |
| 64 B / 4096 B | 1.07× | 1.03× | 1.21× | 1.27× | 1.54× |
| 64 B / 65536 B | 1.05× | 1.05× | 1.09× | 1.05× | 1.06× |

MEASURED. The nine cells fall into **two clean groups**: six whose P50 is
**125 ns** at every capacity, and three — all of them 64 B — at 30 µs, 120 µs and
1.95 ms, one per capacity.

### Q2 — How reproducible is each percentile across repetitions and sessions?

MEASURED. This is two different questions and they have different answers.

**Across sessions** — the spread table above. P50 spread is **1.00×** in six of
nine cells and 1.05–1.09× in the other three; it is the most reproducible
quantity in the dataset. P99.9 and max are the least. And **one cell is an
outlier of its own**: 32 B/65536's P99 spread is **1082×**, driven by a single
session whose median P99 was 270,583 ns against 250–792 ns in the other three.

**Across all 20 repetitions** of each cell, worst case over the nine cells:

| statistic | worst spread over 20 reps | where |
|---|---|---|
| P50 | 1.11× | 64 B / 65536 B (1.00× in six cells) |
| P90 | 6.00× | 32 B / 65536 B |
| P99 | 13,469× | 32 B / 65536 B |
| P99.9 | 19,250× | 32 B / 65536 B |
| max | 19,271× | 16 B / 1024 B |

DERIVED. The ordering is stable and it is the **opposite of the usual
expectation in one respect: P50 is the most reproducible statistic here, not the
least.** For the six fast-band cells it is not merely stable but **constant** —
125 ns in all 120 of their repetitions, spread 1.00×. What instability remains
is concentrated in P99.9 and max, which are single observations by construction.

LIMITATION. These spreads are properties of this host over this run. Nothing here
separates a cell's own variability from the host's: the harness records no
preemption count, no core identity and no frequency, so the movement is a
measured property of the runs and nothing more. The 32 B/65536 outlier is
reported under Q6 and is not explained here.

### Q3 — How much wider is P99.9 than P50?

DERIVED — PRIMARY (session-blocked) ratios, with the max for context:

| cell | P99.9 / P50 | P99 / P50 | max / P99.9 |
|---|---|---|---|
| 16 B / 1024 B | 47.2× | 3.5× | 13.41× |
| 16 B / 4096 B | 67.2× | 3.7× | 2.82× |
| 16 B / 65536 B | 56.0× | 5.2× | 3.77× |
| 32 B / 1024 B | 91.5× | 5.8× | 6.62× |
| 32 B / 4096 B | 162.5× | 5.7× | 2.70× |
| 32 B / 65536 B | 59.7× | 3.5× | 4.25× |
| 64 B / 1024 B | 3.4× | 2.4× | 2.06× |
| 64 B / 4096 B | 3.0× | 2.3× | 1.35× |
| 64 B / 65536 B | 2.1× | 1.9× | 1.01× |

MEASURED. The ratio spans **two orders of magnitude across the matrix**: 47–163×
in the six fast-band cells, only 2.1–3.4× in the three slow-band ones. The
absolute tail height is *not* what separates them — 16 B/65536's P99.9 (7,000 ns)
is an order of magnitude *lower* than 64 B/1024's (103,104 ns) — it is the
**floor**. Where P50 sits at 125 ns, any excursion at all is a large multiple of
it; where P50 is already 1.95 ms, the tail has only 2.1× of headroom left to
move. **A tail ratio is therefore not comparable between two cells whose medians
sit in different bands**, and none of these ratios is a property of the queue.
`TAIL_RATIOS.csv` is published for completeness, not for ranking.

MEASURED. In the two largest-capacity 64 B cells the maximum is barely above
P99.9 — 1.35× and **1.01×**. At 9,696 samples, P99.9 is the ~10th-largest
observation, so in a cell whose samples cluster tightly the two statistics
describe nearly the same point. This is why P99.99 is not reported: it would be
the maximum wearing a percentile's label.

### Q4 — How does capacity relate to producer backpressure and measured latency?

MEASURED — retries summed over each cell's 20 repetitions (200M messages each):

| cell | P50 | producer full-queue retries | consumer empty-queue retries |
|---|---|---|---|
| 16 B / 1024 B | 125 | 4,390,342 | 1,360,124,233 |
| 16 B / 4096 B | 125 | 232,176 | 1,462,849,807 |
| 16 B / 65536 B | 125 | 394,240 | 1,775,052,956 |
| 32 B / 1024 B | 125 | 1,034,197 | 929,296,210 |
| 32 B / 4096 B | 125 | 14,851 | 2,138,905,364 |
| 32 B / 65536 B | 125 | 1,159,802 | 2,540,613,771 |
| 64 B / 1024 B | 30,042 | 204,000,536 | 3,863,928 |
| 64 B / 4096 B | 120,396 | 77,534,424 | 544,316 |
| 64 B / 65536 B | 1,952,833 | 26,425,067 | 12,311 |

MEASURED. The two groups differ in **which thread waits**. In the six fast-band
cells the *consumer* spins empty 0.93–2.5 **billion** times while the producer
almost never finds the ring full: those runs are consumer-starved, the ring is
essentially empty, and the producer is free to run ahead. In the three 64 B cells
that inverts almost exactly — the producer is blocked **26–204 million** times
and the consumer spins empty only 12 thousand to 3.9 million times. The producer
is the constrained side and the ring stays full.

DERIVED. Within the 64 B group, larger capacity means **fewer** producer stalls
but **longer** measured latency — the opposite of the naive reading:

| cell | capacity | P50 | P50 ÷ (capacity × ns/msg) |
|---|---|---|---|
| 64 B / 1024 B | 1,024 | 30,042 | 0.937 |
| 64 B / 4096 B | 4,096 | 120,396 | 0.946 |
| 64 B / 65536 B | 65,536 | 1,952,833 | 0.965 |

P50 is **94–97% of one full ring's drain time**, `capacity × ns_per_message`, in
all three cells. INTERPRETATION, offered only as a hypothesis these numbers are
consistent with: the producer fills the ring and stays ahead, so a sampled
message is enqueued behind a full ring of work and waits for the consumer to
drain everything ahead of it — and the wait is set by how much ring there is to
drain, not by how often the producer is blocked. LIMITATION: producer lead is not
instrumented, so the drain model is **not confirmed** by this dataset, and no
cache, coherence or scheduler cause is asserted for why the 64 B cells are the
slow ones.

MEASURED, for contrast: in the six fast-band cells the same product is
`1024 × 32.88 = 33.7 µs` against a measured P50 of 125 ns. The ring is nowhere
near full there and the drain model does not apply at all.

### Q5 — Which observations are timer-resolution-limited?

MEASURED. The per-process calibration (200,000 back-to-back `steady_clock::now()`
pairs, run in all 36 processes outside the timed transfer) reports only **41, 42,
83, 84 and 125 ns** as non-zero pair costs — one clock quantum and its multiples.
The median pair cost is 0 ns in **every one of the 36 processes** (consecutive
reads landing inside one tick), and the median across processes of the
per-process P99 is **42 ns**. The clock quantum on this host is therefore
**≈ 41.7 ns**, and it is a hard floor on resolution, not a correction factor.

MEASURED — share of each cell's 193,920 samples within a few quanta of that
floor:

| cell | ≤ 1 µs | ≤ 167 ns (4 quanta) | exactly 125 ns (3 quanta) |
|---|---|---|---|
| 16 B / 1024 B | 99.43% | 94.55% | 63.55% |
| 16 B / 4096 B | 99.45% | 95.17% | 65.76% |
| 16 B / 65536 B | 99.45% | 94.78% | 71.80% |
| 32 B / 1024 B | 99.27% | 93.15% | 53.24% |
| 32 B / 4096 B | 99.34% | 94.43% | 61.85% |
| 32 B / 65536 B | 98.69% | 92.24% | 64.80% |
| 64 B / 1024 B | 0.20% | 0.05% | 0.03% |
| 64 B / 4096 B | 0.00% | 0.00% | 0.00% |
| 64 B / 65536 B | 0.00% | 0.00% | 0.00% |

DERIVED. In the six fast-band cells **92–95% of all samples lie within four clock
quanta of zero and 53–72% are exactly 125 ns** (3 × 41.7 = 125.1). Those cells'
P50, P90 and much of their P99 are **timer-resolution-limited** — strongly
timer-quantized. The measured distribution there is only a few quanta wide, and
its values carry information at the granularity of the clock rather than of the
queue. The slow-band cells are not timer-limited at all (0.00% within a
microsecond): their distributions are measured with three to four orders of
magnitude of headroom.

**What this does and does not license.** Sparse timestamping substantially
reduces measurement perturbation — 1,745,280 sampled clock reads per thread
instead of 1,800,000,000 — but a sampled message still contains **two** clock
reads, so the instrumented path is not free and a sampled handoff still cannot be
resolved below one quantum. What can be said about the fast band is that its
percentiles are **timer-resolution-limited**. What **cannot** be said is that
"the latency is the timer, not the queue": the queue is doing real work at a
scale this clock can only bound, not resolve. LIMITATION: this dataset contains
no clock-independent measurement of the fast-band handoff, so no value below
~42 ns is claimed and no claim is made about the true handoff cost. The smallest
sample in the dataset is 0 ns, for the same reason the calibration's median
pair cost is 0 ns — two reads inside one tick.

The calibration remains **descriptive only** and is **never subtracted** from any
latency sample, here or anywhere else in this document.

### Q6 — Are extreme maxima isolated observations or representative tails?

MEASURED. Mostly isolated, with one cell where they are not. Counting, per cell,
how many of the 20 repetitions have a maximum above 5× that cell's own median
maximum:

| cell | median max | worst max (session, rep) | reps > 5× | reps > 100× |
|---|---|---|---|---|
| 16 B / 1024 B | 49,750 | 17,671,958 (s2 r1) | 3 / 20 | 2 |
| 16 B / 4096 B | 26,896 | 533,375 (s2 r5) | 1 / 20 | 0 |
| 16 B / 65536 B | 26,896 | 5,121,834 (s1 r5) | 1 / 20 | 1 |
| 32 B / 1024 B | 47,562 | 254,209 (s1 r1) | 1 / 20 | 0 |
| 32 B / 4096 B | 58,896 | 1,248,334 (s4 r2) | 1 / 20 | 0 |
| **32 B / 65536 B** | 37,292 | 20,416,833 (s3 r5) | **7 / 20** | 1 |
| 64 B / 1024 B | 143,416 | 5,071,959 (s3 r4) | 3 / 20 | 0 |
| 64 B / 4096 B | 543,375 | 3,662,708 (s3 r1) | 1 / 20 | 0 |
| 64 B / 65536 B | 4,111,250 | 6,337,667 (s1 r3) | 0 / 20 | 0 |

DERIVED. In seven of the nine cells the extreme maximum is **one or two
observations in twenty** against a well-separated baseline: isolated events, not
a representative tail. In 32 B/65536 it is neither — **7 of 20** repetitions
exceed 5× the cell's median maximum, and three of them are consecutive
repetitions (s3 r1, r4, r5) inside a **single process**, at 1.85 ms, 3.62 ms and
20.4 ms. That clustering is a property of one process, not of the cell.

LIMITATION. The harness cannot see **why**. It records no preemption count and no
core identity, and no sanitizer or competing workload was observed running
alongside this run. By the H2 rule this is a **contamination candidate, not a
verdict**: it is reported, named and left un-censored, because a rule that
deleted extreme repetitions on magnitude alone would delete exactly the
phenomenon a tail study exists to measure. Note also that **these repetitions
were not slow in aggregate** — the worst `ns_per_message` spread in the whole
dataset is 3.01×, well under the 5× diagnostic threshold, and the verifier raised
no warning — so whatever happened was localized in time, not a whole-repetition
slowdown.

### Q7 — Does any previously observed fast/slow mode survive sparse instrumentation?

MEASURED — a direct comparison of *phenomena*, old dataset against new. These are
two different harnesses, so no absolute value is compared across them:

| observation (superseded dataset) | sparse-sampled dataset |
|---|---|
| P50 within a cell spanned up to **709×** (125 → 88,625 ns, 32 B/4096) | P50 within a cell spans **1.00×** in six cells, 1.05–1.11× in the rest |
| P50 was unstable *within* a cell, between repetitions | P50 is **constant at 125 ns** in all 120 repetitions of the six fast-band cells |
| P99 the most stable statistic, P50 among the least | **reversed** — P50 is now the most reproducible, P99.9/max the least |
| fast/slow bands did not track size or capacity cleanly | bands separate cleanly: **16/32 B fast, 64 B slow**, at every capacity |
| slow-band P50 ÷ (capacity × ns/msg) = 0.66, 0.81, 0.82 | = **0.937, 0.946, 0.965** |
| 81 of 180 repetitions in the fast band | **120 of 180** — the same six cells, every repetition |

DERIVED. **The unstable mode does not survive. The bimodality does**, in a
sharper and entirely stable form. The superseded harness showed a median that
wandered between a fast band and a slow band *within one cell*, by up to 709×,
between repetitions of an identical configuration. The sparse-sampled harness
shows the same nine cells splitting into two groups — six pinned at 125 ns in
every single repetition, three in a slow band stable to ~1.10× — with no cell
straddling them.

INTERPRETATION, and only that: this is consistent with the old instrumentation
having perturbed the runs it was measuring. MEASURED support — the same cells run
**1.1× to 4× faster end-to-end** under sparse instrumentation (16 B/1024's median
`ns_per_message` falls from ~49 ns to 32.9 ns). LIMITATION: the two datasets
differ in more than instrumentation, and neither harness varied instrumentation
as a controlled factor, so **this is a hypothesis, not an attribution**. Nothing
here licenses the claim that the superseded dataset's numbers were wrong: every
one of them was a real measurement of the harness that produced it. What is
claimed is narrower and firmer — **that dataset's characterization of the queue
does not reproduce, and no Phase-4 conclusion may rest on it.**

### Retired claims

Observations from the superseded dataset that do **not** survive, retired here
and retained only as historical observations of that harness:

- **"The fast mode is the timer, not the queue."** Retired in that phrasing. The
  fast band is **timer-resolution-limited** (Q5); the queue is doing real work
  below the clock's resolution, and this dataset bounds it without resolving it.
- **"Capacity and message size do not determine the band."** Reversed: on the new
  dataset they determine it cleanly — message size separates fast from slow, and
  capacity sets the depth within the slow band (Q4).
- **"Throughput is reproducible where latency is not."** No longer a contrast. On
  the new dataset the *latency* is the more reproducible quantity: P50 spread
  1.00–1.09× between sessions against `ns_per_message` spread 1.03–1.57×.

### The extremes

MEASURED. The largest single latency in the dataset is **20,416,833 ns (20.4 ms)**
in `b32_c65536_s3`. Each extreme is **one sample out of 9,696** in its
repetition. LIMITATION: a single context switch inside one sampled interval
produces exactly this signature, and the harness counts neither switches nor
placements, so **no cause is assigned to any extreme value in this dataset.**

### Correctness

MEASURED. All 36 processes and all 180 measured repetitions report
`correctness=PASS`: full delivery (10,000,000/10,000,000 in every repetition),
FIFO sequence intact, payload validation clean (0 mismatches), 0 timestamp
inversions, 0 stamp-contract failures, and exactly the derived 9,696 samples
every time. **5,240,804** independent raw→summary checks passed with 0 failures,
and every repetition's instrumented clock-read counters equal 9,696 on both
threads — the dataset proves its own instrumentation was sparse. The full record
is `docs/results/spsc-tail-latency/invariants.txt`.

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

**Experiment 02 Phase 4 — COMPLETE / FROZEN.** The canonical dataset is
`docs/results/spsc-tail-latency/`: 36 processes, 180 measured repetitions,
1,745,280 sampled latencies, 5,240,804 raw→summary checks passed with 0
failures, and per-repetition instrumentation counters proving that clock reads
were sparse (9,696 per thread per repetition, not 10,000,000).

Experiment 02 — SPSC is **COMPLETE**. Phase 4 is a **measurement** phase: it
characterizes the frozen queue and opens no new optimization. No further SPSC
optimization phase is started from here. See the top-level `README.md` for the
canonical status line.
