# Experiment 02 — SPSC Ring Buffer: Memory Model (Phase 1)

**Scope.** This document is the correctness/memory-model argument for
`include/spsc_ring_buffer.h` (Experiment 02, Phase 1). It explains *why* the
chosen memory orders are exactly what the protocol needs, *what* each ordering
guarantees, and *how* the pieces compose into a happens-before chain. It does
not measure anything (Phase 2) and does not claim the design is the fastest
possible layout (Phases 3–4).

The system model:

```
one producer thread  ──►  bounded SPSC queue  ──►  one consumer thread

Feed / Decoder Thread ──►  SPSC  ──►  OrderBook / Strategy Thread
```

Research question for Phase 1:

> How can one producer and one consumer safely transfer messages **without
> mutexes** while using the **minimum required atomic synchronization**?

Answer in one line: with each cursor owned by exactly one thread, two
release/acquire pairs — producer publishing a payload, consumer releasing a
slot — are both necessary and sufficient; everything else is relaxed or plain.

---

## 1. WHY — what the protocol must actually order

There are exactly two cross-thread data dependencies in an SPSC ring buffer,
and every memory-order decision below exists to serve one of them.

### 1.1 Producer → consumer: payload publication

The consumer may read `slots_[i]` **only after** the producer has finished
writing the message into that slot. Without an ordering guarantee, the consumer
could observe `head_` advance and then read a slot whose payload store has not
yet become visible — a data race on `slots_[i]`.

```
producer                          consumer
────────                          ────────
slots_[i] = msg        (write payload)
head_.store(h+1, REL)             head_.load(ACQ)          (sees h+1)
                                  out = move(slots_[i])    (read payload)
```

### 1.2 Consumer → producer: slot reuse

The producer may overwrite `slots_[j]` **only after** the consumer has finished
reading the message that currently occupies it. The slot becomes reusable
because of what the **consumer** does: the consumer finishes its payload read
and then performs `tail_.store(t+1, release)`, which announces that the slot is
free. The producer does not make a slot reusable by advancing `head_` — it only
*learns* that a slot is reusable by observing the consumer's advanced `tail_`
with `tail_.load(acquire)`. Without that edge the producer could overwrite a
slot the consumer is still reading — again a data race.

```
consumer                          producer
────────                          ────────
out = move(slots_[j])  (read payload)
tail_.store(t+1, REL)             tail_.load(ACQ)          (sees t+1 >= reuse point)
                                  slots_[j] = next_msg     (overwrite old occupant)
```

These are the only edges that matter. Nothing else in the design requires
cross-thread ordering.

---

## 2. WHAT — the chosen memory orders

The implementation (`include/spsc_ring_buffer.h`) performs **six cursor atomic
operations per complete producer → consumer message transfer**: three in a
successful `try_push` (own `head_` load relaxed, remote `tail_` load acquire,
own `head_` store release) and three in a successful `try_pop` (own `tail_`
load relaxed, remote `head_` load acquire, own `tail_` store release). Within
each operation two are on the thread's **own** cursor and one is the acquire
gate on the **remote synchronization cursor**. That count is the motivation for
the *deferred* remote-cursor-cache experiment (§7); it is deliberately not
reduced here.

| Operation | Ordering | Owner of the cursor | Why |
|-----------|----------|---------------------|-----|
| `head_.load()` (push) | `relaxed` | producer (self) | own write cursor; see §3.1 |
| `tail_.load()` (push) | `acquire` | consumer (remote sync cursor) | correctness gate: slot reusable only after consumer released it (§1.2) |
| `head_.store()` (push) | `release` | producer (self) | publish the just-written payload (§1.1) |
| `tail_.load()` (pop) | `relaxed` | consumer (self) | own read cursor; see §3.2 |
| `head_.load()` (pop) | `acquire` | producer (remote sync cursor) | correctness gate: data present only after producer published it (§1.1) |
| `tail_.store()` (pop) | `release` | consumer (self) | release the just-read slot (§1.2) |

`empty()` performs two relaxed loads and is an advisory observation only (§5).
There is **no** `seq_cst`, **no** `compare_exchange`, **no** fence, and no
`volatile`.

The remote loads are **correctness synchronization gates**, not hints: the
producer's `tail_.load(acquire)` gates safe slot reuse, and the consumer's
`head_.load(acquire)` gates safe payload consumption. ("Cached hint" is
reserved for the not-yet-implemented remote-cursor cache, where a *stale* remote
value is deliberately retained for speed.)

---

## 3. HOW — the happens-before argument, step by step

### 3.1 Why own-cursor loads are relaxed

`head_` is written **only** by the producer; `tail_` is written **only** by the
consumer (the threading contract in §6). A thread that loads its *own* cursor
therefore never races: the value it reads is the last value it itself stored,
ordered by ordinary program order plus the write-read coherence guarantee of
the C++ memory model (a same-thread store is visible to a later same-thread
load of the same atomic). No *other* thread's data depends on that load — the
producer does not need an acquire to read `head_` because nothing the consumer
wrote is being consumed at that point; and vice versa. A relaxed load is the
minimum that keeps the load atomic and the read from being cached away
indefinitely, and it is sufficient.

### 3.2 Why remote-cursor loads are acquire

The two gates (§1.1, §1.2) read the *remote* cursor, and each gate protects a
non-atomic payload access whose counterpart happened on the remote thread.
Reading the remote cursor must therefore **acquire** the remote thread's
matching **release** so that the payload access is ordered after the remote
thread's prior payload access.

**Producer publication (consumer side).** The consumer's `head_.load(acquire)`
reads the value `h` that the producer stored with `head_.store(h, release)`
when it published message `h-1`. All of the producer's payload writes for
messages `0 … h-1` are sequenced-before that release store; the acquire load
synchronizes-with it; therefore those payload writes happen-before the consumer
proceeds to read `slots_[tail & mask]`. Message `tail` is among `0 … h-1`
(empty check guarantees `tail < h`), so its payload write is visible.

**Slot reuse (producer side).** The producer needs to overwrite
`slots_[head & mask]`, whose previous occupant was written when the producer's
counter was `head - Capacity` (same slot: `(head - Capacity) & mask ==
head & mask`, since `Capacity` is a power of two). That occupant may be
overwritten only once the consumer has read it. The consumer's
`tail_.store(t+1, release)` is sequenced-after its read of that slot's payload,
and the producer's `tail_.load(acquire)` synchronizes-with it (the push guard
`head - tail < Capacity` ensures the value read is ≥ `head - Capacity + 1`,
i.e. the consumer has already passed that occupant). Hence the consumer's read
of the old occupant happens-before the producer's overwrite.

### 3.3 What the acquire loads do — and do not — synchronize with

`synchronizes-with` requires the acquire load to read the value *of* a release
store (or of a later member of that release store's release sequence). Two
consequences follow, and the second is easy to overstate:

- **When a gate load returns a value that permits the access, the required edge
  exists.** On the consumer path, if `head_.load(acquire)` returns a
  sufficiently advanced value that lets the consumer read message `t`, that
  published `head_` value was written by the producer's release publication, and
  the acquire synchronizes-with it — establishing the payload happens-before
  edge. On the producer reuse path, when the producer is reclaiming an
  already-consumed slot, the sufficiently advanced `tail_` value that permits
  the reuse was written by the consumer's release store, and the acquire
  synchronizes-with it — establishing the reuse happens-before edge.
- **It is NOT the case that every acquire load unconditionally synchronizes
  with a remote release store.** A cursor's initial value `0` is not a release
  store performed by the opposite thread, and a gate load that returns that
  initial (or any not-yet-advanced) value performs no synchronization at all —
  it simply fails the gate and the caller takes the full/empty path.
- **The first use of a never-before-consumed slot needs no consumer → producer
  reuse edge.** Until a slot has held a message there is nothing for the
  consumer to release, so the reuse edge is required only for slots a previous
  message already occupied; the publication edge (§1.1) is what makes the first
  write safe to read.

A stale remote read is **conservative, never unsafe**: a stale `head_` can only
cause a *false empty* (the consumer declines to read data that is in fact
published), and a stale `tail_` can only cause a *false full* (the producer
declines to reuse a slot that is in fact free). Neither can permit an access
that the ordering does not already cover — the gate either returns a value that
carries the required edge, or it fails.

### 3.4 The composed happens-before for one message

Let message `m` be pushed, then popped. In program order within each thread,
with SWS edges marked `sb` and the single synchronizes-with edge marked `sw`:

```
Producer:                         Consumer:
slots_[m & mask] = payload        head_.load(acquire)      -- reads m+1
    │ sb                              │ sb
head_.store(m+1, release)  ──sw──►    │
                                       out = move(slots_[m & mask])   [payload read]
                                            │ sb
                                       tail_.store(m+1, release)
```

`slots_[m & mask] = payload` happens-before `out = move(slots_[m & mask])`
via `sb → release → sw → acquire → sb`. The consumer then releases the slot;
the *next* producer write to that slot (when its counter reaches `m + Capacity`)
happens-before-safe via the symmetric chain. No relaxed-only configuration can
give either chain: relaxing the gate loads removes the `sw` edges and leaves the
payload read/write racing.

---

## 4. Full / empty and counter wrap-around

With monotonic counters the two states are disjoint on the **counter domain**,
so the full capacity is usable — no slot is sacrificed to disambiguate states:

```
empty:  head_ == tail_
full:   head_ - tail_ == Capacity
```

Both cursors are **unsigned `std::size_t` monotonic counters** — 64-bit on the
canonical 64-bit development hosts, but nothing below depends on that width.
They increase monotonically by 1 per successful push/pop. The producer never
gets more than `Capacity` ahead of the consumer (it refuses to push at exactly
`Capacity`), so as ordinary integers `0 ≤ head - tail ≤ Capacity` always holds.

**Wraparound.** The counters are allowed to wrap modulo `2^W`, where `W` is the
width of `std::size_t`. The reasoning is generic to `W`; it does not assume 64.
Two facts keep everything correct while the counters wrap:

1. **Slot indexing needs only the low bits.** `slot = counter & (Capacity - 1)`
   is `counter mod Capacity`, and `Capacity` is a power of two. Any two counters
   congruent mod `Capacity` address the same physical slot, so the mask remains
   correct no matter what the high bits do.
2. **The full/empty difference stays exact modulo `2^W`.** Because
   `0 ≤ head - tail ≤ Capacity` and `Capacity` is tiny compared with `2^W`,
   `head` and `tail` never differ by a full wrap: their `W`-bit images satisfy
   `(head - tail) mod 2^W == head - tail`. The unsigned subtraction in the full
   check therefore never silently borrows.

We do **not** claim to have run `2^W` operations (for `W = 64` that is ~1.8×10^19
pushes — outside any practical run, including the stress test). The reasoning
above is the documented correctness argument for the wrap assumption, which is
the standard one for a single-writer/single-reader monotonic-counter design.

---

## 5. Ownership model, threading contract, and unsupported uses

**Ownership.** `head_` is the producer's write cursor; the producer is the ONLY
thread that ever stores to it. `tail_` is the consumer's read cursor; the
consumer is the ONLY thread that ever stores to it. Each thread *reads* the
other's cursor as a non-destructive **remote synchronization cursor** — a
correctness gate (§3.3), never a mere hint. Because a cursor has one writer, the
next value is always `last value + 1` known only to that writer — there is no
"claim the slot" arbitration, which is what makes CAS unnecessary.

**Threading contract (documented, not enforced by runtime locks).** Behavior is
undefined if:

- multiple producers call `try_push` concurrently, or
- multiple consumers call `try_pop` concurrently, or
- one thread mixes producer and consumer roles concurrently with a counterpart
  (this is two writers to one cursor, which violates single ownership).

The queue adds no locks or runtime checks to catch violations — enforcing the
contract at runtime would defeat its purpose. Violations are a programming
error, detected only by discipline (or by TSan if the violation actually races).

**`empty()` is an advisory observation, not a synchronization primitive.** It
performs two independent relaxed loads, so it is **not a coherent pair-snapshot**
of `head_`/`tail_`: for the designated producer or consumer the local cursor is
current but the remote one may already be stale at the moment the local one is
loaded, and a third (observer) thread is guaranteed no coherent pair at all. It
can be useful diagnostically, and it is safe — a stale remote value can only look
falsely empty/full, never permit an unsafe access — but cross-thread control flow
must use the `try_push`/`try_pop` return values, which perform the real
acquire-gated checks.

---

## 6. Why not the alternatives

### 6.1 Why not `volatile`

`volatile` is not thread synchronization in C++. It tells the compiler "this
object may change outside this thread's view, so do not elide or reorder
accesses to it" — a code-generation constraint aimed at memory-mapped I/O and
signal handlers. It provides **none** of the ordering the protocol needs:

- It does not prevent the CPU from reordering a `volatile` payload write after
  the `volatile` cursor store (no acquire/release semantics; on many ISAs it
  compiles to a plain load/store with no barrier).
- It gives no cross-core visibility guarantee; another core may still see the
  stores in the other order.
- The C++ standard says data races on non-atomic objects (even `volatile` ones)
  are undefined behavior.

`std::atomic` with explicit memory order is the correct tool: it both prevents
compiler reordering and emits the required hardware barriers.

### 6.2 Why SPSC needs no CAS

`compare_exchange` (or any read-modify-write) exists to arbitrate when **two or
more threads race to claim the same resource** — e.g., multiple producers each
trying to take the next free slot, where the answer "who got it" is not known in
advance. In an SPSC queue each cursor has exactly one writer, so the next
counter value is always `its last value + 1`, known without asking anyone. The
producer never contends for `head_`; the consumer never contends for `tail_`.
There is nothing to arbitrate, so CAS would add cost (an RMW bus operation) and
complexity for zero benefit. MPMC designs need CAS (or a lock) precisely because
they *do* have that race; the SPSC contract removes it by construction.

### 6.3 Why `seq_cst` is stronger than necessary

`seq_cst` orders every `seq_cst` operation into **one total order across the
entire system**, consistent across all threads, and gives each such operation
full acquire *and* release semantics. The SPSC protocol needs only two
one-directional edges (producer→consumer payload publication; consumer→producer
slot release), each delivered by a single release store matched with a single
acquire load on the far side. A total order over unrelated atomics elsewhere in
the program is not needed for correctness, and on weakly-ordered hardware (ARM,
the architecture family of the development machine) `seq_cst` loads/stores
compile to stronger instructions/barriers than their acquire/release
counterparts, adding latency to the hot path for no correctness benefit.

Relaxed-only cross-thread publication is likewise **not** claimed correct here
— §3.4 shows the acquire/release edges are load-bearing. The minimum is exactly:
relaxed on your own cursor, acquire on the remote cursor at the gate, release on
your own cursor when you publish/release a payload.

---

## 7. What Phase 1 deliberately does NOT do

To keep the baseline easy to reason about, and so that later phases are clean
controlled comparisons rather than confounded rewrites:

- **No false-sharing isolation.** `head_` and `tail_` are adjacent, packed, and
  unpadded, so the layout **permits and is likely to exhibit** false sharing —
  but adjacency does not *guarantee* that the two objects share one physical
  cache line for every object address and platform (alignment, object placement,
  and cache-line size all matter). That unpadded layout is the intentional
  Phase-1 baseline. Phase 3 will run a controlled experiment that explicitly
  verifies cursor addresses / cache-line placement for a packed same-line
  control and a separated/padded control. Padding now would destroy that
  control.
- **No remote-cursor cache.** Every call loads both cursors. Caching the remote
  cursor (`cached_tail` / `cached_head`) is a separate, later, controlled
  optimization (Phase 3), because it trades freshness for fewer cache misses and
  must be measured, not assumed.
- **No raw storage / placement-new.** Payloads live in `std::array<T, Capacity>`
  and are moved/copied with ordinary assignment. This keeps object lifetimes
  trivial at the cost of requiring `T` to be default-constructible and
  assignable — an acceptable Phase-1 simplification for message types.
- **No benchmarks, no throughput claims.** Correctness first.

---

## 8. Lock-free scope: what is and is not claimed

C++ does not require `std::atomic<std::size_t>` to be lock-free on every
platform, so `SpscRingBuffer` states the requirement explicitly:

```cpp
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "Experiment 02 requires a platform where "
              "std::atomic<std::size_t> is always lock-free: ...");
```

Where that assertion fails, the experiment does not compile — deliberately.
**No mutex fallback is provided**: silently substituting a lock would hide the
exact property under study and would invalidate any later measurement. Where the
assertion holds, the **cursor protocol** (relaxed / acquire / release on `head_`
and `tail_`) is lock-free on supported platforms.

That claim is about the cursor protocol only. `T`'s own copy/move assignment is
arbitrary user code and is **not** claimed to be lock-free, non-blocking, or
allocation-free (see §9).

## 9. Allocation and exception guarantees

**Allocation.** The queue's storage is preallocated — `std::array<T, Capacity>`
is a member of the buffer — and the queue implementation performs no allocator
calls in the hot path. That is *not* the same as "a transfer never allocates":
`T`'s copy/move assignment is user code and may allocate, throw, or block. Phase 2
benchmark message types will be fixed-size, nothrow, allocation-free value types,
so that what is measured is the queue protocol rather than the payload type.

**Exceptions.** The queue's own machinery does not throw for nothrow-assignable
`T`. If `T`'s assignment does throw:

- the cursor is **not** advanced, so the queue stays structurally consistent — a
  failed `try_push` publishes no message, and a failed `try_pop` releases no slot;
- the **value** guarantee is deliberately weak, *not* strong. `try_pop` moves the
  stored payload into the caller's `out` as an rvalue, so a throwing move
  assignment may already have partially modified / moved-from the stored payload.
  The message is still not popped (`tail_` unchanged), but the original stored
  value is **not** promised to be recoverable. `try_push` can likewise leave the
  target slot partially assigned.

No rollback machinery is provided on this path. Intended low-latency message types
are nothrow copy/move-assignable fixed-size values, for which none of this
arises. The same weak value guarantee applies to `MutexBoundedQueue` — whose
`try_*` methods are additionally **neither noexcept nor non-blocking**: acquiring
the mutex may block under contention, and `std::mutex::lock()` may throw
`std::system_error`. `try_*` never waits for the queue *state* to change, but
mutex acquisition itself may block.

## 10. Phase-1 validation commands

Reproducible checks for this phase. Run from the repository root. None of these
*prove* memory-model correctness — sanitizers and tests can find real defects but
cannot establish that a protocol is correct for all executions; the argument in
§3 is what carries that weight.

```sh
# 1. Clean Release build + full suite (Experiment 01 + Experiment 02)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# 2. Strict Clang warnings on the Experiment 02 translation unit
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion \
        -Werror -Iinclude tests/spsc_ring_buffer_tests.cpp -o /tmp/spsc_strict
/tmp/spsc_strict

# 3. AddressSanitizer (macOS: leak detection is unsupported -> detect_leaks=0)
clang++ -std=c++20 -O1 -g -fsanitize=address -Iinclude \
        tests/spsc_ring_buffer_tests.cpp -o /tmp/spsc_asan
ASAN_OPTIONS=detect_leaks=0 /tmp/spsc_asan

# 4. UndefinedBehaviorSanitizer (abort on first UB)
clang++ -std=c++20 -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
        -Iinclude tests/spsc_ring_buffer_tests.cpp -o /tmp/spsc_ubsan
/tmp/spsc_ubsan

# 5. ThreadSanitizer (where supported by the toolchain/platform)
clang++ -std=c++20 -O1 -g -fsanitize=thread -Iinclude \
        tests/spsc_ring_buffer_tests.cpp -o /tmp/spsc_tsan
/tmp/spsc_tsan
```

TSan availability is toolchain- and platform-dependent; on some Apple toolchain
configurations it is unavailable or unsupported. **If TSan cannot be built or run
on a given host, that is reported as such** rather than claimed as a pass. Where
it does run, a clean report is evidence against data races in the executions
actually exercised — it is not proof of lock-free correctness and not a
substitute for the §3 argument.

---

Nothing in this document is a performance claim. Phase 2 measures throughput;
Phase 3 isolates false sharing and cursor caching; Phase 4 measures tail
latency.
