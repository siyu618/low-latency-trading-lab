# Experiment 02 Phase 3B — paired-session comparison

## What is being compared

The two implementations compared here differ in **one intended algorithmic
treatment: remote-cursor caching**. Both come from one algorithm body parameterised by a
compile-time mode, so the payload storage, the payload offset, the object
size, the SEPARATED cursor placement, the capacity, the slot indexing, the
publication protocol, the retry/yield harness and the message types are
identical by construction. There is no batching, no changed memory ordering,
no CAS, no MPSC/MPMC, no affinity and no NUMA tuning in either variant.

**Reduced remote-load frequency is the primary mechanism of that treatment, but
the treatment also carries its local fast-path bookkeeping cost** — a
thread-owned cached-cursor read, a comparison and a branch, plus occasional
cached-value updates. The two are not separated by this design, so **no
throughput difference here isolates the cost of a single remote atomic load.**
Nor are the variants the same machine code: they are distinct template
instantiations with different emitted instruction sequences.

**The release/acquire publication edge exists in BOTH variants and was not
weakened.** The producer still publishes with a release store to `head` and
the consumer still reads the payload only after an acquire observation of
`head`. The cached copy changes how OFTEN the remote cursor is read, not
what reading it guarantees. A stale cached value can only produce a FALSE
FULL or a FALSE EMPTY — never a reused slot and never a read of unpublished
data.

`ratio` = **cached median / baseline median** within a session.
**`ratio < 1` means the cached variant completed a message faster** in that
session.

## Why paired, and not the pooled table

Under the balanced AB/BA design, the baseline process and the cached process
for a given `(message_bytes, capacity)` run as **adjacent processes**, with
variant order swapped between sessions. **Adjacent execution reduces temporal
drift between the two legs but cannot guarantee identical scheduler, DVFS,
thermal, or background-system state** — the two processes are still separated
by a full benchmark run, and each leg can be placed, migrated or
frequency-scaled independently. The pairing narrows the gap; it does not
close it.

Pooling the raw repetitions instead would treat the 5 repetitions inside
one process as independent placements. They are not: a repetition does
create a fresh producer/consumer thread pair and a fresh queue object, but it
shares its process's address space, allocator state and thermal history with
its siblings. The pooled view is kept in `MATRIX.md` as a **secondary,
descriptive** metric.

## Per-session detail

`first` is the variant that ran first in that session's pair.

| bytes | capacity | session | first | baseline ns/msg | cached ns/msg | ratio |
|---|---|---|---|---|---|---|
| 8 | 1024 | 1 | baseline | 46.130529 | 48.974521 | 1.0617 |
| 8 | 1024 | 2 | cached | 43.942800 | 48.898000 | 1.1128 |
| 8 | 1024 | 3 | cached | 39.867337 | 51.329000 | 1.2875 |
| 8 | 1024 | 4 | baseline | 40.749938 | 43.247279 | 1.0613 |
| 8 | 4096 | 1 | baseline | 23.473658 | 54.584617 | 2.3254 |
| 8 | 4096 | 2 | cached | 26.291342 | 54.894962 | 2.0879 |
| 8 | 4096 | 3 | cached | 29.585225 | 56.848758 | 1.9215 |
| 8 | 4096 | 4 | baseline | 47.945746 | 50.125267 | 1.0455 |
| 8 | 65536 | 1 | baseline | 59.871571 | 64.659671 | 1.0800 |
| 8 | 65536 | 2 | cached | 55.894283 | 66.368217 | 1.1874 |
| 8 | 65536 | 3 | cached | 59.651129 | 61.184633 | 1.0257 |
| 8 | 65536 | 4 | baseline | 59.210375 | 66.676992 | 1.1261 |
| 32 | 1024 | 1 | baseline | 54.782050 | 49.424254 | 0.9022 |
| 32 | 1024 | 2 | cached | 42.338229 | 48.021963 | 1.1342 |
| 32 | 1024 | 3 | cached | 44.713604 | 50.487092 | 1.1291 |
| 32 | 1024 | 4 | baseline | 45.747996 | 49.614096 | 1.0845 |
| 32 | 4096 | 1 | baseline | 61.296771 | 60.547737 | 0.9878 |
| 32 | 4096 | 2 | cached | 55.813012 | 62.317446 | 1.1165 |
| 32 | 4096 | 3 | cached | 56.814125 | 60.558437 | 1.0659 |
| 32 | 4096 | 4 | baseline | 60.054096 | 62.978887 | 1.0487 |
| 32 | 65536 | 1 | baseline | 68.979671 | 76.261271 | 1.1056 |
| 32 | 65536 | 2 | cached | 72.106133 | 78.167142 | 1.0841 |
| 32 | 65536 | 3 | cached | 72.174425 | 70.274171 | 0.9737 |
| 32 | 65536 | 4 | baseline | 71.766425 | 70.451942 | 0.9817 |
| 64 | 1024 | 1 | baseline | 54.940583 | 67.299892 | 1.2250 |
| 64 | 1024 | 2 | cached | 54.127321 | 68.658725 | 1.2685 |
| 64 | 1024 | 3 | cached | 52.514763 | 68.094567 | 1.2967 |
| 64 | 1024 | 4 | baseline | 53.073696 | 67.498967 | 1.2718 |
| 64 | 4096 | 1 | baseline | 48.358183 | 65.747471 | 1.3596 |
| 64 | 4096 | 2 | cached | 48.564712 | 69.215342 | 1.4252 |
| 64 | 4096 | 3 | cached | 50.200483 | 69.634304 | 1.3871 |
| 64 | 4096 | 4 | baseline | 47.602913 | 68.974179 | 1.4489 |
| 64 | 65536 | 1 | baseline | 44.473450 | 68.781675 | 1.5466 |
| 64 | 65536 | 2 | cached | 41.461537 | 67.064846 | 1.6175 |
| 64 | 65536 | 3 | cached | 43.991125 | 69.968775 | 1.5905 |
| 64 | 65536 | 4 | baseline | 43.876371 | 66.619788 | 1.5184 |

## Per-cell summary across the 4 session ratios

MEDIAN / MIN / MAX are over the 4 paired ratios. SIGN counts how many
sessions put cached ahead (< 1) and how many put baseline ahead (> 1).
A cell whose 4 ratios do not all point the same way is **not
directionally stable**, whatever its median ratio says. This is a
descriptive criterion, not a significance test.

| bytes | capacity | median ratio | min | max | cached faster | baseline faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 1.087208 | 1.061285 | 1.287495 | 0 | 4 | stable — baseline 4/4 |
| 8 | 4096 | 2.004736 | 1.045458 | 2.325356 | 0 | 4 | stable — baseline 4/4 |
| 8 | 65536 | 1.103038 | 1.025708 | 1.187388 | 0 | 4 | stable — baseline 4/4 |
| 32 | 1024 | 1.106815 | 0.902198 | 1.134246 | 1 | 3 | SPLIT 1-3 |
| 32 | 4096 | 1.057304 | 0.987780 | 1.116540 | 1 | 3 | SPLIT 1-3 |
| 32 | 65536 | 1.032871 | 0.973671 | 1.105562 | 2 | 2 | SPLIT 2-2 |
| 64 | 1024 | 1.270132 | 1.224958 | 1.296675 | 0 | 4 | stable — baseline 4/4 |
| 64 | 4096 | 1.406172 | 1.359593 | 1.448949 | 0 | 4 | stable — baseline 4/4 |
| 64 | 65536 | 1.568549 | 1.518352 | 1.617520 | 0 | 4 | stable — baseline 4/4 |

## Reading these numbers

- **MEDIAN RATIO** is the headline paired figure: < 1 means cached was faster,
  > 1 means baseline was faster.
- **MIN / MAX** show whether that median is representative or is averaging
  over sessions that disagreed. They are the extremes of 4 correlated
  observations, not a confidence interval.
- **DIRECTION STABILITY** is the attribution check. `stable` means all
  4 sessions agreed; `SPLIT` means they did not, and **no directional
  claim should be made for that cell** regardless of its median.
- These are **descriptive** statistics over 4 paired observations per
  cell. They are not a hypothesis test and no p-value is implied.

## Limitations

- 4 paired observations per cell is a small sample; `stable` means
  "4 out of 4 agreed here", not "the effect is proven".
- No CPU pinning or affinity is used or claimed; macOS may migrate threads
  mid-run and may place the two processes' threads on different core types.
- **This cell shape exhibits strong run-to-run and build-to-build regime
  variation on the development host, and the swing between regimes is larger
  than any plausible treatment effect.** A process can settle into a state
  where the consumer spins on an empty queue tens of millions of times instead
  of blocking on real handoffs. A separate diagnostic suggested code-layout
  sensitivity as **one possible contributor** to that bimodality, but **Phase 3B
  does not isolate its cause** — and no reproducible diagnostic package is
  preserved alongside this dataset. Note also that the two variants are distinct
  template instantiations with different emitted code, and the two legs are
  independent processes that are not guaranteed to share scheduler placement,
  core type, migration history, DVFS, thermal state or background load. The
  `producer_full_retries` and `consumer_empty_retries` columns are preserved in
  `summary.csv` and in every raw CSV precisely so the state of each process is
  visible. **No ratio here should be read as resolving a difference smaller
  than that swing.**
- The mechanism leg (`mechanism/`, `MECHANISM.md`) is a SEPARATE set of
  runs at a different instrumentation setting. Its throughput numbers are NOT
  the canonical figures and are not used here.
- ns_per_message is end-to-end elapsed / messages delivered, including queue
  synchronization, payload assignment, coherence traffic, harness
  retry/backpressure and OS scheduling. It is not a per-call latency and not
  a one-way handoff time.
