# Experiment 02 — SPSC Ring Buffer: Controlled False Sharing (Phase 3A)

**Scope.** This document is the *measurement* counterpart to
`docs/SPSC_MEMORY_MODEL.md` (Phase 1, *why the protocol is correct*) and
`docs/SPSC_THROUGHPUT.md` (Phase 2, *how fast the frozen protocol is*). Phase 3A
asks a narrower question than either: of the throughput behaviour observed in
Phase 2, how much changes when the producer-owned `head` cursor and the
consumer-owned `tail` cursor are **forced to occupy the same cache line** versus
**forced onto distinct cache lines** — and nothing else is changed at all.

**Phase 3A changes ONE variable: cursor cache-line placement.** There is no
cached remote cursor, no batching, no memory-order change, no CAS, and no
affinity anywhere in this phase. Remote-cursor caching is Phase 3B and is
deliberately **not** combined with the layout change, because an experiment that
moves two things at once attributes the result to neither.

**Status: Phase 3A — Controlled False Sharing: COMPLETE / FROZEN.**
Canonical dataset: `docs/results/spsc-false-sharing/`.

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
algorithm, the payload, the memory orders, the harness and the machine constant?*

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
is **false sharing**: an artefact of line granularity, not of program logic. The
separated layout removes exactly this, and nothing else.

### 1.3 False sharing vs. true sharing vs. required remote observation

This distinction is the one most easily lost, so Phase 3A states it explicitly.

**Padding does not remove legitimate communication, and Phase 3A does not claim
that it does.** Even with the cursors on separate lines, the protocol still
requires each thread to observe the other's cursor:

- The producer must read `tail` (acquire) before reusing a slot — the **reuse
  gate**. Without it, the producer would overwrite a payload the consumer has not
  yet read.
- The consumer must read `head` (acquire) before reading a payload — the
  **availability gate**. Without it, the consumer would read a slot the producer
  has not yet published.

Both of those are **required remote observations**. They are true sharing in the
sense that matters: the value genuinely must cross between threads, and no layout
can or should eliminate them. They remain in **both** Phase-3A variants,
unchanged, with the same memory orders.

So the two mechanisms are separable, and Phase 3A separates them:

| mechanism | what it is | present in same-line? | present in separated? |
|---|---|---|---|
| **Required remote observation** | producer reads `tail` (acquire); consumer reads `head` (acquire) — correctness gates | yes | **yes** |
| **False sharing** | line-granularity interference between the two *own-cursor* accesses (`head` store vs `tail` load, and vice versa) | yes | **no** |

Because the first row is identical in both variants, any measured difference
between them is attributable to the second row — line granularity — and not to
the existence of cross-thread communication. That is the entire logic of the
experiment, and it depends on the first row really being identical, which is
guaranteed by construction (§3.2) and re-checked by test (§3.6).

### 1.4 What Phase 3A deliberately does not claim

- It does not claim the frozen Phase-1 layout *exhibits* false sharing. Phase 2
  verified no cursor addresses at all, so it cannot support that claim (see
  §2.4). Phase 3A measures two new, verified layouts.
- It does not claim padding is free. Padding pays memory; at capacity 65536 with
  64-byte messages the payload dominates, and at capacity 1024 the two cursor
  lines are a measurable fraction of the object. Phase 3A reports throughput,
  not memory, and does not net one against the other.
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
struct SameLineCursorBlock {                    // sizeof == 128, alignof == 128
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> head{0};
                                   std::atomic<std::size_t> tail{0};   // offset 8
};

struct SeparatedCursorBlocks {                  // sizeof == 256, alignof == 128
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> head{0};   // offset 0
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> tail{0};   // offset 128
};
```

For the same-line block, `sizeof == 1 × 128` and `alignof >= 128` together mean
every complete object occupies exactly one assumed block, and both members lie
inside it by definition of `sizeof` — so "the cursors share a line" is a property
of the *type*, not of one lucky allocation. For the separated blocks,
`sizeof == 2 × 128` with both members carrying the alignment puts them at offsets
0 and 128 of a 256-byte, 128-aligned object, so neither can begin in or share the
other's block. The member offsets (0 and 8) additionally keep the same-line
property true for any real line size down to 16 bytes, so the property survives
a host whose line is *smaller* than the assumption.

Neither block holds anything else: no payload, no padding member between the
cursors, no unrelated hot metadata. The payload array follows the cursors in
**both** variants, in the same order, with the same explicit `alignas`, so the
payload is not a second variable.

**At runtime.** Static layout is a guarantee about the *assumed* line size. Only
the host knows the real one. Every repetition therefore measures the actual
addresses of the object it is about to time and checks the invariant (§2.6).

### 2.3 The cache-line assumption is checked, and 64 would have been wrong

The obvious constant here is 64 bytes. On the canonical development host it is
**wrong**: the Apple M3 Max reports a 128-byte cache line.

```console
$ sysctl hw.cachelinesize
hw.cachelinesize: 128
```

This is not a detail. A wrong line size invalidates **both** controls, in
opposite directions:

- Assume 64 on a 128-byte machine: the "separated" blocks, 64 bytes apart, land
  in the **same** real line. The separated control silently becomes a
  same-line control, and the experiment would report *the opposite of the truth*
  with no failing assertion anywhere.
- Assume 128 on a 64-byte machine: the same-line block spans two lines, so the
  same-line control silently becomes a separated control.

Phase 3A therefore does the following (`include/cache_line.h`):

- Queries the host at runtime — `sysctlbyname("hw.cachelinesize")` on Apple,
  `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` elsewhere.
- Compares it against the compile-time assumption
  (`kAssumedCacheLineSize = 128`, chosen as a conservative value so that a host
  reporting 64 or 32 still satisfies both invariants).
- **Fails the experiment rather than publishing** if the reported size exceeds
  the assumption. The benchmark prints `LAYOUT ASSUMPTION UNSUPPORTED BY HOST`
  and exits non-zero **before any timing begins**; the runner propagates the
  failure and no cell is published. There is no "warn and continue" path.

`std::hardware_destructive_interference_size` would be the portable way to
express this, but it is not available in the canonical toolchain (Apple clang
15, libc++), so a checked constant is used instead of an unavailable one.

### 2.4 Why the frozen Phase-2 SPSC is NOT a control here

The frozen `SpscRingBuffer` is unpadded, with `head_` and `tail_` declared
adjacently. It is tempting to treat it as a free same-line control. Phase 3A
explicitly does **not**, for two reasons:

1. **Adjacency is not evidence of placement.** Two adjacent members are usually
   on the same line, but "usually" is not an invariant, and Phase 2 recorded no
   addresses, so there is no way to check after the fact. A control whose
   defining property was never verified is not a control.
2. **It would be a second variable.** The frozen type also differs in member
   order (`slots_` first, cursors last, vs. cursors first in both Phase-3A
   variants) and in having no `alignas` on the payload. Any difference measured
   against it would be attributable to placement *or* to those differences.

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
| invariants hold for smaller real line sizes | 64 / 32 / 16 all keep both invariants, so 128 is conservative |
| guard **rejects** an unsupported line size | **negative control**: 256 must be rejected (`ok() == false`) |
| payload equivalent across variants | payload alignment, size and indexing are identical |
| variants agree on identical operation sequences | 200,000-op differential; the one-variable claim, falsifiable |

The negative control matters as much as the positive ones: a guard that never
fires is indistinguishable from no guard.

### 3.7 Sanitizers

Both layouts run under ASan + UBSan and under TSan (Apple clang 15, arm64), and
both are clean — TSan reports zero warnings across the concurrent stresses. That
is a substantive result, not a formality: it is independent evidence that the
*padding change did not disturb the memory orders*, since a relaxed load that
should have been acquire, or a release that went missing, would surface as a
race on the payload rather than as a throughput number.

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
- the balanced AB/BA order holds per cell;
- every process summary matches the raw CSV it claims to summarize, recomputed
  independently.

Canonical performance numbers are taken **only** from the optimized,
non-sanitized build.

---

## 4. MEASURED — the canonical dataset

**MEASURED (dataset).** Apple M3 Max, macOS 14.2.1, Apple clang 15.0.0, repo
HEAD `5f485fe`. 4 sessions, **72 processes**, **360 measured repetitions**
(5 per process, plus an excluded warm-up), **3.6 billion message transfers**,
one implementation per process, all `correctness=PASS`, all dataset invariants
PASS (`docs/results/spsc-false-sharing/invariants.txt`).

**MEASURED (layout evidence).** All 360 measured repetitions carried a passing
runtime layout verification under the host's reported **128-byte** cache line.
Every `same_line` row measured `head` and `tail` in one line (addresses 8 bytes
apart); every `separated` row measured them in different lines (addresses 128
bytes apart); no cursor block overlapped payload storage in any row
(`LAYOUT_VERIFICATION.md`). The controls are therefore verified controls on the
objects that actually ran, not on the types.

### 4.1 Primary comparison: paired per-session ratio

The primary figure is the **paired ratio**:

```
ratio = separated session median / same_line session median
```

**`ratio < 1` means separated cursors completed a message faster in that
session.** The two processes of a pair ran adjacent, under the balanced AB/BA
order of §3.4, so the ratio compares neighbouring-in-time measurements rather
than pooling correlated repetitions.

| bytes | capacity | median ratio | min | max | separated faster | same_line faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | **0.2495** | 0.2447 | 0.2695 | 4 | 0 | stable — separated 4/4 |
| 8 | 4096 | **0.3203** | 0.2992 | 0.3887 | 4 | 0 | stable — separated 4/4 |
| 8 | 65536 | **0.3191** | 0.3157 | 0.3661 | 4 | 0 | stable — separated 4/4 |
| 32 | 1024 | 0.9735 | 0.8339 | 1.1590 | 2 | 2 | **SPLIT 2–2 — inconclusive** |
| 32 | 4096 | **1.6136** | 1.5747 | 1.6992 | 0 | 4 | stable — **same_line 4/4** |
| 32 | 65536 | 1.3883 | 0.9628 | 1.6579 | 1 | 3 | **SPLIT 1–3 — inconclusive** |
| 64 | 1024 | **1.1543** | 1.0851 | 1.2631 | 0 | 4 | stable — **same_line 4/4** |
| 64 | 4096 | **0.7960** | 0.7387 | 0.8576 | 4 | 0 | stable — separated 4/4 |
| 64 | 65536 | **0.7704** | 0.6557 | 0.8336 | 4 | 0 | stable — separated 4/4 |

**MEASURED (headline).** Of 9 cells, **7 hold one direction across all four
balanced sessions and 2 are inconclusive** — but the direction is **not
uniform**: **5 cells are stably *separated*-faster** (median ratios 0.25–0.80)
and **2 cells are stably *same-line*-faster** (median ratios 1.15 and 1.61).
Across all 36 paired observations, 23 favour separated and 13 favour same-line.
The median of the nine cell medians is 0.796.

**This dataset does not support "padding is faster" as a general claim.** It
supports the narrower statement that cursor placement has a large, reproducible
effect whose *sign depends on the cell*.

### 4.2 The effect tracks layout, not run position

**MEASURED.** Within each stable cell, the ratios from the sessions where
`same_line` ran first and the sessions where `separated` ran first agree to
within a few percent, while the layout effect is far larger. For 8 B / 1024:
0.2521 and 0.2447 (same-line-first) versus 0.2469 and 0.2695
(separated-first) — a spread of ~4% around a 4× effect. For 32 B / 4096: 1.6060
and 1.6992 versus 1.6213 and 1.5747 — a spread of ~3% around a 1.6× effect.

**INTERPRETATION.** This is the balanced AB/BA design doing its job: if the
result were driven by position-in-run rather than by cursor placement, swapping
the order between sessions would have moved the ratio. It did not.

### 4.3 Largest and smallest effects

**MEASURED.** The largest effects are at **8-byte messages**, where separated is
**3.1×–4.0× faster at every capacity** (ratios 0.2495 / 0.3203 / 0.3191, tight
ranges). Moderate effects favour separated at 64 B / 4096 and 64 B / 65536
(0.796 / 0.770, i.e. separated ~20–23% faster). The two stable reversals are
32 B / 4096 (same-line 1.61× faster) and 64 B / 1024 (same-line 1.15× faster).

**INTERPRETATION.** The 8-byte result is the one that most resembles the
textbook false-sharing signature: a small payload makes the per-message cursor
traffic dominant, and separating the cursors multiplies throughput by 3–4×. The
reversals are **not** explained by that mechanism (§5 Q5, §7.5).

### 4.4 Pooled matrix — secondary

The pooled matrix (`MATRIX.md`, `summary.csv`) is retained as a **secondary,
descriptive** view and is not the basis for any directional claim. Pooling
treats the repetitions inside one process as independent placements. They are
not: a repetition does create a fresh thread pair and a fresh queue object, but
it shares its process's address space, allocator state and thermal history with
its siblings. It agrees with the paired view in aggregate but is not cited for
direction.

### 4.5 Reference: the Phase-2 baseline

The frozen Phase-2 dataset (`docs/results/spsc-throughput/`) is **unchanged**
and remains the natural/unpadded baseline for *its own* question. It is not a
Phase-3A control (§2.4), is not re-analyzed here, and **no Phase-2 number may be
cited as evidence for or against false sharing** in the frozen layout — Phase 2
verified no cursor addresses.

---

## 5. DERIVED — the Phase-3A questions answered

### Q1. Does separating the cursors change throughput, and in which direction?

**Yes, substantially — and the direction is not uniform.** Separated is faster in
5 of the 7 directionally stable cells; same-line is faster in the other 2. The
largest effect is 4.0× (8 B / 1024, separated faster) and the largest reversal is
1.61× (32 B / 4096, same-line faster).

### Q2. Is the direction stable across the balanced sessions?

**For 7 of 9 cells, yes** — all four sessions agree (5 separated 4/4, 2
same-line 4/4). **2 cells are inconclusive**: 32 B / 1024 (2–2) and 32 B / 65536
(1–3). Stability here means "4 of 4 agreed", which is a descriptive criterion,
not a significance test.

### Q3. Where does the effect appear across the size/capacity matrix?

| region | result |
|---|---|
| 8 B, all capacities | separated faster, 3.0×–4.0×, tightest ranges in the dataset |
| 64 B, capacities 4096 / 65536 | separated faster, ~20–23% |
| 64 B, capacity 1024 | **same-line faster**, ~15% |
| 32 B, capacity 4096 | **same-line faster**, ~61% (largest reversal) |
| 32 B, capacities 1024 / 65536 | inconclusive |

The effect is **largest where the payload is smallest** and the per-message
cursor traffic is therefore proportionally dominant.

### Q4. Which cells are inconclusive?

**32 B / 1024** (ratios 1.021, 0.834, 1.159, 0.926 — two sessions each way, no
stable direction) and **32 B / 65536** (1.240, 0.963, 1.658, 1.536 — three of
four favour same-line, but one does not). Neither supports a directional claim
at any magnitude, regardless of what its median ratio says.

### Q5. Is the effect plausibly attributable to false sharing?

This is judged against the §6.1 rule, which requires **all four** conditions.

| condition | status |
|---|---|
| both layouts runtime-verified | **MET** — all 360 measured repetitions, per-object, under the host's reported 128-byte line |
| algorithm and memory orders identical | **MET** — one algorithm body; policy contributes storage only; TSan clean on both; 200k-op differential agreement |
| payload layout and harness otherwise equivalent | **MET** — identical payload alignment/size/indexing; same messages; same timing; same retry policy; one impl per process |
| direction reasonably stable across balanced sessions | **MET for 7 cells, NOT MET for 2** |

**Verdict — separated-faster cells (5).** All four conditions hold, so a
false-sharing attribution is **permitted** for these cells. The 3.0×–4.0×
magnitude at 8 B is consistent with the predicted mechanism: with a small
payload, the per-message cost is dominated by the two own-cursor accesses, and
line-granularity interference between them is exactly what separation removes.

**Verdict — same-line-faster cells (2).** All four conditions also hold here, so
these are **real, reproducible directional results** — and the false-sharing
mechanism predicts the **opposite sign**. **INTERPRETATION:** either a second
mechanism is at work, or a difference not controlled by Phase 3A dominates in
these cells. §7.5 documents one such difference. Phase 3A does **not** identify
which. Reporting these as "padding is still probably better overall" would
misstate the measurement.

**Verdict — inconclusive cells (2).** Reported as **inconclusive**. No
attribution is made in either direction.

**Overall: the false-sharing attribution is permitted for 5 cells, contradicted
in sign for 2, and unavailable for 2.** The experiment does not support a
general claim that separating the cursors improves SPSC throughput; it supports
a cell-dependent one.

---

## 6. INTERPRETATION

### 6.1 When false sharing may be claimed as the attribution

A false-sharing attribution requires **ALL** of the following. If any one fails,
the result is reported as **inconclusive**, not as a weak positive:

1. **Both layouts runtime-verified** — the same-line control measured
   same-line, and the separated control measured separated, on the objects that
   actually ran, under the host's reported line size.
2. **Algorithm and memory orders identical** — one algorithm body, one set of
   memory orders, no cached remote cursor, no batching, no CAS.
3. **Payload layout and harness otherwise equivalent** — same payload alignment,
   size and indexing; same message types; same timing structure; same retry
   policy; one implementation per process.
4. **Direction reasonably stable across the balanced sessions** — a single
   session's direction is not evidence, and a cell whose sessions disagree is
   reported as split rather than summarized by its median.

### 6.2 What Phase 3A explicitly does not claim

- No formal statistical significance. Four paired observations per cell is a
  small sample, and no p-value is computed or implied anywhere.
- No causal mechanism from wall-clock timing alone. An elevated ratio is
  *consistent with* coherence traffic from line-granularity interference; the
  harness records no hardware counter that would identify it.
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

See §4.2. The pooled view is descriptive only.

### 7.4 The separated layout costs memory

Two cache lines for cursors instead of one, plus payload alignment. Phase 3A
measures throughput only and does not net that cost.

### 7.5 The payload sits at a different offset in the two variants

**LIMITATION.** The two variants place their payload array at different offsets
within the object, and Phase 3A does not control for it.

Both variants declare `cursors_` first and `slots_` second, and both align
`slots_` to 128 bytes. But the cursor block is 128 bytes in `same_line` and
**256** bytes in `separated`, so relative to a 128-aligned allocation base the
payload begins at **base + 128** in one variant and **base + 256** in the other.

Consequence: the two variants' payload arrays start in **different cache sets**
(set index shifts by one), so the aliasing relationship between the hammered
cursor line(s) and the streaming payload region is not identical between the
variants. This is a genuine difference between the two legs that is *not* the
false-sharing treatment.

**INTERPRETATION.** This is a plausible contributor to the **smaller-magnitude**
cells, including possibly the two stable reversals (§4.3, §5 Q5). It cannot
plausibly account for a 3×–4× effect, so the 8-byte results are not explained
away by it. Phase 3A does not measure cache-set placement and does **not**
identify how much of any cell's ratio this contributes.

**Not fixed in Phase 3A, deliberately.** Forcing both variants' payloads to the
same offset would require either padding `same_line`'s cursor block to 256 bytes
— which would put both cursors in different lines for half the block and destroy
the same-line control — or restructuring the member order so the payload comes
first, which would change both variants' cursor/payload relationship relative to
the frozen Phase-1 layout. Either would trade one uncontrolled difference for a
worse one. The honest disposition is to document it and let a later phase test it
directly if a cell's direction matters.

### 7.6 Scope of the dataset

The canonical dataset was measured on one host (Apple M3 Max, macOS) with one
toolchain (Apple clang 15). Cache-line size, coherence implementation and
scheduler behaviour are all machine-specific; the *method* transfers, the
*numbers* do not.

---

## 8. Phase status

- Experiment 01 — COMPLETE / FROZEN.
- **Experiment 02, Phase 1 — Correctness / Memory Model: COMPLETE / FROZEN.**
- **Experiment 02, Phase 2 — Throughput Baseline: COMPLETE / FROZEN.**
  Canonical dataset: `docs/results/spsc-throughput/`.
- **Experiment 02, Phase 3A — Controlled False Sharing: COMPLETE / FROZEN.**
  Canonical dataset: `docs/results/spsc-false-sharing/`.
- **Experiment 02, Phase 3B — Remote-Cursor Caching: NOT STARTED.**
- **Experiment 02, Phase 4 — Tail Latency: NOT STARTED.**

**Phase 3B must not be combined with Phase 3A.** Remote-cursor caching
(`cached_head` / `cached_tail`) reduces the *number* of remote cursor
observations; cursor layout changes *where* the cursors live. Run together, a
measured difference is attributable to neither. Phase 3B therefore starts from
these frozen Phase-3A layouts and changes one variable of its own.

No Phase-2 number may be cited as evidence for or against false sharing in the
frozen Phase-1 layout: Phase 2 verified no cursor addresses and has no
packed/separated control.
