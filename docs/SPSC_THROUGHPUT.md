# Experiment 02 — SPSC Ring Buffer: Throughput Baseline (Phase 2)

**Scope.** This document is the *measurement* counterpart to
`docs/SPSC_MEMORY_MODEL.md`. Phase 1 answered *why* the SPSC cursor protocol is
correct and what its memory orders guarantee. Phase 2 asks how fast the frozen
Phase-1 implementation actually moves messages, measured against the
`MutexBoundedQueue` reference baseline under identical conditions.

**The measured code is the frozen Phase-1 code.** Nothing in this phase changes
the protocol: no cache-line padding, no separate cursor cache lines, no cached
remote cursors, no reduced atomic loads, no batching, and no memory-order
changes. Those are Phase 3 subjects and are deliberately absent here, which is
what makes this a *baseline*. See §7.2.

**Status: Phase 2 — Throughput Baseline: COMPLETE / FROZEN.**
Canonical dataset: `docs/results/spsc-throughput/` — a **balanced AB/BA
multi-session** run measured 2026-09-11 on the Apple M3 Max development host.

---

## 1. WHY — the question Phase 2 is allowed to ask

### 1.1 Research question

> How much steady-state end-to-end message-transfer throughput does the Phase-1
> SPSC protocol provide relative to a mutex-serialized bounded queue under
> controlled message sizes and capacities?

**The answer is not claimed in advance.** Phase 2 was designed, built and run
without assuming which implementation wins, and §4 reports the outcome the
machine actually produced.

### 1.2 What this measurement includes

`ns_per_message` here is **end-to-end elapsed time divided by messages
delivered**. That single number necessarily contains all of:

- **queue synchronization** — the acquire/release edges, the atomic cursor
  loads, and (for the baseline) the mutex lock/unlock and its contention;
- **payload assignment** — the `T` copy/move into and out of the slot on every
  message;
- **cache-coherence effects** — both threads touch the same queue object; the
  cursors and the ring storage are shared between cores, so coherence traffic
  is inside the number whether or not it is the dominant term;
- **harness retry / backpressure behaviour** — a `try_*` that fails is retried
  by the calling thread, and those retries are part of the measured interval
  (§3.2);
- **OS scheduling effects** — two runnable threads competing for cores, being
  placed, migrated, and frequency-scaled by macOS (§7.1).

### 1.3 What this measurement does NOT isolate

Phase 2 is **not** a microbenchmark of a single atomic instruction, and **not** a
per-call latency of `try_push` or `try_pop`. It cannot attribute the observed
time to any one of the components listed in §1.2, and in particular:

- It does **not** measure one-way handoff latency (the time from a specific
  `try_push` to the matching `try_pop`). Only the aggregate of N transfers is
  timed.
- It does **not** isolate false sharing. Phase 1's `head_`/`tail_` are adjacent
  and unpadded, so false sharing *permits and is likely to exhibit* itself — but
  Phase 2 has no packed/separated control and no address verification, so **no
  Phase-2 result is attributed to false sharing**. That is Phase 3's job, and
  §6.2 records this refusal explicitly.
- It does **not** measure tail latency or jitter. Only medians and spreads of
  aggregate throughput are reported; Phase 4 owns the distribution tail.

### 1.4 Evidence labels used in this document

Every substantive statement below is tagged:

- **MEASURED** — a number or count that appears in the raw data or is derived
  from it arithmetically.
- **INTERPRETATION** — a reading of the measured data, offered as *consistent
  with* a possible mechanism. Never a demonstrated cause.
- **LIMITATION** — something this phase does not control, isolate or identify.

Where an older draft made a causal claim that Phase 2 does not measure, this
document states the measurement instead and marks the cause as not isolated.

---

## 2. WHAT — implementations, messages, matrix, reported quantities

### 2.1 Implementations under test

| role | type | header |
|---|---|---|
| candidate | `lltl::SpscRingBuffer<T, Capacity>` | `include/spsc_ring_buffer.h` |
| reference baseline | `lltl::MutexBoundedQueue<T, Capacity>` | `include/mutex_bounded_queue.h` |

Both expose the same conceptual `try_push` / `try_pop` API and are exercised by
the same harness code, so the only difference between the two legs of a cell is
the queue itself. `MutexBoundedQueue` is the correctness reference and is
**deliberately not optimized** — it is a `std::mutex` around fixed storage, not
a tuned lock-based queue. It is the honest "what does a straightforward mutex
cost" baseline, not a straw man chosen to lose, and not a state-of-the-art
lock-based competitor.

**One implementation per process.** Each timed interval contains exactly one
queue type; the mutex and SPSC legs are never timed inside the same process, the
same address space, or the same warmed-up state. The benchmark rejects
`--impl=both`.

### 2.2 Message types

Three fixed-size, trivially copyable, nothrow, allocation-free payloads, each
carrying a sequence number:

| bytes | type | fields | checksum words folded |
|---|---|---|---|
| 8 | `Msg8` | `seq` | 1 |
| 32 | `Msg32` | `seq`, `price`, `qty`, `tag` | 4 |
| 64 | `Msg64` | `seq`, `price`, `qty`, `tag`, `w0..w3` | 8 |

No `std::string`, no dynamic allocation, no pointer-owned payloads. `sizeof`,
trivial-copyability, default-constructibility and nothrow copy/move assignability
are enforced by `static_assert`, so a payload that grows or starts allocating
fails the build rather than silently changing what is being measured.

### 2.3 The message-size axis carries a second variable

**LIMITATION — the message-size axis is not a pure payload-size axis.** The
consumer validates every delivered message by folding its words into a running
order-sensitive checksum, and the number of words it folds is *the size of the
message*: 1 word for `Msg8`, 4 for `Msg32`, 8 for `Msg64` (§2.2). Changing
`message_bytes` therefore changes **both** the payload bytes moved through the
queue **and** the consumer's per-message validation ALU work. The size axis and
the consumer-work axis move together and Phase 2 does not separate them.

What this does and does not affect:

- It **does not** invalidate the mutex-versus-SPSC comparison **within** a cell.
  Inside one `message_bytes + capacity` cell both implementations run the
  identical message type, the identical producer construction path and the
  identical consumer fold, so the fold cost is common to both legs and cancels
  in the ratio. The paired analysis (§4.2) is always read within a cell.
- It **does** mean that **absolute** throughput must not be compared **across**
  message sizes as if only the payload changed. A statement like "64-byte
  messages are 2× faster per message than 32-byte messages" would confound
  payload size with validation work, with the slot-to-cache-line mapping (§7.2)
  and with the ring working-set size (§7.2). **This document makes no such
  causal claim.**

The checksum is not redesigned in this phase; a constant-work validation fold
across message sizes would be a separate change to the measured code.

### 2.4 Matrix

```
impl       x message_bytes x capacity
mutex,spsc x 8,32,64       x 1024,4096,65536   =  18 cells
```

Capacity is a compile-time template parameter dispatched by a small explicit
`switch`; there is no runtime-capacity storage and no Phase-1 API change. The
three capacities are the exact values Phase 2 specified.

### 2.5 Reported quantities

Per measured repetition the benchmark records: `impl`, `message_bytes`,
`capacity`, `message_count`, `elapsed_ns`, `ns_per_message`,
`messages_per_second`, `producer_full_retries`, `consumer_empty_retries`,
`checksum`, `correctness`.

> `ns_per_message` means **end-to-end elapsed time / delivered messages.** It is
> **not** individual `try_push` latency, **not** individual `try_pop` latency,
> and **not** one-way handoff latency. Any reading of these numbers that treats
> them as a per-call cost is wrong.

`producer_full_retries` and `consumer_empty_retries` are **observable metrics,
not correctness failures**: they count how many times the calling thread had to
retry because the queue was momentarily full or empty. They are part of the
measured time and are reported so that the backpressure behaviour of each
implementation is visible rather than hidden.

---

## 3. HOW — the measurement procedure

### 3.1 Two-thread timing harness

1. Both threads are **created outside the timed interval**, along with the queue
   object and an independently precomputed expected-checksum pass.
2. Each thread signals READY and then spins on a `start` flag.
3. The main thread waits for **both** READY signals, takes
   `t0 = steady_clock::now()`, then does `start.store(true, release)`.
4. The producer pushes exactly N messages; the consumer pops until it has
   received N, **records its own completion timestamp immediately after
   receiving the final message**, and the main thread joins both.
5. `elapsed = t1 - t0` therefore covers the complete producer-to-consumer
   transfer of N messages.

Excluded from the timed interval: thread construction, queue allocation, result
printing, RNG, and the warm-up repetition. `ns_per_message = elapsed_ns /
delivered`.

### 3.2 Retry / backpressure policy

Both queue types expose **non-blocking-style `try_*` APIs**, but they are not
equivalent, and the difference matters for how these numbers must be read:

- **`SpscRingBuffer::try_push` / `try_pop` do not lock, spin, sleep or block.**
  Each is a bounded sequence of atomic loads/stores and a payload copy. A failed
  call returns `false` after doing that work and nothing else.
- **`MutexBoundedQueue::try_push` / `try_pop` do not wait for the queue state to
  change** — they never block until space or an item appears, and they never
  wait on a condition variable. **But they must acquire the `std::mutex` to
  inspect the queue state, and that acquisition can block** while the other
  thread holds the lock. **Lock acquisition time is inside the measured mutex
  operation; it is part of the result, not harness overhead, and it is not
  excluded.**

The *harness* supplies the backpressure, with the same loop shape on both legs:

```cpp
while (!q->try_push(msg)) { ++producer_full_retries; /* retry */ }
while (!q->try_pop(msg))  { ++consumer_empty_retries; /* retry */ }
```

**The yield policy, stated exactly as implemented (corrected in Phase 2.1).** A
failing attempt is retried immediately. The harness calls
`std::this_thread::yield()` only after **1024 *consecutive* failed attempts**,
and the consecutive-miss counter is **reset by every successful
`try_push`/`try_pop`**. It is a streak length, not a running total, and it is
never reset by time. There is no sleep anywhere in the loop.

This is a **benchmark-harness** policy only. It lives in
`benchmark/spsc_throughput_bench.cpp`; it does not touch either queue
implementation, and both legs receive it identically.

> **Why this is called out.** An earlier revision of the harness incremented a
> miss counter that was never cleared on success, so a `yield()` did not
> necessarily follow 1024 *consecutive* failures — it followed 1024 *cumulative*
> ones, which on a busy stream arrives far sooner. That is a different scheduler
> interaction, so it changed the measurement. No queue implementation changed;
> every canonical performance number in this document was **regenerated** under
> the corrected policy rather than patched from the old run. The pre-correction
> dataset is preserved unedited at
> `docs/results/spsc-throughput-superseded-fixed-order/`.

### 3.3 What a repetition is, and what a session is — and is not

**A repetition launches a fresh producer/consumer thread pair.** The threads are
constructed for that repetition and joined at the end of it. Nothing about a
repetition's thread placement is inherited by the next one, and nothing pins it
in place while it runs.

**A session is a grouping of repetitions inside one process/address-space
lifetime.** Calling a session a "process" describes how the repetitions were
launched — one OS process, one address space, one allocator state, one thermal
and DVFS history — and *not* a fixed hardware configuration. This document does
**not** claim that a process samples exactly one OS thread placement, that a
session's repetitions share a thread placement, or that placement is fixed for
the life of a process. None of those are true: the threads are new every
repetition, and macOS may migrate a thread between cores while it runs.

**LIMITATION — sources of variation Phase 2 does not separate.** Differences
between repetitions and between sessions may reflect any of:

- scheduler placement of the two threads, and **thread migration** mid-run;
- **P-core versus E-core selection** on this asymmetric host (§7.1);
- **DVFS**, thermal state and power management;
- **allocator and address placement** of the queue object and its slots;
- **background system activity** on a machine that is not quiesced.

**Phase 2 does not identify which of these caused any particular slow or fast
run.** Sessions are sampled repeatedly precisely so the spread is described
rather than hidden behind one launch — not because a session corresponds to a
known placement.

### 3.4 Warm-up

Each process runs **1 warm-up repetition** before its measured repetitions.

**The warm-up repetition is excluded from all reported and derived performance
data.** It does not appear in `raw/`, is not counted in any median, min, max,
spread, pooled or paired figure in this document, and is not used in any
implementation comparison. It is internally timed and validated by the benchmark
(the code path is shared with the measured repetitions, so `Clock::now()` is
still called inside it) — that internal timing is simply never published.

### 3.5 The balanced AB/BA run design

The canonical dataset is **not** a single ordered sweep. Two things are
deliberately controlled.

**One implementation per process, and the two implementations of a cell run as
adjacent processes.** For each `message_bytes + capacity` pair, the mutex
process and the SPSC process are launched back to back, so the two legs of a
comparison are neighbours in time rather than separated by the rest of the
matrix. **This reduces temporal drift between the two legs; it does not
eliminate it.** The two legs are still two separate processes, each of which can
be placed on a different core, migrated, frequency-scaled or preempted
independently, and the design measures no signal that would reveal it (§3.3).

**Implementation order is balanced AB/BA across four sessions.**

| session | traversal | implementation order |
|---|---|---|
| 1 | forward | mutex → spsc |
| 2 | reverse | spsc → mutex |
| 3 | forward | spsc → mutex |
| 4 | reverse | mutex → spsc |

Every cell therefore gets **2 mutex-first and 2 SPSC-first** comparisons, and the
forward/reverse traversal balances position in the cell sweep along the time
axis. `SESSIONS is not overridable` in the runner: the AB/BA balance is defined
for exactly four sessions.

> **Why this design.** The previous canonical run executed all nine mutex cells
> before all nine SPSC cells in every session. Implementation was then perfectly
> confounded with position in time: on an unpinned host where placement,
> migration, DVFS, thermal state and background load all drift during a run, an
> implementation difference and a time-of-run difference were indistinguishable.
> That dataset is preserved, labelled and explained at
> `docs/results/spsc-throughput-superseded-fixed-order/`.

The balance is a **verified property of the run, not an intention**: the runner
parses the actual execution order back out of `command.txt` after the run and
fails the dataset if any cell did not get exactly two mutex-first and two
SPSC-first processes (§3.7).

### 3.6 Correctness gating

Every run validates: exactly N messages consumed, sequence strictly increasing
with no gaps and no duplicates, and a final checksum matching an independently
precomputed stream. On any failure the benchmark exits non-zero and the cell is
**not published**.

### 3.7 Dataset invariants (all verified)

The runner refuses to derive any summary unless all of these hold, and writes the
result to `invariants.txt`:

| invariant | required |
|---|---|
| processes recorded in `command.txt` | 72 |
| raw CSV files | 72 |
| measured rows per raw file | exactly 5 |
| `correctness` on every measured row | `PASS` |
| distinct sessions per cell | exactly 4 |
| times each implementation ran **first** in a cell | exactly 2 each |
| distinct cells | 9 |
| checksum stable within a cell | yes |
| per-process summary median/min/max vs its own raw CSV | match |

The canonical run reports `all_invariants=PASS`.

### 3.8 Reproducing the run

```sh
scripts/spsc-throughput.sh            # canonical matrix: 18 cells x 4 sessions
MESSAGES=200000 REPS=3 scripts/spsc-throughput.sh   # smoke-sized matrix
```

The script builds a fresh Release tree, forces `-O3 -DNDEBUG` on the benchmark
target, runs one process per cell in the balanced order above, preserves raw
output before deriving anything, then derives and cross-checks the summaries.
`MESSAGES` (default 10000000), `REPS` (5), `WARMUP` (1) and `OUT` are
overridable. **`SESSIONS` is not overridable** — the AB/BA balance is defined for
exactly 4.

---

## 4. MEASURED — the canonical dataset

Host: Apple M3 Max (Mac15,10), 14 logical cores (10 P + 4 E), 36 GiB,
macOS 14.2.1 (23C71), Apple clang 15.0.0, `-O3 -DNDEBUG`, C++20.

| item | value |
|---|---|
| `message_count` | 10,000,000 per repetition |
| measured repetitions | 5 per process |
| warm-up repetitions | 1 per process, excluded (§3.4) |
| sessions | 4, balanced AB/BA (§3.5) |
| processes | 72 (9 pairs × 2 impls × 4 sessions) |
| measured repetitions total | 360 |
| measured repetitions per cell | 40 (20 per implementation) |
| measured message transfers | **3.6 billion** |
| wall time | 8 m 03 s |
| correctness | `correctness=PASS` on all 360 measured repetitions |
| invariants | `all_invariants=PASS` (§3.7) |

Measured 2026-09-11, UTC 08:05:08. Full provenance in
`docs/results/spsc-throughput/HOST.md`, `command.txt`, `invariants.txt` and
`RESULTS_METADATA.md`. Every one of the 360 measured repetitions is preserved in
`raw/`; none was discarded and no "best run" was selected anywhere.

### 4.1 Per-process (session) medians

Each row is one implementation in one cell; each column is one session's median
over that process's 5 measured repetitions. Full table:
`docs/results/spsc-throughput/SESSIONS.md`.

| impl | bytes | cap | session 1 | session 2 | session 3 | session 4 |
|---|---|---|---|---|---|---|
| mutex | 8 | 1024 | 52.901 | 46.046 | 51.034 | 47.681 |
| spsc | 8 | 1024 | 53.334 | 32.659 | 37.475 | 25.706 |
| mutex | 8 | 4096 | 24.220 | 22.316 | 22.602 | 23.212 |
| spsc | 8 | 4096 | 18.804 | 17.569 | 22.309 | 25.431 |
| mutex | 8 | 65536 | 18.752 | 18.651 | 19.696 | 19.308 |
| spsc | 8 | 65536 | 46.766 | 41.988 | 32.183 | 32.603 |
| mutex | 32 | 1024 | 34.714 | 36.510 | 33.905 | 32.730 |
| spsc | 32 | 1024 | 36.513 | 30.409 | 36.834 | 28.284 |
| mutex | 32 | 4096 | 23.209 | 22.536 | 23.009 | 23.036 |
| spsc | 32 | 4096 | 38.657 | 30.794 | 31.840 | 29.590 |
| mutex | 32 | 65536 | 21.155 | 21.184 | 21.246 | 21.123 |
| spsc | 32 | 65536 | 45.691 | 46.311 | 50.304 | 47.873 |
| mutex | 64 | 1024 | 45.966 | 46.005 | 42.710 | 42.236 |
| spsc | 64 | 1024 | 39.306 | 37.064 | 41.044 | 37.997 |
| mutex | 64 | 4096 | 28.988 | 27.475 | 27.897 | 27.982 |
| spsc | 64 | 4096 | 13.211 | 15.741 | 15.614 | 14.718 |
| mutex | 64 | 65536 | 23.443 | 22.191 | 23.366 | 23.911 |
| spsc | 64 | 65536 | 14.568 | 14.993 | 14.069 | 17.016 |

### 4.2 Paired-session comparison — **PRIMARY**

Files: `docs/results/spsc-throughput/paired_summary.csv` and
`PAIRED_COMPARISON.md`.

**This is the primary implementation comparison**, and it is deliberately *not*
a pooled one. Within each session, the mutex process and the SPSC process of a
cell ran adjacently, within the same few-tens-of-seconds window, with the order
balanced AB/BA. The paired ratio

```
ratio_session = (SPSC session median ns/msg) / (mutex session median ns/msg)
```

compares two processes that were close neighbours in time. **Adjacent execution
reduces temporal drift between the two legs but cannot guarantee identical
scheduler, DVFS, thermal, or background-system state** — the two processes are
still separated by a full benchmark run, and each leg can be placed, migrated or
frequency-scaled independently. The pairing narrows the gap; it does not close
it. **`ratio < 1` means SPSC completed a message faster in that session.**

| bytes | cap | median ratio | min | max | sessions SPSC faster | sessions mutex faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 0.722 | 0.539 | 1.008 | 3 | 1 | **SPLIT 3–1** |
| 8 | 4096 | 0.887 | 0.776 | 1.096 | 3 | 1 | **SPLIT 3–1** |
| 8 | 65536 | 1.970 | 1.634 | 2.494 | 0 | 4 | stable — mutex 4/4 |
| 32 | 1024 | 0.958 | 0.833 | 1.086 | 2 | 2 | **SPLIT 2–2** |
| 32 | 4096 | 1.375 | 1.285 | 1.666 | 0 | 4 | stable — mutex 4/4 |
| 32 | 65536 | 2.226 | 2.160 | 2.368 | 0 | 4 | stable — mutex 4/4 |
| 64 | 1024 | 0.877 | 0.806 | 0.961 | 4 | 0 | stable — SPSC 4/4 |
| 64 | 4096 | 0.543 | 0.456 | 0.573 | 4 | 0 | stable — SPSC 4/4 |
| 64 | 65536 | 0.649 | 0.602 | 0.712 | 4 | 0 | stable — SPSC 4/4 |

**6 of 9 cells have a direction that held in all four sessions** — three in
favour of the mutex baseline, three in favour of SPSC. **3 cells split**, and
none of the six stable cells changes sign between the mutex-first and SPSC-first
sessions.

Sign consistency is a **descriptive** summary of 4 paired observations. It is
**not** a significance test, and a 4/4 cell is reported as *"the same direction
in all four balanced sessions"*, not as "proven" or "equivalent to zero
difference".

Per-session detail, including which implementation ran first in each session, is
in `PAIRED_COMPARISON.md`.

### 4.3 Pooled matrix — **SECONDARY**

Files: `docs/results/spsc-throughput/summary.csv` and `MATRIX.md`.

| impl | bytes | cap | pooled reps | median ns/msg | min | max | spread | median msg/s |
|---|---|---|---|---|---|---|---|---|
| mutex | 8 | 1024 | 20 | 49.147 | 41.818 | 57.094 | 36.5% | 20,347,102 |
| spsc | 8 | 1024 | 20 | 36.089 | 19.240 | 34706.397 | 180291.1% | 27,708,959 |
| mutex | 8 | 4096 | 20 | 23.204 | 21.669 | 26.471 | 22.2% | 43,096,846 |
| spsc | 8 | 4096 | 20 | 20.835 | 13.357 | 31.852 | 138.5% | 47,995,647 |
| mutex | 8 | 65536 | 20 | 19.217 | 17.418 | 20.581 | 18.2% | 52,038,235 |
| spsc | 8 | 65536 | 20 | 41.636 | 28.879 | 51.380 | 77.9% | 24,017,496 |
| mutex | 32 | 1024 | 20 | 34.271 | 32.030 | 38.816 | 21.2% | 29,179,560 |
| spsc | 32 | 1024 | 20 | 32.249 | 25.255 | 40.617 | 60.8% | 31,009,044 |
| mutex | 32 | 4096 | 20 | 22.963 | 21.843 | 23.679 | 8.4% | 43,548,440 |
| spsc | 32 | 4096 | 20 | 31.708 | 26.161 | 39.122 | 49.5% | 31,538,058 |
| mutex | 32 | 65536 | 20 | 21.169 | 20.553 | 22.158 | 7.8% | 47,237,986 |
| spsc | 32 | 65536 | 20 | 47.766 | 43.369 | 59.733 | 37.7% | 20,935,206 |
| mutex | 64 | 1024 | 20 | 45.924 | 38.631 | 48.860 | 26.5% | 21,774,983 |
| spsc | 64 | 1024 | 20 | 38.914 | 35.264 | 47.042 | 33.4% | 25,697,839 |
| mutex | 64 | 4096 | 20 | 27.940 | 24.237 | 30.841 | 27.2% | 35,791,138 |
| spsc | 64 | 4096 | 20 | 14.353 | 12.673 | 18.745 | 47.9% | 69,670,341 |
| mutex | 64 | 65536 | 20 | 23.210 | 21.596 | 24.827 | 15.0% | 43,084,811 |
| spsc | 64 | 65536 | 20 | 14.780 | 12.552 | 19.556 | 55.8% | 67,657,520 |

Every cell's checksum is identical across all 40 of its repetitions (20 per
implementation) and differs across message sizes — the delivered stream is
exactly the expected one.

**This table is secondary and descriptive**, and it should not be used for
implementation direction. Pooling 20 repetitions as though they were 20
independent samples is wrong here: the 5 repetitions inside one process share
that process's address space, allocator state and thermal history, so they are
correlated, and the pooled `min`/`max` are extremes over correlated samples. The
pooled **spread** column is still informative as a description of how wide the
observed distribution is. The pooled median is retained as a secondary
descriptive metric only.

### 4.4 Observed-range descriptive check

Kept as a **descriptive** view of the pooled data — and read with the §4.3
caveat that the pooled samples are not independent.

| bytes | cap | mutex ns/msg | spsc ns/msg | ratio | mutex [min,max] | spsc [min,max] | descriptive reading |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 49.147 | 36.089 | 0.734 | [41.82, 57.09] | [19.24, 34706.40] | ranges overlap — inconclusive |
| 8 | 4096 | 23.204 | 20.835 | 0.898 | [21.67, 26.47] | [13.36, 31.85] | ranges overlap — inconclusive |
| 8 | 65536 | 19.217 | 41.636 | 2.167 | [17.42, 20.58] | [28.88, 51.38] | observed ranges separated |
| 32 | 1024 | 34.271 | 32.249 | 0.941 | [32.03, 38.82] | [25.25, 40.62] | ranges overlap — inconclusive |
| 32 | 4096 | 22.963 | 31.708 | 1.381 | [21.84, 23.68] | [26.16, 39.12] | observed ranges separated |
| 32 | 65536 | 21.169 | 47.766 | 2.256 | [20.55, 22.16] | [43.37, 59.73] | observed ranges separated |
| 64 | 1024 | 45.924 | 38.914 | 0.847 | [38.63, 48.86] | [35.26, 47.04] | ranges overlap — inconclusive |
| 64 | 4096 | 27.940 | 14.353 | 0.514 | [24.24, 30.84] | [12.67, 18.74] | observed ranges separated |
| 64 | 65536 | 23.210 | 14.780 | 0.637 | [21.60, 24.83] | [12.55, 19.56] | observed ranges separated |

**How to read this criterion — and what it is not.**

- "Observed ranges separated" means exactly that the two pooled `[min, max]`
  intervals do not overlap. It is a **descriptive observation about these runs**.
  It is **not** a significance test, not a confidence interval, and not a
  hypothesis test; no null hypothesis is stated, no p-value is computed, and the
  correlation between repetitions inside a process (§4.3) would violate the
  independence such a test assumes.
- "Ranges overlap — inconclusive" means **not separable under this descriptive
  criterion**. Overlapping ranges are **not** evidence that the two
  implementations are equivalent, equally fast, or "effectively tied"; they mean
  this dataset does not distinguish them. **No equivalence is claimed for any
  overlapping cell.**
- For `spsc 8 / 1024` the pooled `max` of 34706.40 ns/msg does not describe
  throughput at all — it is one repetition whose wall-clock interval was
  interrupted (§4.6). That single value is why the range is reported as spanning
  four orders of magnitude.

This criterion is reported for completeness and continuity. The **paired-session
analysis in §4.2 is what this document uses for direction**, because it compares
neighbouring processes rather than pooling correlated samples, and because it is
median-based and therefore not dominated by the §4.6 outlier.

### 4.5 Retry behaviour (MEASURED)

Retries per *delivered* message — totals from `summary.csv` divided by the
200,000,000 messages delivered per cell:

| impl | bytes | cap | full-queue retries/msg | empty-queue retries/msg |
|---|---|---|---|---|
| mutex | 8 | 1024 | 3.129 | 0.339 |
| mutex | 8 | 4096 | 0.284 | 0.176 |
| mutex | 8 | 65536 | 0.086 | 0.068 |
| mutex | 32 | 1024 | 0.891 | 0.470 |
| mutex | 32 | 4096 | 0.081 | 0.200 |
| mutex | 32 | 65536 | 0.003 | 0.162 |
| mutex | 64 | 1024 | 1.162 | 0.395 |
| mutex | 64 | 4096 | 0.428 | 0.067 |
| mutex | 64 | 65536 | 0.246 | 0.002 |
| spsc | 8 | 1024 | 0.454 | 4.812 |
| spsc | 8 | 4096 | 0.237 | 2.699 |
| spsc | 8 | 65536 | 0.289 | 6.552 |
| spsc | 32 | 1024 | 0.037 | 4.722 |
| spsc | 32 | 4096 | 0.009 | 4.776 |
| spsc | 32 | 65536 | 0.002 | 10.297 |
| spsc | 64 | 1024 | 0.005 | 4.613 |
| spsc | 64 | 4096 | 0.084 | 0.068 |
| spsc | 64 | 65536 | 0.036 | 0.107 |

**MEASURED (directional tallies).**

- **mutex:** full-queue retries exceed empty-queue retries in **7 of 9** cells.
- **SPSC:** empty-queue retries exceed full-queue retries in **8 of 9** cells.
- **SPSC's producer almost never fails:** full-queue retry rate is ≤ 0.454/msg in
  every cell, and ≤ 0.084/msg in 6 of 9.
- **SPSC's consumer frequently finds the queue empty:** empty-queue retry rate is
  ≥ 2.699/msg in 7 of 9 cells, but drops to 0.068 and 0.107/msg in the two
  64-byte cells at capacities 4096 and 65536.
- **mutex at capacity 1024:** full-queue retry rate is 0.891–3.129/msg versus
  ≤ 0.428/msg in every other mutex cell.

The two implementations fail *differently*, and that is the observation reported
here. What causes the difference is **not** isolated by Phase 2 (§6.2).

### 4.6 One measured anomaly, reported explicitly

One repetition out of the 360 — `raw/spsc_b8_c1024_s1.csv`, measured repetition
index 2 — recorded `elapsed_ns = 347,063,971,125`, i.e. **347 seconds** for
10,000,000 messages (34706 ns/msg) instead of the ~0.4 s its four siblings took.

**MEASURED:** that run reported `correctness=PASS`, produced the same checksum as
every other repetition of the cell, and recorded 5,533,585 full-queue and
55,119,743 empty-queue retries — *lower* than three of its four siblings in the
same process. The producer and consumer were therefore not stuck retrying
against the queue: work did not stall on queue state. **The observation is
consistent with a long external descheduling or suspension event, but wall-clock
timing alone cannot identify its cause.** The elapsed-time column of a wall-clock
interval cannot distinguish "slow code" from "process not scheduled"; it records
that roughly 346 seconds passed without the measured transfer progressing, not
why.

**How it is handled:** nothing is deleted or edited. The repetition stays in
`raw/` and remains in the pooled `min`/`max` (§4.3, §4.4) where it is visible.

**How it is neutralised:** every implementation comparison in this document is
**median-based** (§4.2), and a single outlier cannot move a 5-repetition median.
This is a concrete reason the paired-session analysis is primary and the pooled
range criterion is secondary.

**LIMITATION:** Phase 2 does not control or detect external descheduling or
suspension, and this harness records no signal that would identify one, so the
possibility of a smaller interruption in another repetition cannot be excluded
from any cell.

### 4.7 How large is the variation (MEASURED)

**Between processes/sessions** — max ÷ min of the four session medians in §4.1:

| impl | range of between-session movement | cells |
|---|---|---|
| mutex | 0.6% – 14.9% | all 9 cells |
| spsc | 10.1% – 107.5% | all 9 cells |

**Between repetitions inside a process** — the pooled spread column of §4.3,
which is `max/min − 1` over 20 pooled repetitions: mutex cells 7.8% – 36.5%;
SPSC cells 33.4% – 138.5% with one cell at 180291% because of the §4.6 outlier.

**MEASURED:** SPSC's session-to-session movement is larger than the mutex
baseline's in **9 of 9** cells. **LIMITATION:** Phase 2 does not identify the cause
(§3.3). **INTERPRETATION:** the observation is consistent with the SPSC cells
being more sensitive to how the two threads are placed relative to each other
than the mutex baseline is — but no placement was measured, so this remains a
hypothesis, and it is deliberately not called a finding.

---

## 5. DERIVED — the Phase-2.1 questions answered

Everything in this section is computed from the canonical dataset in §4; no new
measurement was taken. All five answers are **descriptive**.

### Q1. Is SPSC consistently faster or slower than the mutex reference for any cell?

**Yes, for six of the nine cells** — the direction held in all four balanced
sessions (§4.2):

| direction | cells | median paired ratio |
|---|---|---|
| SPSC faster in 4/4 sessions | 64 / 1024 | 0.877 |
| SPSC faster in 4/4 sessions | 64 / 4096 | 0.543 |
| SPSC faster in 4/4 sessions | 64 / 65536 | 0.649 |
| mutex faster in 4/4 sessions | 8 / 65536 | 1.970 |
| mutex faster in 4/4 sessions | 32 / 4096 | 1.375 |
| mutex faster in 4/4 sessions | 32 / 65536 | 2.226 |

For those six cells the honest summary is: *in every balanced session, the same
implementation completed messages faster.* The remaining three cells (8/1024,
8/4096, 32/1024) show no consistent direction at all.

### Q2. Are directional results stable under balanced AB/BA process ordering?

**Yes for the six cells above; no for the other three — and the sign is not
determined by run order.**

In the three split cells the sign flips *within* the same order class: cells
8/1024 and 8/4096 are SPSC-favouring in one mutex-first session and
mutex-favouring in the other; cell 32/1024 is mutex-favouring in one SPSC-first
session and SPSC-favouring in the other. The instability is therefore **not**
explained by which implementation ran first, which is the specific confound the
AB/BA design exists to remove.

For the six stable cells, the direction additionally survived both forward and
reverse traversal, i.e. both ends of the cell sweep and both ends of the run's
time axis.

### Q3. How large is between-repetition / between-process variation?

- Between processes (session medians): **mutex 0.6%–14.9%**, **SPSC 10.1%–107.5%**
  (§4.7).
- Between repetitions (pooled spread): **mutex 7.8%–36.5%**, **SPSC 33.4%–138.5%**
  plus one cell containing a documented unexplained wall-clock anomaly (§4.6).
- The largest single movement observed anywhere in the canonical dataset is
  `spsc 8 / 1024`: session medians of 25.706, 32.659, 37.475 and 53.334 ns/msg —
  a factor of **2.07** between two sessions of the same implementation on the
  same host with the same binary. `mutex 32 / 65536` moved by 0.6% over the same
  four sessions.

**LIMITATION:** Phase 2 does not attribute any of this movement to a cause
(§3.3).

### Q4. How do retry patterns differ?

**MEASURED**, §4.5: the mutex baseline's failed attempts are predominantly
producer-side (queue full) in 7 of 9 cells; SPSC's are predominantly consumer-side
(queue empty) in 8 of 9 cells. SPSC's producer is nearly never blocked
(≤ 0.454 retries/message everywhere, ≤ 0.084 in 6 of 9 cells) while its consumer
frequently finds nothing to take (≥ 2.7 retries/message in 7 of 9 cells).

**LIMITATION:** retry counts are counts of failed *API calls*, and a failed call
costs different work in the two implementations — a `try_pop` on the mutex
baseline acquires and releases the lock, while a failed `try_pop` on SPSC is a
pair of atomic loads. The counts are comparable as *behaviour* and are not
comparable as *work*.

### Q5. Which cells remain inconclusive?

**Three:** `8 / 1024` (3–1), `8 / 4096` (3–1) and `32 / 1024` (2–2). In these
cells the direction changed between balanced sessions, so this dataset does not
establish a direction for them. The pooled range criterion (§4.4) flags the same
three cells as overlapping, which is a consistency check between two independent
views, not a second test.

**No result anywhere in this document is attributed to false sharing.** Phase 2
has no packed/separated control and no cache-line or address verification
(§6.2).

---

## 6. INTERPRETATION

### 6.1 What the dataset supports

**MEASURED.** SPSC has the lower median in 6 of 9 cells in the pooled view
(§4.3) and a consistent direction in 6 of 9 cells in the paired view (§4.2) —
but the two sets of six are **not the same cells**, and the direction is not
uniform: three cells are consistently SPSC-favouring and three consistently
mutex-favouring.

**INTERPRETATION.** Taken together, this is consistent with the frozen,
unpadded, unbuffered SPSC protocol and a plain `std::mutex` around fixed storage
being *competitive* rather than one dominating the other, with the outcome
depending on the cell. The phase was designed to report whatever the machine
produced, and what it produced is a split result with a stable minority on each
side.

**LIMITATION.** The dataset does not explain *why* any individual cell lands
where it does. In particular:

- The measurement does not isolate the cost of the atomic cursor protocol from
  the cost of the mutex, from payload movement, from coherence traffic, or from
  scheduler placement (§1.2, §1.3).
- No mechanism is claimed for any cell. Where a plausible mechanism exists, it
  is named below as a hypothesis, never as a cause.

**Hypotheses the data is consistent with — none of them measured here:**

- *The 64-byte SPSC cells.* All three consistently SPSC-favouring cells are at
  64 bytes. **LIMITATION:** at 64 bytes the message-size axis is simultaneously
  the slot-to-cache-line-mapping axis and the consumer-fold-work axis (§2.3,
  §7.2), so this is not evidence that payload size causes the difference.
- *The two capacity-65536 cells where SPSC is 2.0–2.2× slower.* **LIMITATION:**
  the capacity axis is also the ring working-set-size axis (1024 × 8 B = 8 KiB
  vs 65536 × 64 B = 4 MiB), and these cells also have the highest SPSC
  empty-queue retry rates in the matrix (6.552 and 10.297 per message, §4.5), so
  the measured time contains a large failed-attempt component (§7.3). Which
  factor dominates is not isolated.
- *The mutex cells at capacity 1024.* **MEASURED:** their full-queue retry rate is
  0.891–3.129/msg, far above every other mutex cell. **INTERPRETATION:** this is
  consistent with a small ring filling faster than the consumer drains it, so the
  producer meets a full queue more often. **LIMITATION:** Phase 2 does not
  measure why the drain is slower there, and no lock-contention, cache or
  coherence explanation is claimed.

### 6.2 What Phase 2 explicitly does not claim

- **No false-sharing attribution.** The unpadded `head_`/`tail_` layout permits
  and is likely to exhibit false sharing. But Phase 2 contains no
  packed/separated control, no address verification, and no cache-line
  instrumentation, so it cannot distinguish false sharing from ordinary
  coherence traffic, from thread placement, from the retry loop, or from any of
  the other factors in §3.3. **Every difference in §4 is left unattributed**,
  and false sharing is named only as a Phase-3 hypothesis.
- **No claim that session differences are caused by thread placement.** No
  placement was recorded or controlled (§3.3).
- **No claim that SPSC is or is not "faster than a mutex" in general.** The
  measurement answers a narrower question — this code, this host, this message
  count, this retry policy — and the answer is "six cells have a stable
  direction, three do not".
- **No equivalence claim.** Cells whose observed ranges overlap are
  *inconclusive*, not equal (§4.4).
- **No extrapolation to other platforms.** These are macOS/Apple-Silicon numbers
  with an unpinned scheduler (§7.1).
- **No per-call latency.** §1.3.

---

## 7. LIMITATIONS

### 7.1 macOS scheduling: no affinity is implemented or claimed

This is Apple Silicon. **No hard CPU pinning or thread affinity is implemented,
and none is claimed.** Concretely:

- macOS offers no supported public API for pinning a thread to a core; the
  `thread_policy_set` / affinity-tag mechanisms are (a) advisory, (b) not
  honored as a hard pin, and (c) not usable in this phase without a documented,
  verified implementation. **Phase 2 contains no macOS affinity hacks.**
- The host is **asymmetric**: 10 performance and 4 efficiency cores. Which kind
  of core each of the two threads lands on is the scheduler's choice, and the
  two threads may land on cores of different classes.
- Threads may be **migrated** between cores within a repetition, and because a
  repetition creates a fresh thread pair, placement is re-decided for every
  repetition as well (§3.3).
- **Frequency** is managed by the OS (DVFS, thermal and power state) and is not
  read or controlled here.
- **System load** is not isolated: no attempt is made to quiesce the machine,
  disable background daemons, or reserve cores, so other work on the host can
  influence a cell — and demonstrably did once (§4.6).
- The harness does **yield** on long consecutive retry streaks (§3.2). That is a
  harness policy applied identically to both implementations, but it does
  interact with the scheduler and is therefore part of the measurement, not
  outside it.

A consequence worth stating plainly: **these numbers characterize this machine
under ordinary conditions, not a controlled real-time environment.**

### 7.2 Confounds the matrix does not break

- **Consumer validation work moves with message size** — 1, 4 and 8 folded words
  for `Msg8`, `Msg32`, `Msg64` (§2.3). Within-cell comparisons are unaffected;
  cross-size absolute comparisons are not made.
- **Slot-to-cache-line mapping** moves with message size (8-byte slots pack 8
  per line, 64-byte slots occupy a whole line), so the message-size axis is not
  a clean "payload cost" axis at 64 bytes.
- **Ring working-set size** moves with capacity (1024 × 8 B = 8 KiB vs
  65536 × 64 B = 4 MiB), so the capacity axis is not a clean "queue length"
  axis either.
- **The two implementations are not the same shape of code.** The mutex
  baseline's failed attempt takes and releases a lock; SPSC's failed attempt is
  a relaxed-plus-acquire load pair. Retry counts (§4.5) are therefore not
  directly comparable as "work", only as behaviour.
- **Implementation is balanced against time order, not eliminated.** The AB/BA
  design removes a *systematic* order confound (§3.5); it does not make the two
  legs of a pair simultaneous, so a fast-moving disturbance between the two
  adjacent processes still lands on one of them.

### 7.3 The retry loop is inside the measurement

Both legs measure a *saturating producer against a consumer that spins* — the
producer never sleeps, and the consumer pays for every failed `try_pop`. A
starved consumer therefore inflates end-to-end time directly: at 10.297
empty-queue retries per message (`spsc 32 / 65536`, the slowest SPSC cell), a
substantial part of the measured 47.8 ns/msg is failed-attempt overhead rather
than transfer work. This is a property of the chosen backpressure policy
interacting with the queue, not a "pure" handoff cost, and it applies to both
implementations.

### 7.4 The baseline is a plain mutex, not a tuned one

`MutexBoundedQueue` is deliberately unoptimized (one `std::mutex`, no
condition-variable handoff, no lock-free fast path). Its retry path takes the
lock on every failed attempt. Conclusions are therefore about *this* baseline; a
tuned lock-based queue could narrow or reverse the stable cells, and Phase 2 does
not attempt to establish where the mutex's ceiling is.

### 7.5 Scope of the dataset

One host, one toolchain, one build (`-O3 -DNDEBUG`, Apple clang 15.0.0), one
message count (10,000,000), one machine state per launch. No cross-platform
comparison, no cross-compiler comparison, and no claim that 10,000,000 messages
is enough to have reached a true steady state on every cell. A re-run reproduces
the *distribution*, not the exact values; the spread is part of the result.

---

## 8. Phase status

- Experiment 01 — COMPLETE / FROZEN.
- **Experiment 02, Phase 1 — Correctness / Memory Model: COMPLETE / FROZEN.**
- **Experiment 02, Phase 2 — Throughput Baseline: COMPLETE / FROZEN.**
  Canonical dataset: `docs/results/spsc-throughput/` — 4 balanced AB/BA sessions,
  72 processes, 360 measured repetitions, all invariants PASS.
- **Experiment 02, Phase 3 — False Sharing / Cursor Caching: NOT STARTED.**
- **Experiment 02, Phase 4 — Tail Latency: NOT STARTED.**

Superseded datasets, retained as labelled artifacts and not to be cited for
comparative claims:

| directory | why superseded |
|---|---|
| `docs/results/spsc-throughput-superseded-fixed-order/` | implementation confounded with run order (all mutex before all SPSC); earlier retry/yield policy |
| `docs/results/spsc-throughput-superseded-single-session/` | single session; no cross-session sampling |

Phase 3 will introduce the controlled variables Phase 2 deliberately held
fixed — packed same-line versus separated/padded cursors, verified by actual
cursor addresses and cache-line placement, plus remote-cursor caching — and will
re-measure against this baseline. No Phase-2 number above may be cited as
evidence for or against false sharing in the Phase-1 layout.
