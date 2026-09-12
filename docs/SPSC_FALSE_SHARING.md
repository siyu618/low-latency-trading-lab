# Experiment 02 — SPSC Ring Buffer: Controlled Cursor Placement (Phase 3A)

**Scope.** This document is the *measurement* counterpart to
`docs/SPSC_MEMORY_MODEL.md` (Phase 1, *why the protocol is correct*) and
`docs/SPSC_THROUGHPUT.md` (Phase 2, *how fast the frozen protocol is*). Phase 3A
asks a narrower question than either: of the throughput behaviour observed in
Phase 2, how much changes when the producer-owned `head` cursor and the
consumer-owned `tail` cursor are **forced to occupy the same cache line** versus
**forced onto distinct cache lines** — and nothing else is changed at all.

The project name for this phase remains *Controlled False Sharing* for
continuity. **The causal language in this document is deliberately more precise
than that name.** Cursor placement is a *coherence-layout* variable, and §1.3
explains why a same-line/separated throughput difference is a measurement of
**controlled cursor placement**, not of "pure false-sharing cost": separating the
cursors removes line-granularity interference between two independent cursor
writes **and** gives up the colocation of two genuinely shared synchronization
values. Phase 3A has no hardware counters, so it cannot decompose the two. The
phrasing used throughout is therefore *"separated-faster is consistent with
reduced false-sharing interference"* — never *"false sharing was proven to cause
the full measured difference."*

**Phase 3A changes ONE program-layout treatment: cursor cache-line placement —
and that requires an equal footprint.** "One variable" means the two queue
implementations differ in exactly one intentional program-layout treatment. It
does **not** mean the two legs' processes are identical in every respect: each
leg runs as an independent process with its own independently allocated queue
object, so absolute addresses, scheduler placement, DVFS and thermal state, and
background-system state all differ and are **not** eliminated by construction.
Those nuisance variables are handled by the design — adjacent process pairing,
balanced AB/BA ordering, four repeated sessions — and by the four-session
directional-stability criterion (§3.4, §4.4), not by construction. There is no
cached remote cursor, no batching, no memory-order change, no CAS, and no
affinity anywhere in this phase.
Remote-cursor caching is Phase 3B and is deliberately **not** combined with the
layout change, because an experiment that moves two things at once attributes the
result to neither. By the same logic, the two cursor policies must have the same
size, so that the payload array after them keeps the same relative offset within
the object when the cursors move (§2.2, §2.7). An earlier same-line policy was
128 bytes against the separated policy's 256, which shifted the payload's offset
by 128 bytes at the same time as it changed cursor placement — two object-layout
changes at once; that dataset is archived and superseded (§7.5).

**Status: Phase 3A — Controlled Cursor Placement: COMPLETE / FROZEN.**
The equal-footprint canonical dataset was collected 2026-09-12 and verified
against every invariant in §3.8 (72 processes, 360 measured repetitions, all
correctness and layout checks passing, equal `object_size` and equal
`payload_offset` across the two layouts in all 9 cells). Canonical dataset:
`docs/results/spsc-false-sharing/`. The superseded pre-3A.1 dataset is retained
unedited, and is not to be cited, at
`docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/`.

**The frozen code stays frozen.** `include/spsc_ring_buffer.h` — the unpadded
Phase-1/2 implementation — was not modified. It remains the historical
*natural* baseline and is **not** one of the two Phase-3A controls; §2.4
explains why it cannot be one.

---

## 1. WHY — the question Phase 3A is allowed to ask

### 1.1 Research question

*How much of the SPSC's throughput behaviour changes when the producer-owned
head cursor and the consumer-owned tail cursor are forced to share one cache
line, compared with the same cursors on distinct cache lines, holding the
algorithm, the payload, the payload's position in the object, the memory orders,
the harness and the machine constant?*

That is the whole question. Phase 3A does not ask "how fast is a padded queue",
does not ask "what is the best SPSC", and does not ask whether caching the remote
cursor helps. Those are different experiments.

### 1.2 What false sharing actually is

In the frozen SPSC, the producer is the **only** writer of `head` and the
consumer the **only** writer of `tail`. Neither thread ever writes the other's
cursor, and neither thread ever reads the other's cursor in the same instruction
that writes its own. On that description there is no data race and no shared
mutable field — and yet the two threads can still slow each other down.

The reason is that cache coherence operates on **cache lines**, not on objects.
When both cursors land in one line:

1. The producer publishes a message with a release store to `head`. This
   requires exclusive ownership of the line.
2. That line also holds `tail` — which the **consumer** owns and had cached.
3. The consumer's next access to its own `tail` (a relaxed load of its own
   cursor, an operation that logically has nothing to do with the producer)
   finds the line invalidated and must fetch it again.
4. Symmetrically, the consumer's release store to `tail` evicts the producer's
   `head`.

Both threads therefore pay coherence traffic for a field they do not share. This
is **false sharing**: an artefact of line granularity, not of program logic.

### 1.3 False sharing vs. true sharing — and why placement is not a clean single-variable isolation of false sharing

This distinction is the one most easily lost, so Phase 3A states it explicitly.

**Separating the cursors does not remove legitimate communication, and Phase 3A
does not claim that it does.** Even with the cursors on separate lines, the
protocol still requires each thread to observe the other's cursor:

- The producer must read `tail` (acquire) before reusing a slot — the **reuse
  gate**. Without it, the producer would overwrite a payload the consumer has not
  yet read.
- The consumer must read `head` (acquire) before reading a payload — the
  **availability gate**. Without it, the consumer would read a slot the producer
  has not yet published.

Both are **required remote observations**, and they remain in **both** Phase-3A
variants, unchanged, with the same memory orders. So far so good — but this is
where the naive version of the argument goes wrong, and Phase 3A does not make
it.

Because each side both writes its own cursor and reads the other's, the two
cursors are **not purely independent write-only state**. `head` and `tail` are
also two legitimately shared synchronization values, and a coherence line that
holds both of them is a line that both threads genuinely need. Cursor placement
therefore moves two things at once, and layout alone cannot pull them apart:

| effect | what it is | same-line | separated |
|---|---|---|---|
| **A — line-granularity interference** | the producer's release store to `head` invalidates the consumer's cached `tail`, and vice versa: interference between two *independent* own-cursor **writes** | present | **removed** |
| **B — shared-cursor colocation** | `head` and `tail` are both genuinely needed by both threads; one line holding both may *reduce* the coherence working set — a legitimate, required-sharing effect | present | **given up** |
| **Required remote observation** | the two acquire loads at the gates (above) — correctness, not a cost to be removed | present | present |

Separating the cursors removes **A**. It also removes **B**, which may have been
helping. The two required acquire loads are identical in both variants and are
not in question.

**Therefore a same-line vs separated wall-clock difference is not a measurement
of "pure false-sharing cost".** It measures *controlled cursor placement* — the
net effect of removing A while giving up B, on one particular host and protocol.
A **separated-faster** result is *consistent with* reduced false-sharing
interference; it does not establish that false sharing caused the whole measured
gap, and conversely a **same-line-faster** result does not falsify the existence
of false sharing — it is what a case where B outweighs A looks like. The
attribution rule in §6.1 is written to match this, and Phase 3A has no
hardware-counter evidence that would decompose the two.

### 1.4 What Phase 3A deliberately does not claim

- It does not claim the frozen Phase-1 layout *exhibits* false sharing. Phase 2
  verified no cursor addresses at all, so it cannot support that claim (see
  §2.4). Phase 3A measures two new, verified layouts.
- It does not claim padding is free, and — because of the equal-footprint control
  (§2.7) — it does not *measure* the memory cost of padding either. Both controls
  occupy 256 cursor bytes here, so the footprint trade-off a real deployment
  would face is deliberately held out of the comparison rather than netted
  against the throughput numbers (§7.4). Phase 3A reports throughput, not memory.
- It does not claim a mechanism from wall-clock timing alone. A ratio is a ratio;
  §6 states what it is consistent with and what it cannot identify.

### 1.5 Evidence labels used in this document

- **MEASURED** — a number or count present in the raw data or derived from it
  arithmetically.
- **INTERPRETATION** — a reading of the measured data, offered as *consistent
  with* a possible mechanism. Never a demonstrated cause.
- **LIMITATION** — something this phase does not control, isolate or identify.

---

## 2. WHAT — the controls, the matrix, the reported quantities

### 2.1 The two controls

Both are aliases over **one** algorithm body in
`include/spsc_cursor_layout_ring_buffer.h`:

| type | layout | storage |
|---|---|---|
| `SpscSameLineRingBuffer<T, Capacity>` | `same_line` | both cursors in **one** interference block |
| `SpscSeparatedCursorRingBuffer<T, Capacity>` | `separated` | each cursor in its **own** interference block |

These are the only two Phase-3A controls. The type system enforces that: the
layout policy is a `static_assert`-constrained template parameter accepting
exactly `SameLineCursorBlock` or `SeparatedCursorBlocks`, so a third layout
cannot be introduced here by accident.

### 2.2 How the layouts are guaranteed, not hoped for

Declaring `std::atomic<size_t> head; std::atomic<size_t> tail;` next to each
other and *assuming* they share a line is exactly the assumption Phase 3A exists
to remove. Both layouts are therefore established **by construction** and then
**verified at runtime**, in that order.

**By construction.** Each policy is a type whose size and alignment are asserted
at compile time against `kAssumedCacheLineSize` (128 bytes; §2.3):

```cpp
struct alignas(kAssumedCacheLineSize) SameLineCursorBlock {
    // sizeof == 256, alignof == 128. Both cursors in the FIRST assumed block.
    std::atomic<std::size_t> head{0};                              // offset 0
    std::atomic<std::size_t> tail{0};                              // offset 8
    alignas(kAssumedCacheLineSize) std::array<std::byte, 128> reserved{};  // offset 128, INERT
};

struct alignas(kAssumedCacheLineSize) SeparatedCursorBlocks {
    // sizeof == 256, alignof == 128. One cursor per assumed block.
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> head{0};  // offset 0
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> tail{0};  // offset 128
};
```

For the same-line block, `offsetof(head) == 0`, `offsetof(tail) < 128`, and
`sizeof == 256` together mean every complete object starts on an assumed block
boundary with **both** cursors inside the **first** block — so "the cursors share
a line" is a property of the *type*, not of one lucky allocation. `reserved` is
inert storage: it is value-initialized once at construction and is never read or
written by `try_push`, `try_pop` or `empty()`. Its only job is to make the
footprint match the separated policy (§2.7). For the separated blocks, the two
members carry the alignment and land at offsets 0 and 128 of a 256-byte,
128-aligned object, so neither can begin in or share the other's block. The
member offsets (0 and 8) additionally keep the same-line property true for any
real line size down to 16 bytes, so the property survives a host whose line is
*smaller* than the assumption.

The assertions are the point. Both policies assert `sizeof == 2 × 128`, both
assert `alignof >= 128`, both assert `head` is at offset 0, and the same-line
policy asserts `tail` is inside the **first** block — so an edit that quietly
moved `tail` into the reserved second block, or that changed one policy's size,
**fails the build** rather than silently producing a different experiment. The
member-offset relationships are asserted rather than left to a comment:

```cpp
static_assert(sizeof(SameLineCursorBlock)   == 2 * kAssumedCacheLineSize);
static_assert(sizeof(SeparatedCursorBlocks) == 2 * kAssumedCacheLineSize);
static_assert(sizeof(SameLineCursorBlock) == sizeof(SeparatedCursorBlocks));
static_assert(offsetof(SameLineCursorBlock, tail) <  kAssumedCacheLineSize);
static_assert(offsetof(SeparatedCursorBlocks, tail) >= kAssumedCacheLineSize);
static_assert(offsetof(SameLineCursorBlock, head) == offsetof(SeparatedCursorBlocks, head));
```

Neither block holds anything else: no payload, no padding member *between* the
cursors, no unrelated hot metadata. The payload array follows the cursors in
**both** variants, in the same order, with the same explicit `alignas`.

**At runtime.** Static layout is a guarantee about the *assumed* line size, and
about the offsets *within* the policy. Only the host knows the real line size,
and only a real object knows where it landed. Every repetition therefore measures
the actual addresses of the object it is about to time and checks the invariant
(§2.6), and the benchmark additionally verifies on real objects that the two
policies produce the same object size and the same payload offset (§2.7).

### 2.3 The cache-line assumption is what the runtime check is measured against

The obvious constant here is 64 bytes. On the canonical development host 64 would
have been a poor choice: the Apple M3 Max reports a 128-byte cache line.

```console
$ sysctl hw.cachelinesize
hw.cachelinesize: 128
```

The role of the compile-time constant is to create the intended **candidate**
layout — alignment cannot be a runtime value — and the role of the runtime check
is to decide, from **measured addresses**, whether that candidate layout actually
materialised on this host for this object. A compile-time assumption larger or
smaller than the real block does not by itself determine the outcome; the
measured relationship of the real addresses does. The measured check is
authoritative, and it is what a cell is required to pass (§2.6).

Phase 3A therefore does the following (`include/cache_line.h`):

- Queries the host at runtime — `sysctlbyname("hw.cachelinesize")` on Apple,
  `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` elsewhere.
- Compares it against the compile-time assumption
  (`kAssumedCacheLineSize = 128`). This is a choice, not a derivation: it is
  large enough that a host reporting 128 keeps the two blocks of the separated
  policy apart, and it is not a claim about what any other reported size would
  imply — for **any** reported size it is the measured addresses, not this
  constant, that decide whether a cell's layout claim holds.
- **Refuses to measure at all** if the reported size exceeds the assumption. This
  is an early precondition rather than the layout proof: past that size the
  compile-time alignment can no longer keep the separated control's blocks in
  distinct real blocks, so the control could not be justified by construction and
  the run would be publishing numbers under a label it has not earned. The
  benchmark prints `LAYOUT ASSUMPTION UNSUPPORTED BY HOST` and exits non-zero
  **before any timing begins**; the runner propagates the failure and no cell is
  published. There is no "warn and continue" path.
- Measures the real addresses anyway, on every object, whatever the assumption
  said — because that is the check that decides.

`std::hardware_destructive_interference_size` would be the portable way to
express the assumption, but it is not available in the canonical toolchain (Apple
clang 15, libc++), so a checked constant is used instead of an unavailable one.

### 2.4 Why the frozen Phase-2 SPSC is NOT a control here

The frozen `SpscRingBuffer` is unpadded, with `head_` and `tail_` declared
adjacently. It is tempting to treat it as a free same-line control. Phase 3A
explicitly does **not**, for three reasons:

1. **Adjacency is not evidence of placement.** Two adjacent members are usually
   on the same line, but "usually" is not an invariant, and Phase 2 recorded no
   addresses, so there is no way to check after the fact. A control whose
   defining property was never verified is not a control.
2. **It would be a second variable.** The frozen type also differs in member
   order (`slots_` first, cursors last, vs. cursors first in both Phase-3A
   variants) and in having no `alignas` on the payload. Any difference measured
   against it would be attributable to placement *or* to those differences.
3. **Its payload offset is a third value again.** The frozen object puts the
   payload first, so its payload offset is 0 — a third relative offset, distinct
   from either Phase-3A variant's. Comparing against it would move cursor
   placement, member order and payload offset simultaneously.

The frozen type is therefore available in the benchmark as `--impl=natural` for
**observational** use only, is excluded from the canonical matrix, and can make
no cache-line claim. Its canonical numbers remain where they are, in
`docs/results/spsc-throughput/`, unmodified.

### 2.5 Matrix and reported quantities

Message types and capacities are identical to Phase 2, so the two datasets are
comparable in shape:

| axis | values |
|---|---|
| cursor layout | `same_line`, `separated` |
| message bytes | 8, 32, 64 (trivially copyable, nothrow, allocation-free) |
| capacity | 1024, 4096, 65536 (compile-time switch) |

**2 layouts × 3 sizes × 3 capacities = 18 cells per session**, 4 sessions, 72
processes, one implementation per process.

| quantity | meaning |
|---|---|
| `ns_per_message` | **end-to-end** elapsed ÷ messages delivered. **Not** a per-`try_push` latency and **not** a one-way handoff time. |
| `elapsed_ns` | the timed interval: consumer's own completion timestamp minus the post-ready-gate start |
| `producer_full_retries` / `consumer_empty_retries` | observed backpressure counts, not failures |
| `checksum`, `correctness` | the correctness gate (§3.5) |
| `reported_cache_line_size`, `head_addr`, `tail_addr`, `head_line`, `tail_line`, `cursors_same_line`, `layout_ok` | the runtime layout evidence, per measured repetition (§2.6) |
| `object_addr`, `object_size`, `payload_offset`, `payload_begin_addr` | the runtime footprint evidence, per measured repetition (§2.7) |

### 2.6 Runtime layout verification — the evidence the claim rests on

For **every repetition**, on the queue object that repetition is about to time,
and **outside** the timed interval, the benchmark records and checks:

| field | requirement |
|---|---|
| `reported_cache_line_size` | the host's own answer; must be > 0 and ≤ 128 |
| `head_addr`, `tail_addr` | the measured addresses of the two cursors |
| `head_line`, `tail_line` | those addresses converted to line indices under the reported size |
| `cursors_same_line` | `head_line == tail_line` |
| `layout_ok` | the conjunction below |

And the invariants:

- **`same_line`**: `head_line == tail_line` must hold.
- **`separated`**: `head_line != tail_line` must hold.
- **Cursor/payload disjointness**: neither cursor's interference block may
  overlap the payload array, in either variant. A cursor line that also holds
  payload would add a second, uncontrolled sharing relationship.
- **Line-size support**: the host's reported size must not exceed the assumption.

A cell whose layout report is not `ok()` **fails** — the process exits non-zero
before that repetition's timed interval — and nothing is published. An
unverified layout is not evidence about layout, so it is never recorded as a
measurement.

The runner independently re-checks all of these columns in every raw CSV before
deriving anything (§3.8), so the invariant is enforced twice by different code.

### 2.7 The equal-footprint requirement — the payload must not move

**This is the correction that Phase 3A.1 exists to make, and it is the reason
the earlier dataset is superseded (§7.5).**

Cursor placement is only the sole variable if nothing *else* about the object
moves when the cursor policy changes. Both variants declare `cursors_` first and
`slots_` second, so the payload offset is determined by the cursor policy's size.
The old design's two policies differed in size by 128 bytes, so the payload
array's **relative offset within the queue object** differed by 128 bytes between
the variants. That introduces an additional object-layout / address-mapping
variable that could affect cache-line and cache-set behaviour, and the experiment
would then change cursor placement *and* object layout at once.

**What the offset is, and is not.** Phase 3A records the object address, the
object size, the payload offset and the measured cursor addresses; it does **not**
measure the hardware's cache-set indexing function. The equal-footprint design
gives both variants the same *relative* payload offset, eliminating that
systematic type/layout-induced shift. It does **not** guarantee that
independently allocated objects in different processes occupy the same absolute
addresses or the same hardware cache sets. Absolute address placement and the
actual cache-set mapping remain uncontrolled and unmeasured nuisance factors —
which is why this document traces the confound to the **relative offset**, not to
a known cache set.

Both policies are therefore asserted to be exactly `2 × kAssumedCacheLineSize`
= 256 bytes, and the benchmark enforces the consequence at runtime **before
timing anything**:

| gate | where | what it requires |
|---|---|---|
| compile-time | `include/spsc_cursor_layout_ring_buffer.h` | `sizeof` and `alignof` of the two policies are equal; `head` at the same offset in both |
| cross-variant, before timing | `verify_cross_variant_footprint` in the benchmark | both policies are instantiated for the cell's message type and capacity, and their **measured** `object_size` and `payload_offset_from_object_base` agree |
| per repetition | `cursor_layout_report` | each measured repetition records `object_addr`, `object_size`, `payload_offset` and `payload_begin_addr` for the object it timed |
| dataset, after the run | runner leg 3 | every row's `payload_begin_addr − object_addr == payload_offset`, every row's `object_size − payload_offset` equals the true payload bytes, all rows of a layout agree, and the two layouts of every cell agree on both values |

A cell that fails the cross-variant gate **exits non-zero without timing
anything**. The runner re-derives the same conclusion from the raw columns and
refuses to publish (`invariants.txt` carries the per-cell footprint table as
`cursor_policies_equal_footprint`, `same_line_and_separated_object_size_equal`,
`same_line_and_separated_payload_offset_equal`).

The verified outcome on the canonical host is that all nine
(message bytes × capacity) shapes report identical object sizes and identical
payload offsets across the two layouts — see `LAYOUT_VERIFICATION.md`.

**What this does and does not buy.** Equal footprints remove payload offset as a
*difference between the two variants* at the level of type and object layout,
which is the level the experiment controls. They do not make the payload's
absolute position — or the host's actual cache-set mapping — controlled: both
remain unmeasured nuisance factors (§7.5). What the control buys is that those
factors are no longer **systematically** different between the two legs.

---

## 3. HOW — the measurement procedure

### 3.1 The harness is the Phase-2 harness

`benchmark/spsc_false_sharing_bench.cpp` reuses the proven Phase-2 measurement
design rather than inventing a new one, so that a difference between Phase 2 and
Phase 3A is a difference in *what was measured*, not in *how*. Identical:
one producer thread and one consumer thread over one queue; fixed-size messages;
10,000,000 messages per repetition; `-O3 -DNDEBUG` forced by the CMake target
regardless of build type; thread creation, queue allocation and the
expected-checksum pre-pass all **outside** the timed interval; the consumer
records its own completion timestamp; the correctness/checksum gate; the
consecutive-miss retry policy; raw repetitions preserved; **one implementation
per process**.

Phase-2 tooling is untouched — `benchmark/spsc_throughput_bench.cpp`,
`scripts/spsc-throughput.sh` and `docs/results/spsc-throughput/` are unchanged.
Phase 3A is a separate binary writing to a separate results directory.

**One implementation per process** is not a stylistic choice. Running both
layouts in one address space would let them share allocator state, thermal
history and address-space layout, and would make "the same-line leg" and "the
separated leg" two phases of one process rather than two independent
observations.

### 3.2 Why the two variants are one algorithm body

The claim "the only difference is cursor placement" is easy to state and easy to
break: a copy-pasted second implementation drifts, both copies still compile, and
both still pass their correctness tests, so the drift is invisible.

Phase 3A therefore makes the claim structural. There is **one** algorithm body,
`CursorLayoutRingBuffer<T, Capacity, CursorLayoutPolicy>`, a semantic copy of the
frozen Phase-1 body — identical full/empty tests (`h - tail == Capacity`,
`head == t`), identical slot indexing (`counter & (Capacity - 1)`), identical
memory orders (relaxed own-cursor load, acquire remote-cursor load, release
publish), identical `try_push(const T&)` / `try_push(T&&)` / `try_pop(T&)` /
`empty()` surface, identical `noexcept` specifications, identical
`static_assert`s. The only template argument that differs between the variants
is `CursorLayoutPolicy`, which contributes **storage and nothing else** — it has
no callable members, so it cannot alter control flow even by accident.

A runtime differential test (`test_variants_agree_under_identical_operation_sequence`)
drives 200,000 identical operations through both layouts and requires
observation-for-observation agreement, making the equivalence claim falsifiable
rather than merely asserted.

**What "the only difference" quantifies over.** It is a claim about the *program*:
the two queue implementations differ in one intentional program-layout treatment,
cursor placement, and in nothing else that the source controls. It is not a claim
that the two legs' *processes* are identical. Because the legs run as separate
processes with independently allocated objects, their absolute addresses differ,
and their scheduler placement, DVFS and thermal state and background-system state
differ too; none of that is eliminated by construction (§3.1, §3.4). The
construction removes the program-level difference so that the *measurement
design* — adjacent pairing, balanced AB/BA order, four sessions — can address the
environmental ones. §1's "one variable" statement should be read this way
throughout.

### 3.3 One repetition, one session

Each repetition constructs a **fresh producer/consumer thread pair and a fresh
queue object**, so cursor addresses are re-measured every repetition and no
repetition inherits another's thread placement. One warm-up repetition per
process is excluded from every published and derived figure.

A **session** is therefore a grouping of repetitions inside one process
lifetime — **not** a fixed thread placement. macOS may migrate threads, and no
placement is claimed anywhere in this phase.

### 3.4 Balanced AB/BA run design

On an unpinned macOS host, a layout difference and a time-of-run difference would
otherwise be indistinguishable. The two layouts of the same
`(message_bytes, capacity)` pair are therefore run as **adjacent processes**,
with both the layout order and the traversal direction balanced across four
sessions:

| session | traversal | layout order |
|---|---|---|
| 1 | forward | `same_line` → `separated` |
| 2 | reverse | `separated` → `same_line` |
| 3 | forward | `separated` → `same_line` |
| 4 | reverse | `same_line` → `separated` |

Every cell therefore receives **2 same-line-first and 2 separated-first**
comparisons, with forward/reverse traversal balancing position along the time
axis. The order is fixed and deterministic — never randomized — and is recorded
as it runs.

**The balance is verified, not assumed.** The runner parses the execution order
back out of `command.txt` and fails the dataset unless every one of the 9 cells
appears in all 4 sessions with each layout first exactly twice.

**Adjacent execution reduces temporal drift between the two legs but cannot
guarantee identical scheduler, DVFS, thermal, or background-system state** — the
two processes are still separated by a full benchmark run, and each leg can be
placed, migrated or frequency-scaled independently. The pairing narrows the gap;
it does not close it.

### 3.5 Correctness gating

Every cell must self-validate or it is not published. Each run must consume
exactly N messages in strictly increasing order with no gaps and no duplicates,
ending with a checksum that matches an independently precomputed expected stream.
A failure makes the benchmark exit non-zero, the runner stop, and no cell appear
in any summary.

The harness supplies backpressure (`while (!try_push(msg))` /
`while (!try_pop(msg))`, busy retry with an occasional `yield`, never a sleep)
identically for both layouts, and counts `producer_full_retries` /
`consumer_empty_retries` as observable metrics. Misses reset on every success;
`yield()` happens only after 1024 **consecutive** failures.

### 3.6 The layout variable is covered by tests, not just by the benchmark

`tests/spsc_false_sharing_tests.cpp` runs the **full Phase-1 correctness
protocol** against both layouts — FIFO ordering, no missing or duplicated
messages, rapid slot reuse at capacity 2, multi-field payload publication,
fill/pop-when-empty/full behaviour, repeated physical wrap-around, and a
differential run against a `std::deque` model and the `MutexBoundedQueue`
reference — and then adds the Phase-3A-specific evidence:

| test | what it establishes |
|---|---|
| same-line control **is** same-line at runtime | the measured addresses satisfy `head_line == tail_line` |
| separated control **is** separated at runtime | the measured addresses satisfy `head_line != tail_line` |
| the relationship reported at smaller line sizes is the intended one | for hypothesised line sizes 64 / 32 / 16 the same-line report still shows same-line, the separated report still shows separated, and neither shows a cursor sharing the payload's line. This tests the **report**, not any real host: no host reporting those sizes was measured, and Phase 3A makes no claim about what an arbitrary assumed-vs-real line-size pairing implies (§2.3) |
| guard **rejects** an unsupported line size | **negative control**: 256 must be rejected (`ok() == false`) |
| payload equivalent across variants | payload alignment, size and indexing are identical |
| cursor policy footprints are equal | `sizeof`/`alignof` equal; `head` offset equal; same-line `tail` in the **first** block, separated `tail` in the **second** |
| object size / payload offset equal per T+capacity | for all nine benchmark shapes **and** the small test shapes: both variants produce the same object size and the same payload offset, measured on real objects |
| variants agree on identical operation sequences | 200,000-op differential; the one-variable claim, falsifiable |

The negative control matters as much as the positive ones: a guard that never
fires is indistinguishable from no guard. The runner's equal-footprint leg was
verified the same way — a raw CSV with one `payload_offset` column altered was
rejected by **four** independent checks (offset vs. `payload_begin_addr −
object_addr`; `object_size − payload_offset` vs. true payload bytes; row-vs-row
agreement within a layout; and the cross-layout comparison).

The equal-footprint requirement is also enforced by `static_assert`, so the
cheapest version of the test is that the code compiles at all. The runtime tests
above exist because "the types agree" and "the objects that ran agree" are
different claims, and only the second one is what the dataset rests on.

### 3.7 Sanitizers

Both layouts run under ASan + UBSan and under TSan (Apple clang 15, arm64), and
both are clean — exit 0, TSan reporting zero warnings across the concurrent
stresses. That is a substantive result, not a formality: it is independent
evidence that the *padding change did not disturb the memory orders*, since a
relaxed load that should have been acquire, or a release that went missing,
would surface as a race on the payload rather than as a throughput number. The
instrumentation is verified to be present rather than assumed to be — the TSan
binary carries 17 `__tsan*` symbols and links
`libclang_rt.tsan_osx_dynamic.dylib`, and the ASan binary carries 708 `__asan*`
symbols — because a sanitizer build that silently failed to instrument would
report exactly the same "clean" result. **No performance number in this document
comes from a sanitizer build**; they are correctness evidence only.

### 3.8 Dataset invariants (all verified)

Checked by the runner, with the summary-vs-raw check performed by different code
from the code that wrote the summaries:

- exactly 72 canonical invocations recorded, exactly 72 raw CSVs, and the file
  set is **exactly** the set the recorded execution order implies;
- every raw CSV has exactly `REPS` measured rows, one stable checksum, and rows
  that agree with each other about which cell they are;
- every row is `correctness=PASS` with a positive `ns_per_message`;
- every row reports the same host cache-line size, and it is supported;
- every row is `layout_ok=PASS`, and the **measured** placement matches the
  layout the row claims — `same_line` rows measured same-line, `separated` rows
  measured separated;
- **equal footprint**: every row carries footprint evidence; its stated
  `payload_offset` equals `payload_begin_addr − object_addr`; `object_size −
  payload_offset` equals the true payload bytes; all rows of one layout agree;
  and the two layouts of every cell agree on both `object_size` and
  `payload_offset` (§2.7);
- the balanced AB/BA order holds per cell;
- every process summary matches the raw CSV it claims to summarize, recomputed
  independently.

Canonical performance numbers are taken **only** from the optimized,
non-sanitized build.

---

## 4. MEASURED — the canonical dataset

### 4.1 Host, build and provenance

Apple M3 Max (`Mac15,10`), macOS 14.2.1, Apple clang 15.0.0, arm64, clean
Release build of `spsc_false_sharing_bench` with no architecture-specific flags
(`BENCH_ARCH_FLAGS` empty), one implementation per process. The host reported a
**128-byte** cache line, which matched the compile-time assumption, so the
pre-timing host guard did not fire. Full host/toolchain metadata is in
`docs/results/spsc-false-sharing/HOST.md`; the git revision, the recorded
`git status --porcelain`, the exact build flags and the SHA-256 of the benchmark
executable and of the four key Phase-3A source files are in
`docs/results/spsc-false-sharing/PROVENANCE.md`. The run was made from a **dirty**
tree (`6916a80-dirty`), which is exactly why those hashes are recorded: the
dataset is identified by the bytes that produced it, not by `HEAD` alone.

### 4.2 The controlled variable, as verified

The equal-footprint requirement (§2.7) held for every cell, checked independently
at four levels:

| check | where | result |
|---|---|---|
| policy `sizeof` / `alignof` / cursor offsets | compile time (`static_assert`) | PASS for both policies — `SameLine` **256 bytes**, `Separated` **256 bytes** |
| `object_size` and `payload_offset_from_object_base` agree across variants | pre-timing runtime gate, every control process | PASS, every process |
| `payload_begin_addr − object_addr == payload_offset` and `object_size − payload_offset == message_bytes × capacity` | runner over the published raw rows | PASS, all 360 rows |
| cross-layout equality of `object_size` and `payload_offset` per cell | runner over the published raw rows | PASS, all 9 cells |

Measured footprint, identical for both layouts in every cell:
`object_size = 256 + message_bytes × capacity`, `payload_offset = 256`
(per-cell table in `invariants.txt` and `LAYOUT_VERIFICATION.md`). The runner
writes its result files only if these hold; the run that produced this section
reported `all_invariants=PASS`.

The cursor treatment itself was verified on the object that each repetition
actually timed: all 72 processes and all 360 measured rows carry the measured
`head`/`tail` addresses, their line indices under the reported 128-byte line,
and a `layout_ok=PASS` verdict. Every `same_line` row measured `head_line ==
tail_line`; every `separated` row measured `head_line != tail_line`; no row put
a cursor on the payload's line (cursor/payload line disjointness `PASS` on every
row). Correctness was `PASS` on all 360 rows, and the checksum was stable within
every cell.

### 4.3 Primary result — paired per-session ratios

`ratio` = separated session median ÷ same-line session median, computed on
**adjacent** processes within one session under the balanced AB/BA order.
`ratio < 1` means separated was faster. This paired view is primary; §4.5 is
secondary.

| bytes | capacity | s1 (first) | s2 (first) | s3 (first) | s4 (first) | median |
|---|---|---|---|---|---|---|
| 8 | 1024 | 0.328 (sl) | 0.235 (sep) | 0.261 (sep) | 0.214 (sl) | **0.248** |
| 8 | 4096 | 0.284 (sl) | 0.309 (sep) | 0.348 (sep) | 0.393 (sl) | **0.329** |
| 8 | 65536 | 0.422 (sl) | 0.374 (sep) | 0.332 (sep) | 0.306 (sl) | **0.353** |
| 32 | 1024 | 0.897 (sl) | 1.072 (sep) | 0.801 (sep) | 0.829 (sl) | 0.863 |
| 32 | 4096 | 1.541 (sl) | 1.160 (sep) | 1.444 (sep) | 1.408 (sl) | **1.426** |
| 32 | 65536 | 0.939 (sl) | 2.086 (sep) | 1.407 (sep) | 0.829 (sl) | 1.173 |
| 64 | 1024 | 0.878 (sl) | 1.015 (sep) | 0.829 (sep) | 0.952 (sl) | 0.915 |
| 64 | 4096 | 0.588 (sl) | 0.893 (sep) | 0.537 (sep) | 0.641 (sl) | **0.615** |
| 64 | 65536 | 0.560 (sl) | 0.558 (sep) | 0.617 (sep) | 0.645 (sl) | **0.588** |

`(sl)` / `(sep)` is which layout ran **first** in that session's pair. Bold marks
the five cells whose four ratios all fall on the same side of 1.

The direction is **not uniform**, and the headline result is *not* "padding is
faster": across the 36 paired observations, **28 favour separated and 8 favour
same-line**, and one cell is stably same-line-faster.

### 4.4 Direction stability across the balanced sessions

| bytes | capacity | median ratio | min | max | sep faster | sl faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 0.248 | 0.214 | 0.328 | 4 | 0 | **stable — separated 4/4** |
| 8 | 4096 | 0.329 | 0.284 | 0.393 | 4 | 0 | **stable — separated 4/4** |
| 8 | 65536 | 0.353 | 0.306 | 0.422 | 4 | 0 | **stable — separated 4/4** |
| 32 | 1024 | 0.863 | 0.801 | 1.072 | 3 | 1 | SPLIT 3–1 — no directional claim |
| 32 | 4096 | 1.426 | 1.160 | 1.541 | 0 | 4 | **stable — same-line 4/4** |
| 32 | 65536 | 1.173 | 0.829 | 2.086 | 2 | 2 | SPLIT 2–2 — no directional claim |
| 64 | 1024 | 0.915 | 0.829 | 1.015 | 3 | 1 | SPLIT 3–1 — no directional claim |
| 64 | 4096 | 0.615 | 0.537 | 0.893 | 4 | 0 | **stable — separated 4/4** |
| 64 | 65536 | 0.588 | 0.558 | 0.645 | 4 | 0 | **stable — separated 4/4** |

**Five cells are directionally stable, and all five are separated-faster; one
cell is stably same-line-faster; three are split and support no directional
claim.** MIN/MAX are the extremes of four correlated observations, not a
confidence interval, and `stable` here means "4 out of 4 agreed", not "proven".

The balanced AB/BA order is what makes the split cells readable as splits rather
than as noise: in the stable cells the ratios from same-line-first and
separated-first sessions overlap, so the direction tracks the layout rather than
the run position — while in the split cells the same check exposes that the
direction does **not** survive the swap.

### 4.5 The pooled matrix, as a secondary view

`MATRIX.md` pools all 20 measured repetitions of a cell across sessions. Pooled
median `ns/msg`, same-line vs separated — 8 B: 50.28 / 39.39 / 33.62 vs 12.85 /
12.54 / 12.78; 32 B: 30.21 / 24.77 / 22.11 vs 29.17 / 35.96 / 24.12; 64 B: 35.40
/ 24.55 / 26.33 vs 32.68 / 15.00 / 15.33 (capacities 1024 / 4096 / 65536).
Pooling treats the 5 repetitions inside one process as independent placements,
which they are not (§7.3), so these numbers describe the dataset rather than
support a directional claim. They agree with §4.3 in sign for every stable cell.

One descriptive detail from the same rows, stated without a mechanism claim: the
harness's retry counters show the two layouts spending their wait time on
different sides. At 8 B / 1024 the same-line producer logged 482 M full-queue
retries against 59 M empty-queue retries, while the separated producer logged
11 M full against 185 M empty; at 32 B / 4096 the separated consumer logged
1.16 B empty retries against 0.20 M full. These are harness-level backpressure
counters, not hardware counters, and they cannot distinguish a cause from an
effect — a faster side simply arrives first and waits more. They are reported
because they show the bottleneck side moves with the layout, which is part of
what a wall-clock ratio is actually summarizing here.

### 4.6 Secondary methodology observation — the superseded dataset

**This comparison is a methodology observation about the control, not a result
about cursor placement.** It uses the superseded, payload-offset-confounded
dataset (`docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/`) and
appears here only to document how much the uncontrolled variable moved the
numbers. **It is not a primary conclusion and must not be cited as one.**

Median paired ratio, superseded → equal-footprint:

| bytes | capacity | superseded | equal-footprint | change | superseded direction | new direction |
|---|---|---|---|---|---|---|
| 8 | 1024 | 0.249 | 0.248 | −0.6% | separated 4/4 | separated 4/4 |
| 8 | 4096 | 0.320 | 0.329 | +2.6% | separated 4/4 | separated 4/4 |
| 8 | 65536 | 0.319 | 0.353 | +10.5% | separated 4/4 | separated 4/4 |
| 32 | 1024 | 0.974 | 0.863 | −11.3% | SPLIT 2–2 | SPLIT 3–1 |
| 32 | 4096 | 1.614 | 1.426 | −11.6% | same-line 4/4 | same-line 4/4 |
| 32 | 65536 | 1.388 | 1.173 | −15.5% | SPLIT 1–3 | SPLIT 2–2 |
| 64 | 1024 | 1.154 | 0.915 | **−20.7%** | **same-line 4/4** | **SPLIT 3–1** |
| 64 | 4096 | 0.796 | 0.615 | −22.8% | separated 4/4 | separated 4/4 |
| 64 | 65536 | 0.770 | 0.588 | −23.6% | separated 4/4 | separated 4/4 |

Three observations, all of them about methodology:

1. **The uncontrolled variable was not negligible.** Moving the payload's
   relative offset within the object changed the median paired ratio by up to
   ~24%, which is the same order as the effects being measured in the 32 B and
   64 B rows. Whatever the mechanism, a design that shifted the object layout
   when it changed cursor placement could not have claimed a one-variable result
   — which is why the earlier dataset is archived rather than cited.
2. **The change is not a constant offset.** It moves ratios in both directions
   and changes the *sign* of exactly one cell (64 B / 1024, from a stable
   same-line win to a split). It therefore cannot be modelled away as a fixed
   correction, and it cannot be subtracted from the superseded numbers to recover
   a "corrected" dataset. No such correction is attempted anywhere here.
3. **This comparison cannot attribute the difference to the payload offset.**
   The two datasets differ in the payload offset **and** in run, so part of every
   delta above is ordinary run-to-run variation. Separating them would need
   repeated runs under both designs, which Phase 3A did not do. What the table
   establishes is the *bound on the problem*, not its cause: an uncontrolled
   layout variable of this kind moves these numbers by amounts comparable to the
   effect, so it had to be eliminated before any per-cell ratio could be read as
   a cursor-placement result.

The largest and most stable effect is also the one the change barely touched
(8 B / 1024 and 8 B / 4096 moved by under 3%), while the near-parity cells moved
most in relative terms. The document does not read a mechanism out of that
pattern.

---

## 5. DERIVED — the Phase-3A questions answered

Answers use the equal-footprint dataset only (§4.3–§4.4). The §6.1 wording rule
applies throughout: a separated-faster cell may be described as **consistent
with** reduced false-sharing interference; it is never described as proving that
false sharing caused the measured difference.

**1. Does the strong 8-byte separated advantage remain?**
**Yes, and it is the clearest effect in the dataset.** All three 8 B cells are
directionally stable separated-faster 4/4, with median paired ratios 0.248 /
0.329 / 0.353 across capacities 1024 / 4096 / 65536 — i.e. separated completed a
message in roughly a quarter to a third of the same-line time. Under the
equal-footprint control this is the one region where the result is large,
consistently signed, and essentially unchanged by the removal of the payload
offset (8 B / 1024 and 8 B / 4096 moved by under 3%; 8 B / 65536 by ~10%). It
satisfies every condition in §6.1 — both layouts runtime-verified, one algorithm
body, identical object size and payload offset, direction stable across all four
balanced sessions — and may therefore be described as *consistent with* reduced
false-sharing interference at that message size, and only in those terms.

**2. Do the 32 B / 4096 and 64 B / 1024 same-line-faster reversals remain?**
**One does, one does not.**
- **32 B / 4096 remains.** Same-line is faster in 4/4 sessions with a median
  paired ratio of 1.426 (range 1.160–1.541) — the only cell in the dataset that
  is stably faster in the same-line direction. It also satisfies every §6.1
  condition except that its sign is the one the simple false-sharing story does
  not predict. It is therefore reported as a **real, reproducible placement
  result**: it survived the equal-footprint control, and that control is what
  makes it readable as a one-variable result at all. Its magnitude did not grow
  (median 1.614 → 1.426); what changed is its evidential status. No mechanism is
  offered for it — Phase 3A collects no profiling evidence that could identify
  one — and it is not explained by the payload offset either, since the payload
  offset is now identical in both legs.
- **64 B / 1024 does not remain as a reversal.** In the superseded dataset it was
  same-line-faster 4/4 (median 1.154); under the equal-footprint control it is
  **SPLIT 3–1**, median 0.915, with all four session ratios inside
  0.829–1.015 — i.e. within about ±18% of parity, straddling 1. This cell is
  **inconclusive**: it supports no directional claim in either direction. Its
  change is the largest in the dataset and is discussed in §4.6.

**3. Which cells are directionally stable across all four balanced sessions?**
**Six of nine: five separated-faster and one same-line-faster. Three are split.**

| direction | cells |
|---|---|
| stable — separated 4/4 | 8 B / 1024 (0.248), 8 B / 4096 (0.329), 8 B / 65536 (0.353), 64 B / 4096 (0.615), 64 B / 65536 (0.588) |
| stable — same-line 4/4 | 32 B / 4096 (1.426) |
| split — no directional claim | 32 B / 1024 (3–1), 32 B / 65536 (2–2), 64 B / 1024 (3–1) |

The two 64 B cells with capacity ≥ 4096 are the only cells besides the 8 B row
that both stay stable and move further from parity: each already showed a
separated advantage (0.796 / 0.770) and shows a larger one under the equal
footprint (0.615 / 0.588), with a narrow session range (0.537–0.893 and
0.558–0.645). The 32 B row is noisy in a different way — one stable same-line
cell and two split cells — and this document does not attempt to explain why
32 B behaves differently from 8 B and 64 B.

**4. How much did equalizing the payload offset change the observed ratios?**
Measured, and reported only as a **secondary methodology observation** (§4.6):
per-cell median paired ratios moved by between −23.6% and +10.5% — up to ~24% in
magnitude, in both directions — and one cell's sign changed (64 B / 1024,
same-line-stable → split). Because the two datasets also differ by run, part of
every delta is
run-to-run variation and none of it can be attributed to the payload offset
alone; and because the change is not a uniform shift, the superseded numbers
cannot be corrected into an equal-footprint equivalent. The value of the
comparison is that it bounds the problem: an uncontrolled payload-offset
difference moves these ratios by amounts comparable to the effects being
measured, so the equal-footprint control was necessary before any per-cell ratio
could carry a cursor-placement reading. **The primary conclusions above rest
entirely on the equal-footprint dataset.**

---

## 6. INTERPRETATION

### 6.1 When a placement result may be *read in terms of* false sharing

This rule gates the **wording**, not just the conclusion. A separated-faster cell
that satisfies every condition below may be described as *consistent with*
reduced false-sharing interference; it may **not** be described as proving that
false sharing caused the measured difference, because separating the cursors also
gives up the colocation of two genuinely shared values (§1.3). A same-line-faster
cell satisfies the same conditions and is described the same way — it is a real
placement result whose sign the simple false-sharing story does not predict.

A placement result requires **ALL** of the following. If any one fails, the cell
is reported as **inconclusive**, not as a weak result:

1. **Both layouts runtime-verified** — the same-line control measured
   same-line, and the separated control measured separated, on the objects that
   actually ran, under the host's reported line size.
2. **Algorithm and memory orders identical** — one algorithm body, one set of
   memory orders, no cached remote cursor, no batching, no CAS.
3. **Payload position and harness otherwise equivalent** — **identical object
   size and identical payload offset across the two layouts for that cell**
   (§2.7); same payload alignment, size and indexing; same message types; same
   timing structure; same retry policy; one implementation per process.
4. **Direction reasonably stable across the balanced sessions** — a single
   session's direction is not evidence, and a cell whose sessions disagree is
   reported as split rather than summarized by its median.

### 6.2 What Phase 3A explicitly does not claim

- No formal statistical significance. Four paired observations per cell is a
  small sample, and no p-value is computed or implied anywhere.
- No causal mechanism from wall-clock timing alone. A separated-faster ratio is
  *consistent with* coherence traffic from line-granularity interference; the
  harness records no hardware counter that would identify it, and the opposite
  sign is equally consistent with the shared-cursor colocation effect (§1.3).
- **No decomposition of the measured difference into a false-sharing part and a
  shared-cursor-colocation part.** Layout alone cannot separate them, and no
  counter evidence is collected.
- **No claim that the payload-offset difference could not have mattered.** In the
  superseded pre-3A.1 dataset the payload's relative offset also changed with the
  cursor policy, and that dataset contains no counter evidence bounding its
  contribution either (§7.5).
- No claim about the frozen Phase-1 layout's false-sharing behaviour (§2.4).
- No claim that padding is free (§1.4).

---

## 7. LIMITATIONS

### 7.1 macOS scheduling: no affinity implemented or claimed

No CPU pinning, affinity or priority change is applied. macOS may migrate
threads mid-run and may place the two processes' threads on different core types
(P-core vs E-core on the canonical host). Phase 3A does not control this and
records no signal that would identify it.

### 7.2 The two variants are not the same shape of code

The layout policy changes cursor placement and nothing else — but that change is
*supposed* to change memory behaviour, which is the effect under study. The
same-line process's threads touch one line for their own cursor accesses; the
separated process's threads touch two. That is the treatment, not a confound.

### 7.3 Pooled repetitions are correlated

See §4.5. The pooled view is descriptive only.

### 7.4 The footprint trade-off is deliberately not measured

Under the equal-footprint control (§2.7) **both** layouts occupy 256 cursor
bytes, so the separated layout no longer costs more cursor memory than the
same-line layout *in this experiment* — and the same-line variant now carries a
128-byte reserved block it never touches. That reserved block is a **control
construct**, not a recommended production layout: outside the control, a
same-line policy would be 16 bytes, not 256. Phase 3A therefore measures the
placement effect at a fixed footprint and says nothing about the footprint
trade-off a real deployment would face (a smaller object, or a separated layout
that genuinely doubles cursor storage). Throughput is the only quantity
measured; memory cost is not netted anywhere.

### 7.5 The payload-offset confound — fixed in Phase 3A.1, and why the earlier dataset is superseded

**This section documents a flaw that has been fixed. It is retained because the
superseded dataset is real evidence and because the fix is only credible if the
flaw is stated plainly.**

**The flaw.** In the first Phase-3A design the two cursor policies were 128 bytes
(`same_line`) and 256 bytes (`separated`). Because both variants declare
`cursors_` first and `slots_` second, the payload's relative offset within the
object was determined by the cursor policy's size: it began at **object base +
128** in one variant and **object base + 256** in the other. The run therefore
changed **two** things at once: cursor cache-line placement, which was the
intended variable, and the object's internal layout / address mapping, which was
not controlled at all. A difference measured under that design cannot be
attributed to cursor placement alone.

**What is and is not being claimed about that.** The confound is real because the
**object layout changed** — the two variants did not merely differ in where the
cursors sat. Phase 3A does not measure the hardware's cache-set indexing function
and does not assert that either offset landed in a particular hardware cache set;
the objection is to the uncontrolled shift itself, not to a known mapping.

**This is a real controlled-variable flaw regardless of how large the effect
looked.** The harness feeds every failed attempt straight back into the next
attempt and eventually calls `yield()`, and the two threads are unpinned on
macOS, so a smaller low-level difference can be amplified into a larger
end-to-end throughput difference. The superseded dataset contains **no
hardware-counter evidence** that would separate the two variables or bound their
contributions, and Phase 3A has none either — so no statement of the form "the
payload offset cannot plausibly explain a 3×–4× result" is made anywhere in this
document. That earlier framing was wrong and has been removed.

**The fix (Phase 3A.1).** Both cursor policies are now exactly
`2 × kAssumedCacheLineSize` = 256 bytes. The same-line policy keeps **both**
cursors in the **first** assumed block and reserves a second, completely inert
block *solely* to equalize the footprint — the reserved line is never read or
written on the transfer path, and the same-line cursor invariant is unchanged.
The separated policy is unchanged. Consequences:

- the payload offset is identical across the two variants for every cell (§2.7),
  so it is no longer a difference between the legs;
- the earlier suggestion that this could not be fixed without destroying the
  same-line control was **incorrect**. Padding to 256 bytes does not force `tail`
  into the second line: `head` sits at offset 0 and `tail` at offset 8, both
  inside the first 128-byte block, and the compile-time assertions now enforce
  exactly that (§2.2);
- what remains different between the variants is where the cursors live and which
  line each cursor shares — the treatment — plus the inert reserved storage,
  which no thread touches.

**The superseded dataset.** Retained, unedited, at
`docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/`, with a
`SUPERSEDED.md` recording that its measurements are real, its correctness and
cursor-layout verification passed, and that it must not be cited for causal
cursor-placement claims. It may be compared against the equal-footprint dataset
as a **secondary methodology observation** only; primary conclusions use the
equal-footprint dataset.

**Still not controlled.** Equal footprints make the payload's *relative* offset
identical, not irrelevant: the payload still sits at object base + 256 in both
variants, and where that lands in the host's cache sets is whatever the host's
indexing function produces. The two legs also run as independent processes with
independently allocated objects, so their absolute addresses differ and nothing
in the design makes them equal. What the control guarantees is only that the
object layout is not *systematically* different between the legs — absolute
address placement and the actual cache-set mapping remain uncontrolled and
unmeasured nuisance factors. Phase 3A records no cache-set or coherence-counter
data and makes no claim about it. The same caveat applies in reverse to the
equal-footprint case, and §2.7 states it there.

### 7.6 Scope of the dataset

The canonical dataset was measured on one host (Apple M3 Max, macOS, 2026-09-12)
with one toolchain (Apple clang 15). Cache-line size, coherence implementation
and scheduler behaviour are all machine-specific; the *method* transfers, the
*numbers* do not. Its exact code identity is recorded in the dataset's
`PROVENANCE.md`, because the tree was dirty at run time.

---

## 8. Phase status

- Experiment 01 — COMPLETE / FROZEN.
- **Experiment 02, Phase 1 — Correctness / Memory Model: COMPLETE / FROZEN.**
- **Experiment 02, Phase 2 — Throughput Baseline: COMPLETE / FROZEN.**
  Canonical dataset: `docs/results/spsc-throughput/`.
- **Experiment 02, Phase 3A — Controlled Cursor Placement (project name:
  Controlled False Sharing): COMPLETE / FROZEN.** The equal-footprint canonical
  dataset was collected 2026-09-12 and verified against every invariant in §3.8:
  72 processes, 360 measured repetitions, `correctness=PASS` on every row,
  `layout_ok=PASS` on every row, `object_size` and `payload_offset` equal across
  the two layouts in all 9 cells, and 2 same-line-first / 2 separated-first per
  cell. Canonical dataset: `docs/results/spsc-false-sharing/`. Superseded
  pre-3A.1 dataset, retained unedited and **not to be cited**:
  `docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/`.
- **Experiment 02, Phase 3B — Remote-Cursor Caching: COMPLETE / FROZEN.**
  Collected 2026-09-12 by starting from **these** frozen Phase-3A separated
  layouts and changing one variable of its own — the frequency of remote cursor
  loads — so nothing in the Phase-3A dataset above is affected. Canonical
  dataset: `docs/results/spsc-remote-cursor/`; methodology and analysis:
  `docs/SPSC_REMOTE_CURSOR_CACHE.md`. Its result is negative (the mechanism
  reduced remote loads; the throughput did not follow), which is a statement
  about Phase 3B and not about cursor placement.
- **Experiment 02, Phase 4 — Tail Latency: NOT STARTED.**

**Phase 3B must not be combined with Phase 3A.** Remote-cursor caching
(`cached_head` / `cached_tail`) reduces the *number* of remote cursor
observations; cursor layout changes *where* the cursors live. Run together, a
measured difference is attributable to neither. Phase 3B therefore starts from
these frozen Phase-3A layouts and changes one variable of its own.

No Phase-2 number may be cited as evidence for or against false sharing in the
frozen Phase-1 layout: Phase 2 verified no cursor addresses and has no
packed/separated control.
