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
reading the message that currently occupies it. Without an ordering guarantee,
the producer could advance `head_` (making room) and then write into a slot the
consumer is still reading — again a data race.

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

The implementation (`include/spsc_ring_buffer.h`) uses four loads/stores per
transfer. Three are on the thread's **own** cursor; the fourth is the acquire
gate on the **remote** cursor.

| Operation | Ordering | Owner of the cursor | Why |
|-----------|----------|---------------------|-----|
| `head_.load()` (push) | `relaxed` | producer (self) | own write cursor; see §3.1 |
| `tail_.load()` (push) | `acquire` | consumer (remote) | gate: slot reusable only after consumer released it (§1.2) |
| `head_.store()` (push) | `release` | producer (self) | publish the just-written payload (§1.1) |
| `tail_.load()` (pop) | `relaxed` | consumer (self) | own read cursor; see §3.2 |
| `head_.load()` (pop) | `acquire` | producer (remote) | gate: data present only after producer published it (§1.1) |
| `tail_.store()` (pop) | `release` | consumer (self) | release the just-read slot (§1.2) |

`empty()` uses two relaxed loads and is documented as a snapshot-only helper
(§5). There is **no** `seq_cst`, **no** `compare_exchange`, **no** fence, and
no `volatile`.

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

### 3.3 Why every acquire load actually matches a release store

A subtlety worth stating explicitly: synchronizes-with requires the acquire
load to read the value *of* a release store (or of a later member of its
release sequence). Here it always does, because **each cursor has a single
writer and every store that writer makes is a release store.** `head_` is
stored only by the producer and always with `release`; `tail_` is stored only
by the consumer and always with `release`. Whatever value an acquire load
reads from a remote cursor was written by that remote thread's release store,
so the acquire synchronizes-with that store. (Monotonicity also means reading a
*newer* value than the minimum the guard needs only orders *more* prior payload
work, never less.)

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

Both cursors are `std::size_t` (`uint64_t` on this platform) and increase
monotonically by 1 per successful push/pop. The producer never gets more than
`Capacity` ahead of the consumer (it refuses to push at exactly `Capacity`), so
as ordinary integers `0 ≤ head - tail ≤ Capacity` always holds.

**Wraparound.** The counters are allowed to wrap past `2^64`. Two facts keep
everything correct while they do:

1. **Slot indexing needs only the low bits.** `slot = counter & (Capacity - 1)`
   is `counter mod Capacity`, and `Capacity` is a power of two. Any two counters
   congruent mod `Capacity` address the same physical slot, so the mask remains
   correct no matter what the high bits do.
2. **The full/empty difference stays exact mod `2^64`.** Because
   `0 ≤ head - tail ≤ Capacity` and `Capacity` is tiny compared with `2^64`,
   `head` and `tail` never differ by a full wrap: their low 64-bit images
   satisfy `(head - tail) mod 2^64 == head - tail`. The unsigned subtraction
   in the full check therefore never silently borrows.

We do **not** claim to have run `2^64` operations (that is ~1.8×10^19 pushes —
outside any practical run, including the stress test). The reasoning above is
the documented correctness argument for the wrap assumption, which is exactly
the standard one for a single-writer/single-reader monotonic-counter design.

---

## 5. Ownership model, threading contract, and unsupported uses

**Ownership.** `head_` is the producer's write cursor; the producer is the ONLY
thread that ever stores to it. `tail_` is the consumer's read cursor; the
consumer is the ONLY thread that ever stores to it. Each thread *reads* the
other's cursor, but only as a non-destructive hint. Because a cursor has one
writer, the next value is always `last value + 1` known only to that writer —
there is no "claim the slot" arbitration, which is what makes CAS unnecessary.

**Threading contract (documented, not enforced by runtime locks).** Behavior is
undefined if:

- multiple producers call `try_push` concurrently, or
- multiple consumers call `try_pop` concurrently, or
- one thread mixes producer and consumer roles concurrently with a counterpart
  (this is two writers to one cursor, which violates single ownership).

The queue adds no locks or runtime checks to catch violations — enforcing the
contract at runtime would defeat its purpose. Violations are a programming
error, detected only by discipline (or by TSan if the violation actually races).

**`empty()` is a hint, not a synchronization primitive.** It loads both cursors
relaxed, so it is exact only for a single caller on one side. Between the two
threads it is a point-in-time snapshot: the remote cursor may move immediately
after the load. Cross-thread control flow must use the `try_push`/`try_pop`
return values (which perform the real acquire-gated checks), never `empty()`.

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
  unpadded, so the two threads will contend on a shared cache line. That packed
  layout is the intentional Phase-1 baseline; Phase 3 will compare it against a
  separated/padded layout as a controlled experiment. Padding now would destroy
  that control.
- **No remote-cursor cache.** Every call loads both cursors. Caching the remote
  cursor (`cached_tail` / `cached_head`) is a separate, later, controlled
  optimization (Phase 3), because it trades freshness for fewer cache misses and
  must be measured, not assumed.
- **No raw storage / placement-new.** Payloads live in `std::array<T, Capacity>`
  and are moved/copied with ordinary assignment. This keeps object lifetimes
  trivial at the cost of requiring `T` to be default-constructible and
  assignable — an acceptable Phase-1 simplification for message types.
- **No benchmarks, no throughput claims.** Correctness first.

Nothing in this document is a performance claim. Phase 2 measures throughput;
Phase 3 isolates false sharing and cursor caching; Phase 4 measures tail
latency.
