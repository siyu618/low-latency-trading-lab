# SUPERSEDED — per-message timestamp instrumentation

**Status: real data, valid gates, wrong instrumentation. Do not cite any number
in this directory as a final Phase-4 latency result.**

This is the first correctly-measured Phase-4 dataset. It is archived, not
deleted, and every file in it is exactly as it was produced.

## What is real about it

- **All measurements are real.** 36 processes, 180 measured repetitions,
  1,745,280 sampled latencies, collected on the Apple M3 Max.
- **All correctness checks passed.** Every repetition delivered every message in
  strict sequence, every payload passed its validator, and the per-message-size
  checksum was identical across all 20 repetitions of each size.
- **All raw→summary verification passed.** 5,240,255 independent checks, 0
  failures, 0 timestamp inversions. Every percentile in the derived tables was
  recomputed from the raw samples rather than trusted.
- **No raw numeric result was edited.** The raw per-sample CSVs, the summaries,
  the calibrations and the derived tables in this directory are byte-for-byte
  what the run produced.

Nothing here is fabricated, and nothing here is a failed gate.

## Why it is superseded

The latency instrumentation was **not sparse**, and it used a **second memory
path** that the intended design does not have.

- **`Clock::now()` was executed for every message.** The producer took a
  timestamp for all 10,000,000 messages of every repetition, not only for the
  9,696 that were sampled.
- **`Clock::now()` was executed after every successful pop.** The consumer took
  a receipt timestamp for all 10,000,000 messages, not only for the sampled ones.
- **Sampling only controlled which latency values were retained.** The
  `SamplingCountdown` decided which of the already-measured latencies were stored
  in the sample vector. It did not reduce the number of clock reads, and it did
  not reduce the work done per message.
- **A separate `ready_ticks[2 * Capacity]` array carried the producer stamps.**
  This is the part that matters most for interpretation. The queue payload is
  `Capacity * sizeof(Msg)` bytes; the timestamp array added a further
  `2 * Capacity * 8` bytes of *separately addressed* memory that the producer
  wrote and the consumer read on every message. It is a second, independent
  memory working set whose size scales with **capacity**, so it contaminates
  precisely the capacity comparison this phase is supposed to make.

The consequence is that a capacity-dependent memory footprint was varied
alongside capacity itself. Any capacity trend in this dataset is confounded with
the size of a side array that the final design does not contain. Two clock reads
per message also put instrumentation cost on every message rather than on the
1-in-1021 that is actually sampled.

## What the final design does instead

- The producer-ready timestamp **travels inside the sampled message**, through
  the same SPSC payload path — `producer -> SPSC message -> consumer` — with no
  side array, no map and no shared metadata structure.
- `Clock::now()` is called **only for sampled messages**: approximately 9,696
  producer reads and 9,696 consumer reads per repetition, not 10,000,000 each.
- The message payload stays exactly 16 / 32 / 64 bytes with the stamp in it.

The sampled **sequence indices are unchanged**. The final design's schedule
(`next_sample_seq = settling + interval - 1`, advancing by `interval`) selects
exactly the same messages the `SamplingCountdown` selected here, so the sample
count remains 9,696 per repetition. Any difference between this dataset and the
final one is therefore attributable to the instrumentation path, not to a
different sample of messages. That is a statement about which messages are
sampled, not a prediction that any result will reproduce — see below.

## Historical observations — superseded harness only

The analysis written against this dataset reported the following. They are
retained here **as observations of this harness**, and they are not carried
forward as Phase-4 conclusions. The final dataset is analysed from scratch and
none of these is assumed to survive sparse timestamping:

- a fast band near the clock floor and a slow band whose P50 tracked
  `capacity x ns_per_message`;
- very large P50 and P90 spread across repetitions of an identical
  configuration (up to ~379x and ~442x), with P99 comparatively stable;
- an end-to-end `ns_per_message` that was reproducible (1.12x-1.70x) where the
  latency statistics were not;
- the observation that capacity and message size did not determine which band a
  cell landed in.

Some or all of these may be artifacts of per-message timestamping and of the
`2 * Capacity` array. Treat them as hypotheses to be re-tested, not as findings.

The write-up that reported them — including its per-cell table, its five
findings and its discussion of what could not be claimed — remains in the
repository's history at the commit that introduced this directory, and the
phase document `docs/SPSC_TAIL_LATENCY.md` was rewritten against the final
dataset.

## Preserved for the record: the side-array aliasing argument

The final benchmark has no timestamp side array and therefore no need for this
argument. It is kept here because it is the reason the array was sized
`2 * Capacity` rather than `Capacity`, and because the failure it prevents is
worth remembering: it is silent, rare and corrupting rather than loud.

> Indexing the array by the ring slot (`s & (Capacity - 1)`) would have been
> wrong, and wrong in a way that corrupts samples rather than failing loudly.
> The producer writes the stamp for message `s` before it pushes `s`, so it can
> write the stamp for index `h + Capacity` while the consumer's head is still
> `h` — the ring allows `Capacity` messages in flight, and the producer writes
> its stamp for the message it is *about* to push, one past the last one it
> managed to publish. With `Capacity` slots that write lands on exactly the slot
> holding the stamp for index `h`, i.e. on the stamp the consumer is about to
> read for the message it just popped. The consumer then subtracts a *later*
> timestamp from its own and computes a negative latency — a sample that is not
> merely noisy but impossible. It is rare (roughly one in 5e7 samples on this
> host) because the window between the pop and the stamp read is a few
> instructions, which is precisely what makes it dangerous.
>
> With `2 * Capacity` slots addressed by a power-of-two mask, let the consumer
> have just popped index `c`, so the shared head is `c + 1`. The producer can
> publish at most index `head + Capacity - 1` and can therefore stamp at most
> index `head + Capacity = c + 1 + Capacity`. The next index sharing `c`'s slot
> is `c + 2 * Capacity`, which is unreachable because
> `c + 1 + Capacity < c + 2 * Capacity` for every `Capacity >= 2`. So the stamp
> for index `c` is intact from the moment it is written until the moment it is
> read.
>
> Visibility was the ring's own release/acquire pair, exactly as for the
> payload: the stamp store was sequenced before the release store that published
> the message, and the consumer's acquire load of the cursor synchronized with
> it. The plain (non-atomic) store and load were therefore correctly ordered,
> not a race.

Note that no timestamp inversion was ever observed in this dataset's 1,745,280
samples, which is consistent with the estimate above: the window is narrow
enough that a run of this size would not be expected to hit it. The design was
chosen so that it could not happen at all.

## What must not be done with this directory

- Do not cite its percentiles as Phase-4 results.
- Do not compare its absolute numbers against the final dataset as though the
  difference were a queue property. The instrumentation path differs.
- Do not run `scripts/analyze-spsc-tail.py` against it expecting success: the
  analysis gate requires a dataset whose `invariants.txt` records sparse
  timestamp counters, which this one predates.

## Note on this directory's recorded source hashes

This directory's `HOST.md` and `PROVENANCE.md` record the sha256 of each source
file as it was on disk when this dataset was collected. Those files have since
changed — Phase 4.1 replaced the per-message instrumentation with the sparse
sequence-keyed schedule — so **those digests no longer match the working tree,
and are not expected to.** They remain an accurate record of what produced *this*
dataset, which is all they ever claimed to be. The audit of what changed, and
the reconstruction that proves the later comment-only pass touched no timed code,
is in `../spsc-tail-latency/PROVENANCE.md`. Nothing in this directory was edited
to make it match anything.
