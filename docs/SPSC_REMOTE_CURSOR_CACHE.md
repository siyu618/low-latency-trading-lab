# Experiment 02 — SPSC Ring Buffer: Remote Cursor Caching (Phase 3B)

**Scope.** This document is the *measurement* counterpart to
`docs/SPSC_MEMORY_MODEL.md` (Phase 1, *why the protocol is correct*),
`docs/SPSC_THROUGHPUT.md` (Phase 2, *how fast the frozen protocol is*) and
`docs/SPSC_FALSE_SHARING.md` (Phase 3A, *what cursor cache-line placement
changes*). Phase 3B asks a different and narrower question than any of them:

> **Of the synchronization traffic the SPSC performs, how much of the wall
> clock does the frequency of remote cursor loads actually account for?**

Phase 3A moved *where* the cursors live. Phase 3B holds placement fixed at the
layout Phase 3A verified, and moves only *how often* each thread reads the
other's cursor.

**Phase 3B changes ONE implementation treatment: the frequency of remote cursor
loads.** As in Phase 3A, "one variable" means the two implementations differ in
exactly one intentional treatment. It does **not** mean the two legs' processes
are identical in every respect: each leg runs as an independent process with its
own independently allocated queue object, so absolute addresses, scheduler
placement, DVFS and thermal state, and background-system state all differ and
are **not** eliminated by construction. Those nuisance variables are handled by
the design — adjacent process pairing, balanced AB/BA ordering, four repeated
sessions — and by the four-session directional-stability criterion (§3.3,
§4.4), not by construction.

There is no batching, no changed memory ordering, no CAS, no MPSC/MPMC, no
affinity or NUMA tuning, and no tail-latency instrumentation anywhere in this
phase. Cursor placement is **not** combined with the caching treatment, because
an experiment that moves two things at once attributes the result to neither.
The Phase-3A same-line control is not part of this comparison at all.

**Status: Phase 3B — Remote Cursor Caching: COMPLETE / FROZEN.** The balanced
canonical dataset was collected 2026-09-12 and verified against every invariant
in §3.7 (72 canonical processes, 360 measured repetitions, 18 separate
mechanism processes, `correctness=PASS` and both layout verdicts `PASS` on every
row, equal `object_size` and `payload_offset` across the two variants in all 9
cells, no instrumentation leak). Canonical dataset:
`docs/results/spsc-remote-cursor/`.

**Headline result: the mechanism worked and the performance did not follow.**
Remote cursor loads fell by up to ~44,910× on one side of the transfer, while
the cached variant was **slower** in six of nine cells — stably, across all four
balanced sessions, by 1.09×–2.00× — with the remaining three cells inconclusive.
See §4.3, §4.6 and §5 question 8.

**The frozen code stays frozen.** `include/spsc_ring_buffer.h` (the unpadded
Phase-1/2 implementation) and `include/spsc_cursor_layout_ring_buffer.h` (the
Phase-3A controls) were **not** modified. The Phase-3B type includes the
Phase-3A header read-only, for its evidence type. Every Phase-1, Phase-2 and
Phase-3A canonical result file is untouched, and Phase 3A was not rerun.

---

## 1. WHY — the question Phase 3B is allowed to ask

### 1.1 Research question

*Holding the algorithm, the payload, the payload's position in the object, the
cursor cache-line placement, the capacity, the slot indexing, the publication
protocol, the message types, the memory orders, the retry/yield harness and the
machine constant, how much does throughput change when each thread replaces its
per-operation acquire load of the opposite thread's cursor with a thread-owned
cached copy that is refreshed only when the cached value says the operation
might not succeed?*

That is the whole question. Phase 3B does not ask "how fast is a padded queue"
(Phase 3A), does not ask "what is the best SPSC", and does not ask whether
caching helps *in general*. It asks what happens to wall clock when one specific
class of load — the remote cursor observation — is made less frequent while
everything else, including the synchronization that makes the algorithm correct,
stays exactly where it was.

### 1.2 What remote cursor caching is

The frozen protocol has each thread publish its own cursor and read the other's:

| thread | writes (release) | reads (acquire) |
|---|---|---|
| producer | `head` | `tail` |
| consumer | `tail` | `head` |

The acquire load of the remote cursor is what makes the algorithm correct
(§2.3). It is also, in principle, the expensive part: the remote cursor is a
cache line that the other thread keeps writing, so reading it can require
coherence work. A thread that reads it on *every* operation pays that cost even
when the answer cannot have changed in a way that matters.

Caching makes that read *conditional*. The producer keeps a private
`cached_tail`, and only consults the real `tail` when `cached_tail` says the
queue **may** be full. The consumer keeps a private `cached_head`, and only
consults the real `head` when `cached_head` says the queue **may** be empty.

The two cached values are **ordinary, non-atomic, thread-owned members**. They
are not shared, they are not synchronized, and nothing about them is atomic.
`cached_tail` is written and read only by the producer; `cached_head` only by
the consumer. The compiler is free to keep them in registers; the hardware is
free to keep them in the local cache with no coherence obligation at all.

### 1.3 The release/acquire edge is NOT removed, and must not be described as removed

This is the single most important thing to get right about Phase 3B, and it is
easy to get wrong in a summary.

**The cached variant removes no release/acquire pair from the protocol.** Both
variants publish with a release store and observe with an acquire load:

* the producer still publishes the payload with
  `head.store(h + 1, std::memory_order_release)`;
* the consumer still reads the payload only after an **acquire** observation of
  `head` — in the cached variant that observation is
  `cached_head = head_.load(std::memory_order_acquire)`, executed less often but
  with its ordering guarantees fully intact;
* the producer's `cached_tail = tail_.load(std::memory_order_acquire)` is still
  what orders the payload write to a recycled slot after the consumer's finished
  read of that slot.

What changes is the **frequency** of the acquire loads, not their existence,
their order, or their strength. No memory order was relaxed; if anything the
cached variant performs *fewer* synchronization operations but each one is
exactly as strong as before. A summary that says "Phase 3B removed the acquire
load" or "Phase 3B replaced synchronization with a cache" is **wrong**, and no
result in this document supports it.

### 1.4 What Phase 3B deliberately does not claim

* It does not claim that fewer remote loads **must** produce a wall-clock
  improvement. It measures whether they did, per cell (§5, question 4).
* It does not claim hardware coherence behaviour, cache misses, or invalidation
  counts. There are no hardware performance counters in this experiment. The
  counters here count **remote cursor loads performed by the program**, which is
  a software-visible quantity (§2.9, §6.1).
* It does not claim a per-call latency. Every throughput figure is end-to-end
  elapsed time divided by messages delivered (§3.1).
* It does not claim that the cached variant is generally better, or that it
  should be adopted. It reports where the mechanism reduces loads, where it does
  not, and where the wall clock moved — including the cases where those three
  answers disagree.

### 1.5 Evidence labels used in this document

Following the discipline established in Phase 3A:

* **MEASURED PERFORMANCE** — wall-clock `ns_per_message` from the canonical,
  uninstrumented (`--instrument=0`) runs.
* **MEASURED MECHANISM** — counts of remote cursor loads, from the separate
  `--instrument=1` leg. This is a *software* count of loads performed, and
  nothing more.
* **INTERPRETATION** — what the author reads the two together as meaning, always
  explicitly labelled and never presented as measurement.

These are never blended. In particular, a throughput number and a load count
from the same cell come from **different processes**, and no statement in this
document treats them as having been measured together.

---

## 2. WHAT — the treatment, the layout, the counters

### 2.1 The two variants

| dataset name | meaning |
|---|---|
| `baseline` | the Phase-3A **separated** algorithm, unchanged: one acquire load of the remote cursor per `try_push` / `try_pop` |
| `cached` | the same algorithm plus a thread-owned cached copy of the remote cursor, refreshed only when the cached value says the operation may fail |

`baseline` is not a new implementation. It is the Phase-3A separated algorithm
re-instantiated through the Phase-3B template, so that the two variants can
share one algorithm body (§2.2).

### 2.2 One algorithm body, so the "only one variable" claim is structural

The two variants are **one template** parameterised by a compile-time mode, not
two copy-pasted implementations:

```cpp
template <typename T, std::size_t Capacity, RemoteCursorMode Mode, bool Instrumented>
class RemoteCursorRingBuffer;
```

`try_push` and `try_pop` are each written **once**, with the cached/uncached
decision made by `if constexpr (Mode == RemoteCursorMode::Cached)`. Two separate
hand-written implementations could drift; a single body with a compile-time
branch cannot. This is the same technique Phase 3A used, for the same reason.

Both variants use the **same cursor policy type**
(`SeparatedRemoteCursorBlock`). So "identical object footprint, identical
payload offset, identical cursor cache-line placement" is not an assertion about
two similar types — it is *the same type*, and it cannot differ. The
compile-time `static_assert`s on the block's size, alignment and member offsets
(§2.4) are therefore satisfied by both variants simultaneously.

### 2.3 The correctness argument — why a stale cached value is safe

**The cached copies can only ever be BEHIND reality, never ahead of it.**
Both cursors are monotonically increasing `std::size_t` counters (§2.6), so:

* `cached_tail` holds a value the real `tail` had at some earlier moment, and
  `tail` only increases, so **`cached_tail <= tail`** always holds.
* `cached_head` holds a value the real `head` had at some earlier moment, so
  **`cached_head <= head`** always holds.

The two failure modes that follow are both **conservative**:

| stale value | what the thread wrongly concludes | consequence |
|---|---|---|
| `cached_tail < tail` | "the queue may be full" (FALSE FULL) | producer refreshes and may refuse a push that would in fact have fit |
| `cached_head < head` | "the queue may be empty" (FALSE EMPTY) | consumer refreshes and may refuse a pop that would in fact have succeeded |

Neither can cause a slot to be reused before the consumer has released it, and
neither can expose a payload the producer has not published. The producer's
abbreviated test `head - cached_tail == Capacity` can only *under*-estimate the
free space, never over-estimate it; the consumer's abbreviated test
`cached_head == tail` can only *under*-estimate the published count. A false
negative costs a refresh and a retry; a false positive is impossible.

**The two induction steps that make the abbreviated check exact.** The claim is
not merely "a stale cache is conservative" but "the cached variant accepts
exactly the pushes/pops the baseline accepts, up to refresh timing":

1. *No slot is written before it is free.* The producer writes
   `slots_[head & (Capacity-1)]` only after establishing
   `head - cached_tail < Capacity`, hence `head - tail < Capacity` (since
   `cached_tail <= tail`). The slot being written is therefore at least
   `Capacity` positions behind `head`, which is precisely the slot the consumer
   has already finished reading. The acquire load that produced `cached_tail` is
   what makes that "already finished" ordering observable, and it is still
   present (§1.3).
2. *No payload is read before it is published.* The consumer reads
   `slots_[tail & (Capacity-1)]` only after establishing `cached_head != tail`,
   hence `head != tail` (since `cached_head <= head`, a strict inequality
   against a stale value implies the real `head` has moved past `tail`). The
   acquire load that produced `cached_head` is the edge that orders the
   producer's payload write before this read.

**On counter wrap.** `head == tail` means empty and `head - tail == Capacity`
means full, so the actual number of messages in flight must never reach
`2^N` for an N-bit counter. Phase 1 already established that the counters are
unsigned and that the difference arithmetic is correct across wrap; Phase 3B
preserves those assumptions unchanged, and adds no signed arithmetic. The tests
cover physical ring wrap-around specifically (§3.4).

**Construction.** Both cached values are initialised to `0`, matching the
initial `head` and `tail`. Construction performs no remote acquire at all: there
is nothing to observe yet, and adding one would be an unmeasured cost in the
constructor rather than in the timed region.

### 2.4 Cursor placement stays the verified separated layout

Phase 3B does **not** change cursor placement. `head` keeps offset `0` and
`tail` keeps offset `kAssumedCacheLineSize` — the exact offsets Phase 3A
verified — so that Phase 3A's result and Phase 3B's result are about different
variables rather than about the same one twice.

The layout is Phase 3A's separated layout, with the cached remote cursors and
the instrumentation counters added to the block of the thread that owns them:

```
line 0  | head (producer writes, consumer reads) | cached_tail (producer only) | producer_remote_tail_loads (producer only)
line 1  | tail (consumer writes, producer reads) | cached_head (consumer only) | consumer_remote_head_loads (consumer only)
--------| payload
```

Ownership, which the runtime check verifies on the object that ran:

| member | owner | atomic? |
|---|---|---|
| `head` | producer writes, consumer reads | yes |
| `cached_tail` | producer only | **no** |
| `producer_remote_tail_loads` | producer only | **no** |
| `tail` | consumer writes, producer reads | yes |
| `cached_head` | consumer only | **no** |
| `consumer_remote_head_loads` | consumer only | **no** |

### 2.5 The cached values must not introduce a NEW cross-thread false-sharing relationship

This is the constraint that determines where the cached values live, and it is
the reason they are *not* simply placed next to the cursors they mirror.

A cache line is a coherence unit. Phase 3A established a layout in which no line
is written by both threads. If Phase 3B put `cached_tail` (producer-written) on
the `tail` line (consumer-written), it would create exactly the cross-thread
write sharing Phase 3A removed — the experiment would be introducing the very
interference it is trying to measure the absence of.

So each cached value is stored **with the state its own owner already owns**:

* `cached_tail` sits on **line 0**, alongside `head`. Line 0 is already
  producer-written and consumer-read; adding producer-private storage to it
  creates no new sharing, because the consumer never writes that line.
* `cached_head` sits on **line 1**, alongside `tail`, by the same argument
  mirrored.

The result is that **no cache line becomes written by both threads that was not
already**, and the two cached values are on *different* lines from each other
(one is producer-private, one consumer-private — putting them together would
create a new line both threads write). The runtime report verifies all of this
per repetition, on the addresses actually used (§2.7), rather than asserting it
from the type.

### 2.6 Equal footprint, unchanged

Phase 3A established that the two cursor policies must have the same size
(`2 * kAssumedCacheLineSize` = 256 bytes on the canonical host) so that the
payload keeps the same relative offset and `payload_offset` is not confounded
with cursor placement.

Phase 3B inherits that property for free and for a stronger reason: **both
variants are instantiated over the same policy type**, so their `object_size`
and `payload_offset_from_object_base` cannot differ. The benchmark nevertheless
checks it, because "cannot differ" is a claim about the code and the dataset
should carry evidence about the objects: before timing anything, the cell's
dispatch path instantiates **both** variants and compares their
`object_size` and `payload_offset_from_object_base`, failing the cell if they
disagree. The canonical runner re-derives the same comparison from the published
raw rows of the two independently allocated processes (§3.7).

### 2.7 Runtime verification — the evidence the claims rest on

Every measured repetition constructs a **fresh queue object** and reports, from
that object, outside the timed interval:

* the object base address and size, and the payload's offset and begin address;
* the `head` and `tail` addresses, and their **line indices** under the host's
  reported cache-line size;
* whether the cursors share a line, and whether either shares the payload's
  line;
* the `cached_tail` and `cached_head` addresses and line indices;
* a `layout_ok` verdict for the **Phase-3A separated** claim, and a
  `cached_placement_ok` verdict for the **Phase-3B** placement claim.

The Phase-3B verdict requires all three of: `cached_tail` colocated with its
owner's line, `cached_head` colocated with its owner's line, and the cached
state disjoint from the remote cursor it mirrors. The benchmark **fails the
cell rather than publishing** if any of these does not hold, and the runner
re-checks every recorded row independently (§3.7).

The host's reported cache-line size is queried at runtime and checked against the
compile-time assumption before anything is timed; a host reporting a line larger
than the assumption is refused rather than silently mis-measured. On the
canonical host the reported line is **128 bytes**, not the 64 that most code
assumes.

### 2.8 Matrix and reported quantities

Nine cells: `message_bytes` ∈ {8, 32, 64} × `capacity` ∈ {1024, 4096, 65536}.
All nine are reported. No cell is dropped, and no repetition is dropped: one
warm-up repetition per process is excluded, and every measured repetition is
kept.

Reported per cell, per session:

* `med_ns_per_message` — median over the measured repetitions of one process,
  end-to-end elapsed / messages delivered;
* `producer_full_retries`, `consumer_empty_retries` — harness backpressure
  counters (§3.1);
* the layout and cached-placement evidence of §2.7;
* in the separate mechanism leg only: `producer_remote_tail_loads`,
  `consumer_remote_head_loads` and their per-message rates.

### 2.9 The instrumentation counters

The counters count **remote cursor loads actually performed by the program**. In
the `baseline` they are, by construction, the `try_push`/`try_pop` call counts,
and are reported for verification only. In the `cached` variant they count real
refresh loads — the mechanism the experiment is about.

They are **ordinary, non-atomic, thread-owned members**, incremented on the hot
path by their owner and read by the harness only **after both threads have been
joined**. No global atomic was added to the timed hot path to count these
operations: doing so would inject exactly the cross-thread traffic the
experiment is trying to vary, i.e. the instrument would become the experiment.

In the canonical (uninstrumented) instantiation the counting is
**compile-time-eliminated** — `if constexpr (Instrumented)` means no increment
is emitted at all. The counter *storage* stays in the type so that the object
layout is identical between the two instantiations, and an explicit leak guard
fails the run if a canonical process reports a non-zero counter, which is how an
accidental instrumentation leak is caught rather than published.

---

## 3. HOW — the measurement procedure

### 3.1 The harness is the Phase-2/3A harness

The timed region, thread structure, checksum verification, retry/yield policy and
message types are the frozen Phase-2 harness, unchanged. In particular:

* **One implementation per process.** The other variant is not instantiated in
  the timed binary path, is never timed in the same interval, and runs in a
  separate process.
* **The retry/yield policy is exactly the hardened Phase-2 policy**: a thread
  retries immediately, and yields only after **1024 consecutive failed
  attempts**. This is identical in both variants — it is not part of the
  treatment — and the same `producer_full_retries` / `consumer_empty_retries`
  counters are preserved.
* **`ns_per_message` is end-to-end elapsed / messages delivered.** It includes
  queue synchronization, payload assignment, coherence traffic, harness
  retry/backpressure and OS scheduling. It is **not** a per-`try_push` latency,
  **not** a per-`try_pop` latency, and **not** a one-way handoff time.
* Thread creation, queue allocation, the expected-checksum pre-pass and all
  address/layout reporting happen **outside** the timed interval; the consumer
  records the end timestamp itself.
* Every repetition constructs a **fresh** producer/consumer thread pair and a
  fresh queue object.

### 3.2 Performance mode vs mechanism mode

Two separate measurements, never blended:

| mode | flag | counters | used for |
|---|---|---|---|
| performance (canonical) | `--instrument=0` | compile-time-eliminated | **the canonical throughput result** |
| mechanism | `--instrument=1` | live | the remote-load counts only |

The canonical throughput numbers come **only** from `--instrument=0` processes.
The instrumented leg is a **different instantiation**, and this document does not
claim that a throughput number measured under materially different
instrumentation is the clean canonical result — so the instrumented leg's
`ns_per_message` is recorded for completeness and is **never** quoted as the
Phase-3B throughput figure. The two legs are written to different directories
(`raw/` and `mechanism/`) and labelled differently everywhere they appear.

### 3.3 Balanced AB/BA run design

On an unpinned macOS host, a treatment difference and a time-of-run difference
are otherwise indistinguishable. The two variants of a given
`(message_bytes, capacity)` therefore run as **adjacent processes**, with both
the variant order and the traversal direction balanced across four sessions:

| session | traversal | variant order |
|---|---|---|
| 1 | forward | baseline → cached |
| 2 | reverse | cached → baseline |
| 3 | forward | cached → baseline |
| 4 | reverse | baseline → cached |

Every cell therefore receives **2 baseline-first and 2 cached-first**
comparisons. The order is fixed and deterministic, recorded verbatim in
`command.txt` as it runs, and then **verified by parsing that record back** —
the balance is a checked property of the run, not an intention.

The primary reported statistic is the **median paired ratio**, one ratio per
session (cached session median ÷ baseline session median), with the four
session ratios, their median, min and max, and the 4/4 directional-stability
count. `stable` requires **all four** sessions to agree; a cell whose ratios do
not all fall on the same side of 1 is reported as **SPLIT** and supports **no
directional claim**, whatever its median says. This is a descriptive criterion.
Four paired observations per cell cannot establish formal statistical
significance, and no p-value is implied anywhere in this document.

### 3.4 Correctness gating

A cell that fails any check is **not published**: the benchmark exits non-zero
and the runner stops. Covered by the test suite, run once per variant:

* empty queue; one push/pop; fill to exactly `Capacity`; full detection; full
  drain; repeated physical ring wrap-around; rapid slot reuse;
* a deterministic large FIFO sequence with a structured multi-field payload,
  verifying no duplicates, no missing messages and no reordering;
* differential testing against the Phase-3A separated baseline;
* the counter-wrap arithmetic the cached full/empty tests depend on, with
  targeted tests around physical ring wrap (testing the logical properties;
  there is no need to execute `2^N` logical operations);
* the refresh **mechanism** itself, as a set of logical properties rather than a
  measured ratio — the tests assert that a refresh happens when the cached value
  says it must and that the cached value never exceeds the real cursor. No
  fragile performance-dependent ratio is hard-coded;
* ASan/UBSan and TSan, which are **not** used for any performance figure.

### 3.5 Sanitizers

ASan/UBSan and TSan runs are correctness evidence only. TSan's scheduler
radically changes the producer/consumer lag split, so per-side margins under TSan
are not assertable and no timing is taken from a sanitizer build. Canonical
numbers come only from an optimized, non-sanitized Release build.

### 3.6 Provenance

The dataset records the git revision, the full `git status --porcelain`, the
SHA-256 of every relevant source file and of the benchmark executable, the host
and toolchain metadata, the host-reported cache-line size, the exact build
commands, and every effective invocation in execution order. A committed, clean
tree is preferred, and the run that produced the canonical dataset was made from
one.

### 3.7 Dataset invariants

The runner writes its summaries only after re-deriving each process summary from
its own raw repetition CSV, and only if every invariant holds. The invariants
include: the recorded invocation count; that **no canonical invocation was
instrumented**; that the raw files on disk are exactly the set the recorded
execution order implies; that every row reports `measurement_mode=performance`,
`instrumented=0` and **zero** remote-load counters (the leak check); that the
separated-layout and cached-placement verdicts pass on **every** row and match
the addresses recorded on that row; that the two variants of a pair agree on
`object_size` and `payload_offset`; and that the AB/BA pairing and balance hold
as recorded.

---

## 4. MEASURED — the canonical dataset

### 4.1 Host, build and provenance

Apple M3 Max (`Mac15,10`), macOS 14.2.1, Apple clang 15.0.0, arm64, clean
Release build of `spsc_remote_cursor_bench` with no architecture-specific flags
(`BENCH_ARCH_FLAGS` empty), one implementation per process. The host reported a
**128-byte** cache line, matching the compile-time assumption, so the
pre-timing host guard did not fire.

The five compiled inputs were committed at **`6ff0b57`** and unmodified for the
duration of the run; the recorded `git status --porcelain` lists only this
document and the dataset directory itself, neither of which is compiled into the
benchmark. Git revision, source hashes, the benchmark binary's SHA-256, the exact
build commands and every effective invocation in execution order are in
`docs/results/spsc-remote-cursor/PROVENANCE.md` and `command.txt`.

### 4.2 The treatment and the layout, as verified

The dataset is 72 processes (9 cells × 2 variants × 4 sessions), **360 measured
repetitions** (one warm-up repetition per process excluded), plus a separate
18-process instrumented mechanism leg. All 360 canonical rows report
`correctness=PASS`, and every row was independently re-derived from its own raw
CSV by the runner before any summary was written.

The layout evidence, re-checked on the object each repetition actually timed —
and re-checked a second time by the runner over the published rows:

| check | result |
|---|---|
| Phase-3A separated layout (`layout_ok`) | **PASS on all 360 rows** |
| `head_line != tail_line` under the reported 128-byte line | **all 360 rows** |
| Phase-3B cached placement (`cached_placement_ok`) | **PASS on all 360 rows** |
| `cached_tail` on the producer's `head` line | all 360 rows |
| `cached_head` on the consumer's `tail` line | all 360 rows |
| cached values on the **remote** cursor's line | **never** |
| the two cached values on the same line as each other | **never** |
| canonical rows reporting a non-zero remote-load counter | **none** (leak check PASS) |
| `object_size` / `payload_offset` equal across the two variants of a pair | **all 9 cells** |

Measured footprint, identical for both variants in every cell:
`object_size = 256 + message_bytes × capacity`, `payload_offset_from_object_base
= 256`. The two cached values therefore added **no** new cross-thread
false-sharing relationship: they sit on lines that were already single-writer,
and on different lines from each other (§2.5).

### 4.3 Primary result — paired per-session ratios

`ratio` = **cached** session median ÷ **baseline** session median, computed on
**adjacent** processes within one session under the balanced AB/BA order.
**`ratio > 1` means the cached variant was slower.** This paired view is primary;
§4.5 is secondary.

| bytes | capacity | s1 (first) | s2 (first) | s3 (first) | s4 (first) | median |
|---|---|---|---|---|---|---|
| 8 | 1024 | 1.062 (base) | 1.113 (cached) | 1.287 (cached) | 1.061 (base) | **1.087** |
| 8 | 4096 | 2.325 (base) | 2.088 (cached) | 1.922 (cached) | 1.045 (base) | **2.005** |
| 8 | 65536 | 1.080 (base) | 1.187 (cached) | 1.026 (cached) | 1.126 (base) | **1.103** |
| 32 | 1024 | 0.902 (base) | 1.134 (cached) | 1.129 (cached) | 1.085 (base) | 1.107 |
| 32 | 4096 | 0.988 (base) | 1.117 (cached) | 1.066 (cached) | 1.049 (base) | 1.057 |
| 32 | 65536 | 1.106 (base) | 1.084 (cached) | 0.974 (cached) | 0.982 (base) | 1.033 |
| 64 | 1024 | 1.225 (base) | 1.268 (cached) | 1.297 (cached) | 1.272 (base) | **1.270** |
| 64 | 4096 | 1.360 (base) | 1.425 (cached) | 1.387 (cached) | 1.449 (base) | **1.406** |
| 64 | 65536 | 1.547 (base) | 1.618 (cached) | 1.591 (cached) | 1.518 (base) | **1.569** |

`(base)` / `(cached)` is which variant ran **first** in that session's pair. Bold
marks the six cells whose four ratios all fall on the same side of 1.

**The direction is uniform where it is stable, and it is not the direction the
optimization was intended to produce: the cached variant is slower.** Across the
36 paired observations, **32 favour the baseline and 4 favour the cached
variant**; every directionally stable cell is baseline-faster, and no cell is
stably cached-faster.

### 4.4 Direction stability across the balanced sessions

| bytes | capacity | median ratio | min | max | cached faster | baseline faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 1.087 | 1.061 | 1.287 | 0 | 4 | **stable — baseline 4/4** |
| 8 | 4096 | 2.005 | 1.045 | 2.325 | 0 | 4 | **stable — baseline 4/4** |
| 8 | 65536 | 1.103 | 1.026 | 1.187 | 0 | 4 | **stable — baseline 4/4** |
| 32 | 1024 | 1.107 | 0.902 | 1.134 | 1 | 3 | SPLIT 1–3 — no directional claim |
| 32 | 4096 | 1.057 | 0.988 | 1.117 | 1 | 3 | SPLIT 1–3 — no directional claim |
| 32 | 65536 | 1.033 | 0.974 | 1.106 | 2 | 2 | SPLIT 2–2 — no directional claim |
| 64 | 1024 | 1.270 | 1.225 | 1.297 | 0 | 4 | **stable — baseline 4/4** |
| 64 | 4096 | 1.406 | 1.360 | 1.449 | 0 | 4 | **stable — baseline 4/4** |
| 64 | 65536 | 1.569 | 1.518 | 1.618 | 0 | 4 | **stable — baseline 4/4** |

**Six cells are directionally stable — all six baseline-faster — and three are
split and support no directional claim.** MIN/MAX are the extremes of four
correlated observations, not a confidence interval, and `stable` here means
"4 out of 4 agreed", not "proven".

Two features of this table are worth separating:

* **The three split cells are all at 32 B.** Their ratios cluster tightly around
  1 (0.90–1.13), i.e. the split is not two wildly different populations but a
  small effect whose sign does not survive the AB/BA swap. That is reported as
  inconclusive, not as a median.
* **The magnitude is not uniform in message size.** The penalty grows with
  message size: medians of 1.06–2.00 at 8 B, 1.03–1.11 at 32 B, and 1.27–1.57 at
  64 B. The largest single median is **2.005 at 8 B / 4096**.

### 4.5 The pooled matrix, as a secondary view

`MATRIX.md` pools the 5 measured repetitions within each process. Pooled median
`ns/msg`, baseline vs cached (capacities 1024 / 4096 / 65536):

* 8 B: 42.67 / 31.82 / 58.66 vs 48.11 / 54.11 / 64.72
* 32 B: 46.90 / 58.49 / 71.26 vs 49.39 / 61.60 / 73.79
* 64 B: 53.66 / 48.68 / 43.45 vs 67.89 / 68.39 / 68.11

Pooling treats repetitions inside one process as independent placements, which
they are not (§7.3), so these describe the dataset rather than support a
directional claim. They agree with §4.3 in sign in all nine cells — the pooled
ratios run from 1.036 to 1.700, all above 1 — including the three cells where the
paired view is split, where pooling shrinks the difference toward the middle of
the per-session spread.

### 4.6 MEASURED MECHANISM — remote cursor loads

From the **separate** instrumented leg (`mechanism/`), reported as loads **per
message**. These are software counts of loads the program executed; they are
**not** hardware cache-miss or coherence counters, and nothing here is described
in those terms (§6.1).

| bytes | capacity | side | baseline loads/msg | cached loads/msg | change |
|---|---|---|---|---|---|
| 8 | 1024 | producer | 1.545 | 0.089 | **17.4× fewer** |
| 8 | 1024 | consumer | 3.234 | 6.150 | 1.9× **more** |
| 8 | 4096 | producer | 1.028 | 0.018 | **57.3× fewer** |
| 8 | 4096 | consumer | 3.012 | 7.426 | 2.5× **more** |
| 8 | 65536 | producer | 1.066 | 0.0000152 | **70,102× fewer** |
| 8 | 65536 | consumer | 2.767 | 11.414 | 4.1× **more** |
| 32 | 1024 | producer | 1.008 | 0.341 | **3.0× fewer** |
| 32 | 1024 | consumer | 10.714 | 7.765 | 1.4× fewer |
| 32 | 4096 | producer | 1.001 | 0.0079 | **126.6× fewer** |
| 32 | 4096 | consumer | 12.140 | 10.244 | 1.2× fewer |
| 32 | 65536 | producer | 1.000 | 0.0000152 | **65,805× fewer** |
| 32 | 65536 | consumer | 14.821 | 15.988 | 1.1× **more** |
| 64 | 1024 | producer | 2.319 | 6.241 | 2.7× **more** |
| 64 | 1024 | consumer | 1.247 | 0.00108 | **1,156× fewer** |
| 64 | 4096 | producer | 1.997 | 6.155 | 3.1× **more** |
| 64 | 4096 | consumer | 1.190 | 0.000277 | **4,292× fewer** |
| 64 | 65536 | producer | 2.027 | 6.028 | 3.0× **more** |
| 64 | 65536 | consumer | 1.000 | 0.0000223 | **44,910× fewer** |

**The remote-load reduction is directly verified, and it is real — but it is
one-sided, and which side it lands on depends on message size.** At 8 B and
32 B the *producer* is the side whose loads collapse (up to ~70,000×) while the
*consumer* performs up to 4.1× **more** loads. At 64 B the pattern inverts: the
*consumer*'s loads collapse (up to ~44,910×) while the *producer* performs ~3×
**more**.

The mechanism is therefore not "fewer remote loads"; it is **fewer remote loads
on one side, more on the other**. A summary that reports only the collapsing
side would misdescribe the result.

**A caveat that limits how these counts may be combined with §4.3.** The counts
are end-to-end totals for a whole run, so a variant that takes longer to move
the same number of messages also gets more opportunities to reach the failure
path and load again. Part of the *increase* on the losing side is therefore a
consequence of the slowdown rather than a cause of it, and these counters cannot
separate the two. Additionally, the instrumented leg is a *different
instantiation* from the canonical one (§3.2); its own `ns/msg` and retry rates
differ from the canonical leg's, so the load counts and the §4.3 throughput
ratios come from **separate runs in generally-similar but not identical
regimes**. No "loads saved per nanosecond" arithmetic is performed anywhere in
this document, and none is supported by this dataset.

### 4.7 Backpressure and the producer/consumer balance

Per-message harness retry counts, canonical leg, summed over the four sessions
(these are backpressure counters, not mechanism counters — §7.4):

| bytes | capacity | variant | producer full retries/msg | consumer empty retries/msg |
|---|---|---|---|---|
| 8 | 1024 | baseline | 0.09 | 6.98 |
| 8 | 1024 | cached | 0.26 | 5.76 |
| 8 | 4096 | baseline | 0.03 | 5.27 |
| 8 | 4096 | cached | 0.00 | 7.22 |
| 8 | 65536 | baseline | 0.00 | 13.97 |
| 8 | 65536 | cached | 0.07 | 10.89 |
| 32 | 1024 | baseline | 0.06 | 8.41 |
| 32 | 1024 | cached | 0.21 | 8.78 |
| 32 | 4096 | baseline | 0.01 | 15.48 |
| 32 | 4096 | cached | 0.01 | 11.35 |
| 32 | 65536 | baseline | 0.00 | 20.53 |
| 32 | 65536 | cached | 0.00 | 17.39 |
| 64 | 1024 | baseline | 5.05 | 1.04 |
| 64 | 1024 | cached | 5.99 | 0.00 |
| 64 | 4096 | baseline | 4.25 | 0.00 |
| 64 | 4096 | cached | 6.66 | 0.00 |
| 64 | 65536 | baseline | 2.94 | 0.00 |
| 64 | 65536 | cached | 6.84 | 0.00 |

The side that waits **flips with message size, in both variants**: at 8 B and
32 B the consumer is the pacer (5–20 empty retries per message against ≈0 full
retries), while at 64 B the producer is the pacer (3–7 full retries per message
against ≈0 empty retries). Caching did not create that asymmetry — it is present
in the baseline too — but at 64 B the cached variant **shifts it further toward
the producer**, raising producer full retries from 5.05 → 5.99, 4.25 → 6.66 and
2.94 → 6.84.

---

## 5. DERIVED — the Phase-3B questions answered

**1. How much does remote cursor caching reduce actual remote cursor
refreshes?**
On one side, enormously: up to **~70,000×** fewer producer loads (8 B / 65536)
and up to **~44,910×** fewer consumer loads (64 B / 65536). On the other side it
**increases** them, by up to **4.1×** (consumer, 8 B / 65536) and **~3×**
(producer, 64 B). It is not a uniform reduction in remote loads; it is a
relocation of them (§4.6).

**2. Does lower remote-load frequency translate into higher end-to-end
throughput?**
**No.** Every directionally stable cell is **baseline-faster**, with median
ratios from 1.087 to 2.005. No cell in this dataset is stably cached-faster. The
one-sided load reduction of question 1 produced no throughput gain anywhere it
was stable, and the largest penalties appear in cells where the load reduction
was largest (§5, question 8).

**3. Is the effect stable across message sizes?**
**No — message size is the discriminating axis.** All three 8 B cells and all
three 64 B cells are stable, but the penalty grows with size (8 B: 1.087–2.005;
32 B: 1.033–1.107, all split; 64 B: 1.270–1.569). The *mechanism* also inverts
with size: at 8 B/32 B the producer's loads collapse and the consumer's rise; at
64 B the reverse.

**4. Is the effect stable across capacities?**
**Within a message size, yes — capacity is not the discriminating axis.** At 8 B
and at 64 B all three capacities give the same stable verdict (baseline-faster);
at 32 B all three are split. Capacity changes the magnitude but not the sign.

**5. Which cells remain inconclusive?**
**32 B / 1024, 32 B / 4096 and 32 B / 65536.** All three are SPLIT (1–3, 1–3,
2–2) with ratios tightly clustered around 1 (0.902–1.134), so the direction does
not survive the AB/BA swap and no directional claim is made. Every other cell is
directionally stable.

**6. How do producer-full and consumer-empty retry patterns change?**
The pacer flips with message size in both variants (§4.7). At 8 B and 32 B the
consumer's empty retries dominate (5–20 per message) and the cached variant
changes them only modestly (e.g. 13.97 → 10.89 at 8 B / 65536). At 64 B the
producer is the pacer and the cached variant **raises** its full retries in all
three cells (5.05 → 5.99, 4.25 → 6.66, 2.94 → 6.84) — the same cells with the
largest throughput penalty. These counters cannot distinguish cause from effect
(§7.4), and no causal claim is made from them.

**7. Does remote cursor caching alter the producer/consumer rate balance?**
It does not create the balance — the flip between consumer-paced (8 B/32 B) and
producer-paced (64 B) is present in the **baseline** as well. Within 64 B the
cached variant shifts the balance further toward the producer by increasing its
full retries roughly 1.2–2.3×. Elsewhere the balance is broadly preserved.

**8. Are there cells where remote loads fall dramatically but throughput does
not improve?**
**Yes — this is the headline result, and it occurs in almost every cell.**
The clearest cases are both directionally stable:

* **64 B / 65536**: consumer remote loads fall **~44,910×** (1.000 → 0.0000223
  per message) and the producer's rise ~3×. Throughput is **1.569× worse**,
  stable 4/4.
* **8 B / 65536**: producer remote loads fall **~70,102×** (1.066 → 0.0000152
  per message) and the consumer's rise 4.1×. Throughput is **1.103× worse**,
  stable 4/4.
* **8 B / 4096**: producer loads fall **57×**; throughput is **2.005× worse** —
  the single largest penalty in the dataset, in a cell with a large one-sided
  load reduction.

A reduction of four to five orders of magnitude in one side's remote cursor
loads coexists with a stable throughput **regression**. The mechanism was
achieved; the wall-clock benefit was not. This is exactly the outcome §6.2
anticipated, and it is the reason no result in this document treats mechanism
reduction as evidence of performance improvement.

---

## 6. INTERPRETATION

### 6.1 What a lower remote-load count does and does not license

A reduction in remote cursor loads is a **software** measurement: it counts
loads the program executed. It says nothing directly about cache misses,
coherence transactions or invalidations, and this document never describes it in
those terms. Saying "remote loads fell by 1000×" is supported by the mechanism
leg; saying "cache misses fell" is not supported by anything here, because no
hardware counter was read.

### 6.2 A reduction in remote loads does not guarantee a wall-clock improvement

This is a real possible outcome, not a contradiction, and §5 question 8 looks
for it explicitly. There are several reasons it can happen:

* the remote loads that **remain** are the ones that were on the critical path
  anyway — the refresh occurs precisely when progress is blocked — while the
  elided ones may have been served from a line already held in a compatible
  coherence state;
* the cached variant adds work on the fast path: a thread-local read of its own
  cached value and an extra comparison, which are not free;
* when the queue is genuinely full or genuinely empty, the real remote cursor has
  **not moved**, so the cached value cannot improve and the thread refreshes on
  every failed attempt — performing *as many* remote loads as the baseline, plus
  the extra comparison. In a regime where one side spins on an empty queue for
  tens of millions of iterations, this is the dominant case.
* at macOS/Apple-Silicon scheduling granularity, a small coherence-traffic
  difference may be swamped by scheduler placement, DVFS and thermal variation.

### 6.3 Causal language discipline

Throughout this document:

* **Permitted:** "the cached variant performed N remote cursor loads against the
  baseline's M", "the paired median ratio was R", "the mechanism reduced
  program-visible remote loads without a corresponding wall-clock change".
* **Not permitted:** "cache misses fell", "coherence traffic dropped", "false
  sharing was reduced by the cache", "the acquire load was removed". None of
  these is measured here.

### 6.4 What this dataset does and does not establish

**Established.** Under this workload, on this host, with every other variable
held fixed by construction and the balance verified by the run record:

* the cached variant **does** reduce remote cursor loads, on one side, by up to
  four to five orders of magnitude, and this reduction was **directly measured**
  (§4.6);
* the cached variant is **slower** in six of nine cells, stably across all four
  balanced sessions, by 1.09× to 2.00×, and no cell is stably faster (§4.3);
* the three remaining cells are inconclusive, not neutral: their direction does
  not survive the AB/BA swap (§4.4).

**Not established, and not claimed.** That remote cursor caching is generally
harmful — this is one workload, one host, one message-size/capacity matrix, and
the effect is regime-dependent (§7.5). That the slowdown is *caused* by any
particular microarchitectural mechanism — the retry counters are backpressure,
not mechanism (§7.4), and no hardware counter was read (§6.1). That the
one-sided load reduction is the only change the cached variant makes — it also
adds a comparison and a branch on the fast path (§6.2), and the two are not
separated by this design.

**The valid one-sentence reading**, following the §6.3 discipline: *the cached
variant reduced explicit remote cursor observations on one side of the transfer,
by up to ~44,910× in the mechanism leg, and was associated with a stable
end-to-end throughput regression of 1.09×–2.00× in six of nine cells under this
workload.* Anything stronger — in particular any statement about cache misses or
coherence traffic — is not supported by this dataset.

---

## 7. LIMITATIONS

### 7.1 macOS scheduling: no affinity implemented or claimed

No CPU pinning, thread priority or affinity is set anywhere in this phase.
macOS may migrate either thread mid-run and may place the two processes on
different core types (the canonical host has performance and efficiency cores).
Adjacent pairing reduces temporal drift between the two legs but cannot
eliminate it.

### 7.2 The two variants are not the same shape of *machine* code

Both variants come from one source body, but they are distinct instantiations
with different instruction sequences and different code sizes. The claim that
they differ in exactly one treatment is a claim about the **algorithm and
object layout**, not about the emitted instruction stream: the cached variant
necessarily has more instructions on the fast path. This is inherent to the
treatment and is why §6.2 lists the extra comparison as a cost that is part of
the treatment, not a confound.

### 7.3 Pooled repetitions are correlated

The pooled `MATRIX.md` view treats the measured repetitions inside one process
as independent placements. They are not: a repetition creates a fresh thread
pair and a fresh queue object, but shares its process's address space, allocator
state and thermal history with its siblings. The pooled view is therefore
secondary and descriptive; the paired view of §3.3 is primary.

### 7.4 The retry counters are backpressure, not a mechanism

`producer_full_retries` and `consumer_empty_retries` count harness retries. They
cannot distinguish cause from effect — a faster side simply arrives first and
waits more. They are reported because they reveal **which regime** a process ran
in (§7.5), not as an explanation of any ratio.

### 7.5 The cell shape is strongly bimodal on the development host

This is the most important limitation in this document.

This cell shape has **two regimes** on the canonical host, and which one a run
lands in is selected by the *compiled binary's code placement*, not by the
variant being measured. The diagnostic evidence:

* the same source, compiled two ways, produced `producer_full_retries ≈ 22.8 k`
  / `consumer_empty_retries ≈ 42.8 M` in one build and `≈ 845 k` / `≈ 875 k` in
  the other — a ~50× swing in retry counts;
* the producer lambda extracted from a slow build and a fast build was
  **83 instructions, byte-identical**: the code did not change, its placement
  did;
* a fixed-work dependent-add probe read a flat 0.744–0.766 ns/iteration whether
  the benchmark was fast or slow, so the CPU was in the same speed regime in
  both cases;
* adding one unrelated template instantiation to the same translation unit moved
  the path from ~37 ns/message to ~16 ns/message.

Consequences, all of which shape how this dataset may be read:

1. **Both Phase-3B variants live in the same binary**, so the two processes of a
   pair share code placement and therefore share a regime. The comparison is
   internally valid even though the absolute level is regime-dependent.
2. **The balanced AB/BA design is the mitigation** for drift *between* sessions,
   and the four-session directional rule is what prevents a regime difference
   from being read as a treatment difference.
3. **The raw retry counts are preserved per repetition** so the regime of every
   measured process is visible in the dataset rather than averaged away.
4. **No Phase-3B result may be presented as resolving a difference smaller than
   the regime swing.** Where a cell's ratios are split across sessions, that is
   reported as SPLIT, not smoothed into a median.

**The absolute `ns_per_message` values in this dataset are not comparable to
Phase 3A's.** The Phase-3B benchmark binary lands in the slow regime on this
host (Phase 3A's separated 8 B / 1024 cell measured ≈ 15.6 ns/message from its
own binary; the Phase-3B `baseline` variant of the same cell measures several
times that), while both Phase-3B variants are measured inside the same regime.
The *comparison between the two variants* is the result; the *level* is a
property of the binary and this host.

### 7.6 Scope of the dataset

Nine cells, one machine, four sessions of one process each per cell per variant,
one warm-up repetition excluded per process. This is a small sample. `stable`
means "four out of four sessions agreed here", not "the effect is proven".

---

## 8. Phase status

Experiment 02

| phase | status |
|---|---|
| Phase 1 — Correctness / Memory Model | COMPLETE / FROZEN |
| Phase 2 — Throughput Baseline | COMPLETE / FROZEN |
| Phase 3A — Controlled Cursor Placement | COMPLETE / FROZEN |
| **Phase 3B — Remote Cursor Caching** | **COMPLETE / FROZEN** |
| Phase 4 — Tail Latency | NOT STARTED |

Phase 3B was marked complete only after: correctness passed; the layout
invariants passed on every measured row; the remote-load reduction was directly
verified; the balanced canonical dataset was collected; and the raw → summary
verification passed for every one of the 72 canonical and 18 mechanism
processes. Canonical dataset: `docs/results/spsc-remote-cursor/`.

**Result in one line.** Caching the remote cursor demonstrably reduced remote
cursor loads on one side of the transfer (up to ~44,910×) and produced no
throughput improvement in any directionally stable cell — it was **slower** in
six of nine cells, stably, by 1.09×–2.00×, with three cells inconclusive.

No frozen Phase-1, Phase-2 or Phase-3A result was modified, and Phase 3A was not
rerun. Phase 4 was not started.
