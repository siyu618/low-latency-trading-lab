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
what makes this a *baseline*. See §6.2.

**Status: Phase 2 — Throughput Baseline: COMPLETE / FROZEN.**
Canonical dataset: `docs/results/spsc-throughput/` (measured 2026-09-11 on the
Apple M3 Max development host).

---

## 1. WHY — the question Phase 2 is allowed to ask

### 1.1 Research question

> How much steady-state end-to-end message-transfer throughput does the Phase-1
> SPSC protocol provide relative to a mutex-serialized bounded queue under
> controlled message sizes and capacities?

**The answer is not claimed in advance.** Phase 2 was designed, built and run
without assuming which implementation wins, and §5 reports the outcome the
machine actually produced — which is not a clean win for either side.

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

| bytes | type | fields |
|---|---|---|
| 8 | `Msg8` | `seq` |
| 32 | `Msg32` | `seq`, `price`, `qty`, `tag` |
| 64 | `Msg64` | `seq`, `price`, `qty`, `tag`, `w0..w3` |

No `std::string`, no dynamic allocation, no pointer-owned payloads. `sizeof`,
trivial-copyability, default-constructibility and nothrow copy/move assignability
are enforced by `static_assert`, so a payload that grows or starts allocating
fails the build rather than silently changing what is being measured. Both
implementations use the identical message construction path and the identical
per-sequence value derivation, so a difference between them cannot come from the
payload.

### 2.3 Matrix

```
impl       x message_bytes x capacity
mutex,spsc x 8,32,64       x 1024,4096,65536   =  18 cells
```

Capacity is a compile-time template parameter dispatched by a small explicit
`switch`; there is no runtime-capacity storage and no Phase-1 API change. The
three capacities are the exact values Phase 2 specified.

### 2.4 Reported quantities

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
printing, RNG, and the warm-up. `ns_per_message = elapsed_ns / delivered`.

### 3.2 Retry / backpressure policy

Both queues remain **non-spinning APIs**: `try_push` / `try_pop` return
immediately and never wait or sleep internally. The *harness* supplies the
backpressure, with the same policy on both legs:

```cpp
while (!q.try_push(msg)) { ++producer_full_retries;  /* busy retry + occasional yield */ }
while (!q.try_pop(msg))  { ++consumer_empty_retries; /* busy retry + occasional yield */ }
```

A failing attempt is retried immediately, with a `std::this_thread::yield()`
after 1024 consecutive misses. There is no sleep. This loop is part of the
measurement for both implementations equally (§7.3).

### 3.3 Warm-up, repetitions and sessions

- Each process runs an **untimed, unreported warm-up** before its measured
  repetitions.
- **5 measured repetitions per process**, all kept, plus the per-session medians
  so the distribution is visible.
- **3 independent processes (sessions) per cell**, all kept, pooled.
- Every repetition is preserved in `raw/`; no run is silently discarded and the
  reported figure is never "the best one".

Pooling across sessions exists because a single process samples exactly one OS
thread placement and one allocator placement, and this host was observed to hand
out placements that differ by ~3× for byte-identical machine code. That finding
and its evidence are recorded in
`docs/results/spsc-throughput-superseded-single-session/SUPERSEDED.md`; the
mitigation was to *sample more placements*, never to change the measured code.

### 3.4 Correctness gating

Every run validates: exactly N messages consumed, sequence strictly increasing
with no gaps and no duplicates, and a final checksum matching an independently
precomputed stream. On any failure the benchmark exits non-zero and the cell is
**not published**. The canonical run reports `correctness=PASS` for all 54
processes, and the runner additionally re-verifies every summary against the raw
CSV it claims to summarize before deriving any matrix.

### 3.5 Reproducing the run

```sh
scripts/spsc-throughput.sh            # canonical matrix: 18 cells x 3 sessions
MESSAGES=200000 REPS=3 scripts/spsc-throughput.sh   # smoke-sized matrix
```

The script builds a fresh Release tree, forces `-O3 -DNDEBUG` on the benchmark
target, runs one process per cell, preserves raw output, then derives and
cross-checks the summary. `MESSAGES` (default 10000000), `REPS` (5), `SESSIONS`
(3), `WARMUP` (1) and `OUT` are overridable.

---

## 4. MEASURED — the canonical dataset

Host: Apple M3 Max (Mac15,10), 14 logical cores (10 P + 4 E), 36 GiB,
macOS 14.2.1 (23C71), Apple clang 15.0.0, `-O3 -DNDEBUG`, C++20.
`message_count = 10000000` per repetition; 15 pooled repetitions per cell;
measured 2026-09-11, UTC 07:09:52. Full provenance in
`docs/results/spsc-throughput/HOST.md`, `command.txt` and `RESULTS_METADATA.md`.

Volume: 18 cells × 15 repetitions × 10,000,000 messages = **2.7 billion**
measured message transfers, plus warm-ups. All 54 processes `correctness=PASS`.

### 4.1 Canonical matrix (pooled over 3 sessions)

`ns/msg` = end-to-end elapsed / messages delivered; `min`/`max` are over the 15
pooled repetitions; `spread` = `max/min − 1`.

| impl | bytes | capacity | median ns/msg | min | max | spread | median msg/s |
|---|---|---|---|---|---|---|---|
| mutex | 8 | 1024 | 54.179187 | 37.711258 | 59.434383 | 57.604% | 18,457,272 |
| mutex | 8 | 4096 | 24.080171 | 23.287742 | 26.653071 | 14.451% | 41,527,944 |
| mutex | 8 | 65536 | 21.238917 | 19.865325 | 24.482404 | 23.242% | 47,083,380 |
| mutex | 32 | 1024 | 40.339529 | 36.169742 | 43.951188 | 21.514% | 24,789,580 |
| mutex | 32 | 4096 | 25.294542 | 23.875600 | 27.566279 | 15.458% | 39,534,220 |
| mutex | 32 | 65536 | 22.733117 | 21.314888 | 23.669438 | 11.047% | 43,988,688 |
| mutex | 64 | 1024 | 45.193958 | 41.009308 | 53.238158 | 29.820% | 22,126,852 |
| mutex | 64 | 4096 | 30.385200 | 26.167021 | 32.719554 | 25.041% | 32,910,759 |
| mutex | 64 | 65536 | 25.366746 | 23.646517 | 30.282442 | 28.063% | 39,421,690 |
| spsc | 8 | 1024 | 34.682263 | 19.186242 | 41.573608 | 116.684% | 28,833,182 |
| spsc | 8 | 4096 | 18.052813 | 10.302021 | 39.309242 | 281.568% | 55,393,029 |
| spsc | 8 | 65536 | 34.623804 | 22.569054 | 40.772254 | 80.656% | 28,881,864 |
| spsc | 32 | 1024 | 38.109292 | 28.570258 | 43.861658 | 53.522% | 26,240,320 |
| spsc | 32 | 4096 | 30.861983 | 27.939317 | 35.349767 | 26.523% | 32,402,325 |
| spsc | 32 | 65536 | 48.372858 | 44.703142 | 53.285629 | 19.199% | 20,672,750 |
| spsc | 64 | 1024 | 40.324587 | 36.725538 | 42.835300 | 16.636% | 24,798,766 |
| spsc | 64 | 4096 | 14.130263 | 12.998429 | 18.765262 | 44.366% | 70,770,091 |
| spsc | 64 | 65536 | 14.926150 | 13.232088 | 19.283879 | 45.736% | 66,996,513 |

Every cell's checksum is identical across all 15 pooled repetitions and all 3
sessions, and differs across message sizes — the delivered stream is exactly the
expected one.

### 4.2 Per-session medians

Session-to-session movement is large and is the dominant source of spread in
several cells; the full table is `docs/results/spsc-throughput/SESSIONS.md`.
The clearest examples (median ns/msg, sessions 1→3):

| cell | session 1 | session 2 | session 3 | pooled |
|---|---|---|---|---|
| spsc 8 / 4096 | 12.962683 | 24.939433 | 18.052813 | 18.052813 |
| spsc 32 / 65536 | 45.702817 | 49.238804 | 50.458679 | 48.372858 |
| mutex 8 / 1024 | 54.466946 | 48.066662 | 57.029133 | 54.179187 |
| mutex 8 / 4096 | 23.844925 | 24.080171 | 24.458446 | 24.080171 |

`spsc 8 / 4096` moves by a factor of 1.92 between two launches of the *same*
binary; `mutex 8 / 4096` moves by 2.6%. The mutex cells are comparatively stable
across sessions; the SPSC cells are not. This is a property of how this host
places two cache-coherence-heavy threads, and it is why the pooled spread is
reported rather than a single-session median.

### 4.3 Retry behaviour (MEASURED)

Retries per *delivered* message (totals from `summary.csv` ÷ 150,000,000
messages per cell):

| impl | bytes | cap | full retries/msg | empty retries/msg |
|---|---|---|---|---|
| mutex | 8 | 1024 | 3.499 | 0.325 |
| mutex | 8 | 4096 | 0.250 | 0.185 |
| mutex | 8 | 65536 | 0.082 | 0.103 |
| mutex | 32 | 1024 | 1.795 | 0.282 |
| mutex | 32 | 4096 | 0.480 | 0.071 |
| mutex | 32 | 65536 | 0.370 | 0.004 |
| mutex | 64 | 1024 | 1.042 | 0.323 |
| mutex | 64 | 4096 | 0.355 | 0.065 |
| mutex | 64 | 65536 | 0.090 | 0.011 |
| spsc | 8 | 1024 | 0.369 | 5.279 |
| spsc | 8 | 4096 | 0.259 | 1.894 |
| spsc | 8 | 65536 | 0.135 | 5.829 |
| spsc | 32 | 1024 | 0.030 | 4.388 |
| spsc | 32 | 4096 | 0.001 | 4.072 |
| spsc | 32 | 65536 | 0.003 | 8.048 |
| spsc | 64 | 1024 | 0.025 | 4.291 |
| spsc | 64 | 4096 | 0.076 | 0.017 |
| spsc | 64 | 65536 | 0.047 | 0.019 |

The two implementations fail *differently*: the mutex baseline is dominated by
the **producer** finding the queue full, whereas the SPSC cells are dominated by
the **consumer** finding the queue empty.

---

## 5. DERIVED — comparison

Everything in this section is computed from §4.1; no new measurement was taken.
`ratio` = SPSC median ÷ mutex median, so **< 1 means SPSC completes a message
faster**.

| bytes | cap | mutex ns/msg | spsc ns/msg | ratio | mutex [min,max] | spsc [min,max] | verdict |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 54.179 | 34.682 | **0.640** | [37.71, 59.43] | [19.19, 41.57] | ranges overlap — not separable |
| 8 | 4096 | 24.080 | 18.053 | **0.750** | [23.29, 26.65] | [10.30, 39.31] | ranges overlap — not separable |
| 8 | 65536 | 21.239 | 34.624 | **1.630** | [19.87, 24.48] | [22.57, 40.77] | ranges overlap — not separable |
| 32 | 1024 | 40.340 | 38.109 | **0.945** | [36.17, 43.95] | [28.57, 43.86] | ranges overlap — effectively tied |
| 32 | 4096 | 25.295 | 30.862 | **1.220** | [23.88, 27.57] | [27.94, 35.35] | **separated: mutex faster** |
| 32 | 65536 | 22.733 | 48.373 | **2.128** | [21.31, 23.67] | [44.70, 53.29] | **separated: mutex faster** |
| 64 | 1024 | 45.194 | 40.325 | **0.892** | [41.01, 53.24] | [36.73, 42.84] | ranges overlap — not separable |
| 64 | 4096 | 30.385 | 14.130 | **0.465** | [26.17, 32.72] | [13.00, 18.77] | **separated: SPSC faster** |
| 64 | 65536 | 25.367 | 14.926 | **0.588** | [23.65, 30.28] | [13.23, 19.28] | **separated: SPSC faster** |

Range separation is the conservative test used throughout: a verdict is only
called "separated" when the 15-repetition `[min, max]` intervals of the two
implementations do not overlap at all.

**Only 4 of 9 cells separate.** SPSC is unambiguously faster in 2 cells
(64 bytes at capacities 4096 and 65536, by 2.15× and 1.70×); the mutex baseline
is unambiguously faster in 2 cells (32 bytes at capacities 4096 and 65536, by
1.22× and 2.13×); the remaining 5 cells overlap and support no directional
claim, including the seemingly large `spsc 8 / 4096` and `spsc 8 / 1024` wins,
which are produced by a bimodal distribution rather than by a consistent
advantage.

---

## 6. INTERPRETATION

### 6.1 Answers to the Phase-2 questions

**How much faster or slower is SPSC than mutex, per message size?**
There is no consistent answer, and that is the result. Pooled medians span
0.465× (SPSC 2.15× faster) to 2.128× (mutex 2.13× faster); SPSC has the lower
median in 6 of 9 cells, but only 2 of those 6 survive the non-overlapping-range
test, while 2 cells separate in the opposite direction. The honest summary is
that **on this host, at this message count, the frozen unpadded SPSC protocol
does not dominate a plain mutex queue, and a mutex queue does not dominate it
either** — the outcome is cell-dependent and, in over half the matrix, not
resolvable above the measurement's own spread.

**Does message size materially change relative performance?**
Yes, and in a direction that resists a simple story. SPSC's two clear wins are
both at 64 bytes; its two worst relative losses are at 8 and 32 bytes. Larger
payloads being *relatively* cheaper for the lock-free queue is not what a pure
per-byte copy-cost model predicts, which is itself a reason to be cautious: the
64-byte cells are also the ones where a slot is exactly one cache line, so the
payload-size axis is confounded with the slot-to-cache-line mapping. Phase 2
does not separate those two effects (§7.2).

**Does capacity materially change throughput?**
For the mutex baseline, clearly yes at the small end: capacity 1024 is
1.77–2.55× slower than the better of the two larger capacities at every message
size (1.77× at 32 bytes, 1.78× at 64 bytes, 2.55× at 8 bytes), consistent with a
small ring keeping the lock hot and the producer blocked on a full queue
(§4.3: 1.04–3.50 full retries per message at capacity 1024 versus ≤0.48
elsewhere). For SPSC there is no monotone capacity story at all — capacity 4096
is its best cell at 8 and 64 bytes and its second-worst at 32 bytes, and no
capacity ordering is consistent across message sizes.

**Which queue experiences more retries?**
They fail in opposite directions (§4.3). The mutex baseline is producer-side
backpressure-dominated (full-queue retries exceed empty-queue retries in 8 of 9
cells, by up to two orders of magnitude); SPSC is consumer-side
starvation-dominated (empty-queue retries exceed full-queue retries in 7 of 9
cells). SPSC's producer almost never finds the queue full (0.001–0.369
retries/message); its consumer frequently finds it empty (1.894–8.048
retries/message, except the two 64-byte cells at 0.017/0.019).

**Are any results unstable enough that no strong conclusion should be drawn?**
Yes — 5 of 9 cells, and the instability is concentrated in SPSC. The two most
extreme spreads are both SPSC (281.6% and 116.7%), and the per-session table
(§4.2) shows those spreads are largely *between processes*, not between
repetitions. Any Phase-2 conclusion drawn from a single SPSC launch of those
cells would be an artifact of where the OS happened to place the two threads.

### 6.2 What Phase 2 explicitly does not claim

- **No false-sharing attribution.** The unpadded `head_`/`tail_` layout permits
  and is likely to exhibit false sharing, and the SPSC cells' coherence-heavy
  behaviour is consistent with it. But Phase 2 contains no packed/separated
  control, no address verification, and no cache-line instrumentation, so it
  cannot distinguish false sharing from ordinary coherence traffic, from thread
  placement, or from the retry loop. **Every difference in §5 is left
  unattributed**, and false sharing is named only as a Phase-3 hypothesis.
- **No claim that SPSC is or is not "faster than a mutex".** The measurement
  answers a narrower question — this code, this host, this message count — and
  the answer is "it depends on the cell".
- **No extrapolation to other platforms.** These are macOS/Apple-Silicon
  numbers with an unpinned scheduler (§7.1).
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
- Threads may be **migrated** between cores within a repetition, and the
  per-session table (§4.2) shows the consequence: identical binaries, identical
  inputs, different placements, up to ~2× different medians.
- **Frequency** is managed by the OS (DVFS, thermal and power state) and is not
  read or controlled here.
- **System load** is not isolated: no attempt is made to quiesce the machine,
  disable background daemons, or reserve cores, so other work on the host can
  influence a cell.
- The harness does **yield** on long retry streaks (§3.2). That is a harness
  policy applied identically to both implementations, but it does interact with
  the scheduler and is therefore part of the measurement, not outside it.

A consequence worth stating plainly: **these numbers characterize this machine
under ordinary conditions, not a controlled real-time environment.** Their
reproducibility is bounded by that, which is precisely why 15 pooled
repetitions, a reported spread, and a non-overlapping-range test are used before
any directional claim is made.

### 7.2 Confounds the matrix does not break

- **Slot-to-cache-line mapping** moves with message size (8-byte slots pack 8
  per line, 64-byte slots occupy a whole line), so the message-size axis is not
  a clean "payload cost" axis at 64 bytes.
- **Ring working-set size** moves with capacity (1024 × 64 B = 64 KiB vs
  65536 × 64 B = 4 MiB), so the capacity axis is not a clean "queue length"
  axis either.
- **The two implementations are not the same shape of code.** The mutex
  baseline's failed attempt takes and releases a lock; SPSC's failed attempt is
  a relaxed-plus-acquire load pair. Retry counts (§4.3) are therefore not
  directly comparable as "work", only as behaviour.

### 7.3 The retry loop is inside the measurement

Both legs measure a *saturating producer against a consumer that spins* — the
producer never sleeps, and the consumer pays for every failed `try_pop`. A
starved consumer therefore inflates end-to-end time directly: at 8.048 empty
retries per message (`spsc 32 / 65536`, the slowest SPSC cell), a substantial
part of the measured 48.4 ns/msg is failed-attempt overhead rather than transfer
work. This is a property of the chosen backpressure policy interacting with the
queue, not a "pure" handoff cost, and it applies to both implementations.

### 7.4 The baseline is a plain mutex, not a tuned one

`MutexBoundedQueue` is deliberately unoptimized (one `std::mutex`, no
condition-variable handoff, no lock-free fast path). Its retry path takes the
lock on every failed attempt. Conclusions are therefore about *this* baseline;
a tuned lock-based queue could narrow or reverse the separated cells, and Phase
2 does not attempt to establish where the mutex's ceiling is.

### 7.5 Scope of the dataset

One host, one toolchain, one build (`-O3 -DNDEBUG`, Apple clang 15.0.0), one
message count (10,000,000), one machine state per launch. No cross-platform
comparison, no cross-compiler comparison, and no claim that 10,000,000 messages
is enough to have reached a true steady state on every cell.

---

## 8. Phase status

- Experiment 01 — COMPLETE / FROZEN.
- **Experiment 02, Phase 1 — Correctness / Memory Model: COMPLETE / FROZEN.**
- **Experiment 02, Phase 2 — Throughput Baseline: COMPLETE / FROZEN.**
  Canonical dataset: `docs/results/spsc-throughput/` (15 pooled repetitions per
  cell over 3 independent processes). Superseded first pass, retained as a
  labeled artifact:
  `docs/results/spsc-throughput-superseded-single-session/`.
- **Experiment 02, Phase 3 — False Sharing / Cursor Caching: NOT STARTED.**
- **Experiment 02, Phase 4 — Tail Latency: NOT STARTED.**

Phase 3 will introduce the controlled variables Phase 2 deliberately held
fixed — packed same-line versus separated/padded cursors, verified by actual
cursor addresses and cache-line placement, plus remote-cursor caching — and will
re-measure against this baseline. No Phase-2 number above may be cited as
evidence for or against false sharing in the Phase-1 layout.
