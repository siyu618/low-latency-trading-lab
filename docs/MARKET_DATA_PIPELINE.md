# Experiment 03 Phase 1 — Market Data Pipeline (Sequencer and Correctness)

> This document now covers two phases of Experiment 03. Everything down to and
> including the Phase-1 status block is **frozen as written**; the Phase-2 part
> begins at the second top-level heading.

## Why

Experiments 01 and 02 built a correct L2 order book and a correct single-producer
/ single-consumer transport. Neither answers the question a real feed handler
faces on every session: **what does a consumer do when the sequenced stream it
is reading is lossy?**

A book that has silently missed a message is not a slightly stale book. It is a
**wrong** book, and no amount of careful updating afterwards repairs it — the
only recovery is a full state replace. Everything that follows is the policy for
detecting that situation, refusing to pretend otherwise, and accounting for it.

Phase 1 is **correctness only**. It reads no clock, measures no time, and ships
no benchmark binary. That matches how Experiments 01 and 02 both began, and it
is a deliberate ordering: there is no useful latency question to ask about a
state machine whose transitions have not been pinned.

## What is measured — and what is deliberately not

Nothing is timed. What this phase establishes is a set of **observable,
assertable facts per message**:

- the outcome of every message (exactly one, from a fixed set of ten),
- the state after every message,
- the next sequence the pipeline requires after every message,
- the contents of the book at the end of a trace,
- and the outage accounting: which sequences were lost, how far the stream ran
  while the view was unusable, and what that cost in unusable messages.

Outage accounting is in **messages and sequence numbers**, never in nanoseconds.
`max_seq_seen - first_missing_seq + 1` is how far the stream ran during an
outage; `discarded` is what it cost. There is no duration anywhere in this
phase, and a reader should not read the sequence counts as time.

### What this phase does NOT do

- **It does not request a resync.** A real handler asks the venue for a fresh
  snapshot when it detects a gap. This one detects the gap and waits for a
  snapshot to arrive. That is a policy gap, stated rather than hidden.
- **It does not decode anything.** Messages are typed values. There is no wire
  format, no byte layout, no parser, and therefore no parser tests.
- **It does not reorder or buffer.** A message that arrives out of order is
  stale or a gap — never held for a later arrival. There is no jitter buffer.
- **It does not validate that a snapshot is best-first or uncrossed.**
  `validate_snapshot()` checks negatives, the price domain and per-side
  duplicates; ordering is not part of the contract it enforces, so the pipeline
  does not enforce it either.
- **It does not close the position.** A partial level update (a delta against an
  existing quantity rather than a replacement) is not modelled; `qty` is the
  absolute quantity at that price, as in Experiments 01 and 02.

## The seam: framing and sequencing vs snapshot validity

**The pipeline owns framing and sequencing. The book owns snapshot validity and
application.**

Snapshot content accumulates into a staging `llob::BookSnapshot`. At
`SnapshotEnd` the pipeline hands that staging to its sink's own
`load_snapshot()`, so `validate_snapshot()` governs snapshots here exactly as it
does for every other snapshot path in this repository. **There is one snapshot
contract, not two.**

The rejected alternative was to stage by `apply()`-ing each level into a fresh
empty book. That was rejected because it would give snapshots a *different*
validity rule from every other consumer of the same data: a repeated price would
be silently overwritten instead of refused as ambiguous state, and an
out-of-domain price silently consumed instead of refusing the run. It would also
cost a second full-size book and could not be swapped in atomically.

### In-order levels are delegated to the sink

The pipeline never decides by hand whether an in-order level is good. It calls
`book.apply()` and maps the result. This is not a convenience — two of the
book's five outcomes are **invisible to a sequence comparison alone**:

- `InvalidUpdate` (`qty < 0`) makes the book **unsync itself**;
- `OutOfRange` **consumes the sequence** and leaves the book synced.

A pipeline that only compared sequence numbers would report `Live` forever over
an unsynced book — a silent, permanent stall with no gap ever firing to reveal
it. Delegating also means the book is invalidated by the book's own code path
rather than by a second mechanism the pipeline would have to invent; no book in
this repository exposes an `invalidate()`.

## The contract

### Messages

```cpp
enum class MdKind : uint8_t { SnapshotBegin, SnapshotEnd, Level };
struct MdMessage { MdKind kind; uint64_t seq; Side side; int64_t price; int64_t qty; };
```

The **snapshot is sequenced**. `SnapshotBegin / Level... / Level / SnapshotEnd`
is a contiguous run that consumes sequence numbers exactly like the incremental
stream does, and `SnapshotEnd`'s own sequence becomes the sequence the book is
published at — so the next incremental message is `end.seq + 1`. There is no
side channel and no out-of-band snapshot.

The `Level` messages inside a bracket are the **same kind** that carries the
live stream. The only difference is the framing around them: the snapshot path
is not a second message type with its own decoding rules.

`side`, `price` and `qty` are meaningful only for `Level`; the `md_begin` /
`md_end` factories zero them. `qty < 0` is representable on purpose — it is a
malformed message the pipeline must classify, not a value it may assume away.

### States

| State | Meaning |
|---|---|
| `NotSynced` | No usable view has **ever** been established. The initial state. |
| `Live` | The book is in step with the stream; in-order levels apply. |
| `Snapshot` | A bracketed full-state run is being assembled into staging. |
| `Gap` | A view **was** established and has been lost. |

`NotSynced` is kept distinct from `Gap` because the two cost different things. A
cold start that never syncs has an outage of unknown length; losing a live view
has a measurable one, and only the latter opens a recovery episode. The
transition `Gap → NotSynced` is impossible: `ever_synced_` is never cleared.

### Outcomes

Ten, deliberately richer than the book's `ApplyResult`, which conflates "you
sent me a duplicate" with "my view is unusable" into one `Stale` (`types.h`).
A feed handler must tell those apart.

| Outcome | Meaning |
|---|---|
| `Applied` | In-order level applied to the live book. |
| `OutOfRange` | In-order level outside the price domain: sequence **consumed**, book stays synced, we stay `Live`. |
| `Staged` | Bracket opened, or a level absorbed into the open snapshot. |
| `SnapshotCommitted` | `SnapshotEnd` published a valid snapshot; now `Live`. |
| `SnapshotAbandoned` | An open snapshot was discarded. |
| `Stale` | `seq` is behind the relevant cursor: a replay or a duplicate. |
| `GapDetected` | Forward jump while `Live`; the view is lost. |
| `Malformed` | In-order level the book refused (`qty < 0`); view lost. |
| `Rejected` | A level arriving when there is no view to apply it to. |
| `ProtocolViolation` | `SnapshotEnd` with no bracketed run open. |

### The transition table

Every message goes through exactly one row.

| State | Message | Condition | Outcome | Next state |
|---|---|---|---|---|
| any | `SnapshotBegin` | `seq < expected()` | `Stale` | unchanged |
| `Snapshot` | `SnapshotBegin` | fresh | `Staged` (old run discarded) | `Snapshot` |
| any other | `SnapshotBegin` | fresh | `Staged` | `Snapshot` |
| `NotSynced` / `Gap` | `Level` | — | `Rejected` | unchanged |
| any except `Snapshot` | `SnapshotEnd` | — | `ProtocolViolation` | unchanged |
| `Live` | `Level` | book says `Applied` | `Applied` | `Live` |
| `Live` | `Level` | book says `OutOfRange` | `OutOfRange` | `Live` |
| `Live` | `Level` | book says `Stale` | `Stale` | `Live` |
| `Live` | `Level` | book says `GapDetected` | `GapDetected` | `Gap` |
| `Live` | `Level` | book says `InvalidUpdate` | `Malformed` | `Gap` |
| `Snapshot` | `Level` | `seq < snap_expected` | `Stale` (staging intact) | `Snapshot` |
| `Snapshot` | `Level` | `seq == snap_expected` | `Staged` | `Snapshot` |
| `Snapshot` | `Level` | `seq > snap_expected` | `SnapshotAbandoned` | `Gap` |
| `Snapshot` | `Level` | staging cap reached | `SnapshotAbandoned` | `Gap` |
| `Snapshot` | `SnapshotEnd` | `seq < snap_expected` | `Stale` | `Snapshot` |
| `Snapshot` | `SnapshotEnd` | `seq > snap_expected` | `SnapshotAbandoned` | `Gap` |
| `Snapshot` | `SnapshotEnd` | `seq == snap_expected`, valid | `SnapshotCommitted` | `Live` |
| `Snapshot` | `SnapshotEnd` | `seq == snap_expected`, refused | `SnapshotAbandoned` | `Gap` |

Notes that matter:

- **`GapDetected` does not consume the sequence.** The offending `seq` is never
  advanced past, so the missed range cannot be silently skipped and `expected()`
  still reports the sequence actually needed.
- **A duplicate inside a bracket is `Stale` and the staging survives it.** There
  is nothing ambiguous about a repeat — we already have it. Only a jump forward
  discards the run.
- **`ProtocolViolation` does not invalidate a healthy book.** A stray
  `SnapshotEnd` arriving while `Live` leaves the view intact and the next
  in-order level applies normally. Discarding a synced book over one misplaced
  frame would be a self-inflicted outage.
- **`Gap → NotSynced` cannot happen.** Only a cold start that never synced
  reports `NotSynced`, and only a loss after a commit reports `Gap`.

### The freshness gate

`snapshot_fresh(seq) = seq >= expected()`

This is the pipeline's own guard, not the book's. **`load_snapshot()` has no
staleness check and will happily rewind a book to an older sequence.**
Committing a stale bracket would move `last_applied` backwards and then
double-apply every message between the two positions, with no gap firing to
reveal it.

The comparison is against `expected()` — the watermark, the next sequence the
pipeline requires — and **not** against `book.last_applied_seq()`. While `Live`
those two differ by exactly one, and the difference is the whole point:
`last_applied_seq()` is the last sequence already *consumed*, so gating on it
accepts a `SnapshotBegin` numbered at a sequence we have already used. That is a
frame behind the watermark, and a frame behind the watermark is stale whatever
its kind. This is not a hypothetical: the gate was written against
`last_applied_seq()` in Phase 1 and was corrected — see defect 7 below.

Because `expected()` is derived rather than stored, this is **one rule with one
meaning in every state**: while a bracket is open it is the bracket's own cursor,
and otherwise it is the book's. So

- a `SnapshotBegin` at or ahead of the watermark opens (or restarts) a bracket;
- one behind it is `Stale`, *including* one numbered at the last applied
  sequence;
- while a bracket is already open, a nested `SnapshotBegin` behind **the
  bracket's own** cursor is also `Stale`, and the run in progress survives it —
  exactly as it survives a duplicate level.

The gate is applied in **every** state, including `Gap`. A repair bracket older
than the view that was lost is not a repair: it would restore an old book and
then replay old messages over it.

One consequence worth stating because it looks surprising: **a gap does not
advance the book's cursor.** A repair bracket must therefore begin at the first
sequence the book still needs — `cursor + 1` — or later. A bracket beginning
*exactly at* the lost cursor is refused. Both boundary cases are pinned by
scenario vectors, in `Live` and in `Gap` alike.

### Recovery accounting

One `MdRecoveryEpisode` per outage, opened at the first loss and closed by the
commit that ends it. Globals: `outages`, `recovered_outages`.

| Field | Meaning |
|---|---|
| `cause` | `SeqJumpLive`, `SeqJumpInSnapshot`, `MalformedContentLive`, `MalformedSnapshot`. |
| `first_missing_seq` | The first sequence the **book** can no longer be advanced to — its cursor plus one at the moment the outage opened. |
| `gap_detect_seq` | The sequence that revealed the loss. |
| `lost_span` | `gap_detect_seq - first_missing_seq`. |
| `max_seq_seen` | Highest sequence observed while the episode was open. |
| `discarded` | Levels `Rejected` while out of sync, **plus** staged levels thrown away when an attempted repair was abandoned. |
| `attempts` | Brackets **accepted** while this outage was open. |
| `recovery_begin_seq` / `recovery_end_seq` | The Begin of the most recent attempt, and the committing End's sequence. |
| `recovered` | Whether a commit closed it. |

`first_missing_seq` is read from the **book**, never from the bracket that
happened to fail. A mid-stream refresh bracket carries its own cursor, which
runs *ahead* of the book's; reporting that one would understate the outage by
every sequence between the two. Nothing between a loss and the read moves the
book — `apply()` leaves the sequence unconsumed on both `GapDetected` and
`InvalidUpdate`, and staging never touches the book at all — so the read is
unambiguous.

An outage that is already open is **never reopened**: it keeps the position and
cause of the loss that opened it and only accumulates state. One outage, N
attempts.

Two consequences of these definitions, both pinned by tests:

- The bracket whose failure *opened* an outage was accepted before the outage
  existed, so it is not counted in `attempts`. An outage opened by a failed
  mid-stream refresh therefore reports `attempts == 0` and `recovery_begin_seq
  == 0` until some later bracket is accepted.
- A stream that **ends** mid-outage leaves the episode open, and an open episode
  is deliberately **not** copied into `episodes()`. A truncation test must be
  able to see `recovered == false`, and it could not if the episode were only
  visible after being closed — which never happens. `open_episode()` exposes it.

`open_episode()` is non-null whenever a view is lost and not yet restored, which
**includes** the interval spent assembling a repair bracket, where `state() ==
Snapshot`. It is *not* "non-null iff `Gap`" — that reading would hide the outage
for exactly as long as the repair is in flight.

### The accounting identity

```
messages == applied + out_of_range + staged + snapshot_committed
          + snapshot_abandoned + stale + gap_detected + malformed
          + rejected + protocol_violations
```

Every message has exactly one outcome, always. This holds **by construction**:
every result the pipeline returns is built in one private `note()` function, and
that function is the only place an outcome counter is incremented. The switch is
exhaustive over `MdOutcome`, so a new outcome will not compile until it is
counted.

Diagnostics are *not* counted there — `sync_attempts`, `staging_limit_hits`,
`nested_brackets_discarded` and the rest are not the outcome of any single
message. `nested_brackets_discarded` exists precisely because a nested
`SnapshotBegin` discards a bracket while the message that caused it has its own
outcome (`Staged`); counting the discarded bracket under `snapshot_abandoned`
would make the identity overshoot by one per nested bracket.

## Two inherited asymmetries

Both are inherited from the existing repository contracts, not introduced here.
They are pinned as defined behaviour and documented rather than papered over.

1. **A repeated price inside a snapshot rejects the whole snapshot.**
   `validate_snapshot` treats a duplicate price on a side as ambiguous state. A
   venue that repeats a price costs a full snapshot. Note that a price listed
   twice *including once with `qty == 0`* is still listed twice and still
   refuses the run — the duplicate scan includes zero-quantity entries. The same
   price on **opposite** sides is not a duplicate.

2. **An out-of-domain price behaves differently on the two paths.** Inside a
   snapshot it rejects the whole run; as an in-order incremental it is ignored
   with the sequence consumed (`ApplyResult::OutOfRange`). A banded book
   deliberately does not cover every price, so a live out-of-range level is not
   a loss — but the snapshot contract is all-or-nothing. The two contracts are
   both correct for their own path; they are not unified, and this doc does not
   claim they are.

## Verification

Six layers. Each is stated with what it can and cannot establish, because a
correctness claim is only as strong as the layer that supports it.

### Layer 1 — the specification sketch, verbatim

The state-machine sketch that specified this phase is a **literal expected
outcome table** in the test suite: 13 messages, 13 expected outcomes, read off
the sketch rather than off the implementation. This is the **only** layer that
can catch a specification error, because its expectations were written by hand.

It pins the two details the sketch exists to specify: the offending `seq=9` is
`GapDetected` and **not applied**, `expected()` is still 7 afterwards, and the
`seq=10` that follows is `Rejected` because the pipeline is in `Gap`. The gap is
never papered over.

### Layer 2 — scenario vectors

**28 hand-written traces**, 112 checks, one per failure hypothesis, each with a
full expected outcome vector *and* an expected final state, `expected()`, and
top of book. Covered: the sketch; an empty snapshot; `SnapshotEnd` as the very
first message; a snapshot that never ends; a gap inside a bracket; a nested
`SnapshotBegin`; duplicates and replays while `Live`; an idempotent delete of an
absent level; `qty == 0` inside snapshot content; a duplicate price inside a
snapshot; an out-of-domain price inside a snapshot; negative `qty` inside a
snapshot; a negative `qty` on a **live** level; an out-of-domain price on a live
level; a stale `SnapshotBegin` while `Live`; a long-lost stream resumed by a
bracket; the same price on both sides of a snapshot; a price listed twice
including a zero quantity; and `ProtocolViolation` leaving a healthy book intact.

A group of them pin the freshness boundary and exist because the fuzz cannot —
see defect 7. They come in pairs, because a boundary is only pinned by testing
both sides of it:

- `SnapshotBegin` **at** the last applied sequence while `Live` → `Stale`, with
  the healthy book untouched, immediately followed by the same frame one
  sequence later → accepted;
- a repair bracket **at** the lost cursor → `Stale`, and **at the watermark** →
  accepted and recovers;
- a nested `SnapshotBegin` **behind** the bracket's own cursor → `Stale`, with
  the run in progress surviving it.

### Layer 3 — accounting

43 checks. The identity above, asserted on targeted multi-attempt traces, on a
stream that ends mid-outage, and on the whole corpus. Plus the episode fields,
computed by hand.

The suite additionally asserts that the corpus **reaches** every one of the ten
outcomes, every one of the nine diagnostics, all four states and all four loss
causes. This is the layer that keeps layer 4 honest: a differential test where
both implementations agree on a counter that is permanently zero proves nothing
about that counter. A coverage assertion is what makes "we compared them"
different from "we compared them on something".

### Layer 4 — differential fuzz

**1550 seeded traces** run through the pipeline and through
`market-data-pipeline/tests/md_oracle.h`, an independently written reference
implementation, requiring identical outcomes, states, `expected()`, all
twenty counters, the full level set, and every episode field.

The oracle shares the *vocabulary* — `MdOutcome`, `MdState`, `MdCounters` are
the things being talked about — and no *logic*. It is a different book (flat
vectors with linear scans), different staging (one interleaved list rather than
per-side parallel arrays), hand-derived snapshot validity rather than
`validate_snapshot()`, an inline `apply()` precedence chain rather than the
book's own `apply()`, and one flat `if`/`else` chain rather than dispatch to
per-kind helpers.

The corpus is the four base scenarios × all 15 mutations individually, plus
seeded composition of 0–4 mutations over 250 seeds.

### Layer 5 — sink agreement

**300 traces** through `MarketDataPipeline<MapOrderBook>` and
`MarketDataPipeline<FlatOrderBook>` — the same corrupted stream, two different
sinks — requiring identical outcomes, counters, position, top of book, and full
level-set parity by enumerating the map book's keys and querying the flat book
for each. Agreement on the top of book is not agreement on the book.

### Layer 6 — generator determinism

25 checks. The same `(scenario, seed, recipe)` must produce the same bytes, and
different seeds must actually explore differently. Failure output prints the
recipe (scenario, seed, mutation list) and the offending message, so a
divergence found by the fuzz is reproducible and can be frozen into a named
regression with `build_with_recipe`.

Generator determinism is load-bearing, not stylistic. `std::mt19937_64` is
exactly specified by the standard and is used; `std::uniform_int_distribution`
is **not** — its mapping from engine output is implementation-defined, so the
same seed can produce different streams on libstdc++ and libc++. It is
deliberately avoided in favour of a hand-rolled `uniform_below`. (`benchmark/`
`stream_gen.h` may use the standard distributions; it pins no golden values.)

### What each layer cannot do

- Layers 4 and 5 catch **implementation divergence**, not **specification
  error**. If the pipeline and the oracle misread the contract the same way,
  they agree and are both wrong. That is what layers 1 and 2 are for, and it is
  not hypothetical — see below.
- Layer 5 is weaker than it looks in one direction: both flat books were written
  against the same contract, so it tests the *pipeline's* sink-agnosticism, not
  the books' correctness. The books have their own suites.
- Layer 3 makes the counters *complete*, not the *semantics* correct. A wrong
  transition can still be counted consistently.

## Defects found and fixed during this phase

Recorded because they are evidence about the verification, not just about the
code.

1. **The accounting identity did not hold.** `SnapshotBegin` returned `Staged`
   without incrementing `staged`, and three separate stale paths returned
   `Stale` without incrementing `stale`. The identity was therefore off by
   exactly the number of brackets and stale frames in a trace.

2. **The differential fuzz could not see it.** The oracle had been written from
   the pipeline and inherited the same omission, so all 1550 traces agreed while
   both were wrong. This is the layer-4 limitation above, caught in practice by
   layer 3 — and it is why outcomes are now counted in exactly one place.

3. **`attempts` double-counted.** Abandoning a bracket incremented `attempts` a
   second time for a bracket `on_begin` had already counted, so a two-bracket
   outage reported three attempts.

4. **`first_missing_seq` was taken from the bracket, not the book.** For a
   failed mid-stream refresh it reported the bracket's own cursor — e.g. 7 when
   the book's real position was 5 — understating the outage by every sequence
   staged inside the bracket.

5. **`discarded` did not implement its own definition.** Staged levels thrown
   away by an abandoned repair were never charged to the outage, although the
   documented definition says they are.

6. **Two generator mutations tested nothing.** `NegativeQty` and
   `OutOfDomainPrice` targeted the first `Level` in a trace, which is always
   inside the opening snapshot — so they exercised snapshot refusal while
   `malformed` and `out_of_range` stayed permanently at zero. Fixed to target
   the first level that arrives while `Live`. The coverage assertion found this;
   the differential fuzz could not have.

7. **The freshness gate was one sequence too lenient.** `snapshot_fresh`
   compared against `book.last_applied_seq()` — the last sequence *consumed* —
   instead of `expected()`, the watermark. The two differ by exactly one while
   `Live`, and this accepted a `SnapshotBegin` numbered at a sequence already
   used. A frame behind the watermark is stale whatever its kind. Found in
   review, after the phase had been declared complete.

   **The differential fuzz reported full agreement — 1550 of 1550 traces — with
   the defect present**, because the oracle had been written from the pipeline
   and inherited the same off-by-one. This is defect 2's failure mode occurring
   a second time, in a phase whose documentation already named it. The fix was
   re-validated by reverting *both* implementations together and confirming that
   the fuzz still reports `[ok] differential vs oracle (1550 checks)` while the
   scenario vectors fail 7 of 112 — the harness proving its own limits.

The verification harness was itself checked by deliberately breaking the oracle
and the pipeline and confirming each break is caught: the freshness gate read
against the last consumed sequence instead of the watermark, `first_missing_seq`
taken from the bracket, a wrong staging-cap comparison, and the historical
counting omission. Each is detected. The staging-cap case is only detectable
because the suite runs part of the corpus with a small cap — at the default cap
of 2²⁰ a ~150-message trace never approaches it. The first of those four is
detected **only** by the scenario vectors; the differential passes it.

## Runtime and reproducibility

```sh
cmake -S . -B build-exp03 -DCMAKE_BUILD_TYPE=Release
cmake --build build-exp03 -j 8
ctest --test-dir build-exp03 --output-on-failure     # includes the _exitcode guard
./build-exp03/market-data-pipeline/market_data_pipeline_tests
```

The suite runs in well under a second, so it is not conditioned on anything and
runs in the default CTest set. Output is **byte-identical across two runs**;
there is no randomness outside the seeded generator, and no clock is read
anywhere in the phase.

Clean under **ASan** and **UBSan** (`-fno-sanitize-recover=all`, no
diagnostics). Builds warning-free under `-Wall -Wextra`.

The exit-code guard (`market_data_pipeline_tests_exitcode`) drives the binary
through `LLDB_SELFTEST_FAIL=1`, which runs only a deliberately failing `CHECK`,
and asserts a non-zero exit — so a regression that stopped propagating failures
would fail the suite rather than silently pass it.

## What cannot be claimed

- **No performance claim of any kind.** Nothing is timed, no benchmark binary
  exists, and no number in this document is a duration. Phase 1 is not evidence
  about throughput or latency, and must never be quoted as such.
- **No claim about behaviour under a real venue's feed protocol.** The message
  set is a minimal abstraction. Real feeds have per-instrument sequence
  channels, heartbeat and timeout semantics, and cancel/replace ordering rules,
  none of which are modelled.
- **No claim that the pipeline recovers in bounded time.** It waits for a
  snapshot. How long that takes is a property of the venue, not of this code,
  and this phase measures nothing about it.
- **No claim about the two inherited asymmetries being desirable.** They are
  documented as the repository's existing contracts. Whether a repeated price
  should cost a full snapshot is a design question this phase does not answer.
- **No claim about allocation or memory behaviour.** Staging is bounded by
  `max_staged_levels_per_side` (default 2²⁰ levels per side) and exceeding it
  abandons the bracket rather than truncating it, because a truncated snapshot
  is a wrong snapshot. But no allocation measurement was made.
- **The fuzz corpus is not exhaustive.** It is a fixed catalogue plus seeded
  composition. It explores combinations; it does not prove absence.

## Status

**Phase 1 — Sequencer / Correctness: COMPLETE / FROZEN.**

- `market-data-pipeline/include/md_message.h` — message vocabulary.
- `market-data-pipeline/include/market_data_pipeline.h` — the state machine,
  `MdResult`, `MdCounters`, `MdRecoveryEpisode`.
- `market-data-pipeline/include/md_stream_gen.h` — deterministic base scenarios,
  the named mutation catalogue, and the seeded grammar.
- `market-data-pipeline/tests/md_oracle.h` — the independent reference
  implementation.
- `market-data-pipeline/tests/market_data_pipeline_tests.cpp` — six suites, all
  green; CTest total 31/31 including the exit-code guard.

Phase 1 measures nothing and therefore publishes no results dataset. No
Experiment 01 or 02 file is modified: the only change outside the new directory
is one `add_subdirectory(market-data-pipeline)` line in the root `CMakeLists.txt`.
Phase 1 adds no benchmark target and no `BENCH_ARCH_FLAGS` entry, because it has
nothing to time. (Phase 3A later added both — at the repository root, beside
every other benchmark, never in this directory.)

**A later phase may add measurement** — ingress-to-book latency, sequencer cost
per message, or the cost of a snapshot commit — on top of this contract. No such
phase is opened here.

---

# Experiment 03 Phase 2 — Threaded Decoder → SPSC → Book Correctness

## What this phase adds — and what it does not measure

Phase 1A froze a sequencer and Phase 1B froze a byte decoder, and each was
verified **single-threaded**. What neither phase established is that the two
still compose when a thread boundary and a queue are placed between them. That
is the whole question here:

```
byte chunks ──▶ Decoder Thread ──▶ decode_one() ──▶ MdMessage
                                                        │
                        SpscSeparatedBaselineRingBuffer<MdMessage, Capacity>
                                                        │
                                                        ▼
                                       Book Thread ──▶ MarketDataPipeline<FlatOrderBook>
```

Four components are **integrated and reimplemented zero times**: Experiment
01's `FlatOrderBook`, Phase 1A's `MarketDataPipeline`, Phase 1B's `decode_one`,
and Experiment 02's frozen SPSC selection. No file in Experiments 01 or 02 is
modified, and no frozen Phase-1 header changes semantics — the only Phase-1 edit
in this task is a comment in `md_encoder.h` that had gone stale.

**This phase measures nothing.** No nanoseconds per message, no messages per
second, no latency percentiles, no timing call of any kind, no CPU affinity, no
thread pinning, and no capacity-performance matrix. Introducing a thread does
not create a measurement phase; it creates a new **failure mode** — loss,
reordering, torn state, and a consumer that exits one message too early — and
the question of this phase is whether the existing correctness survives it.

## Thread ownership

The design is a strict ownership split with exactly one shared object.

| State | Owner | Lifetime |
|---|---|---|
| byte chunk cursor (the caller's chunk source) | decoder thread | the run |
| `StreamDecoder` carry buffer and framing state | decoder thread | the run |
| SPSC **producer** side | decoder thread | the run |
| `MarketDataPipeline<FlatOrderBook>` and the book inside it | book thread | the run |
| SPSC **consumer** side | book thread | the run |
| producer statistics (`MdProducerStats`) | decoder thread | read after join |
| consumer statistics (`MdConsumerStats`) | book thread | read after join |
| `producer_done` | producer writes, consumer reads | the run |
| final state inspection | `run`'s caller | **only after both joins** |

Nothing is shared except the queue and the one completion flag, and neither
statistics struct is atomic — they are plain integers written by exactly one
thread and read by the caller only after `join`, where the join itself is the
synchronization edge. Adding atomics to them would be paying for a race that
the join already rules out, and would invite a reader to think the numbers are
safe to poll *during* a run, which they are not.

### Why the OrderBook remains single-writer

Experiment 01's `FlatOrderBook` has no lock, no atomic and no internal
synchronization, and this phase does not add one. It does not need one: the book
has **exactly one writing thread for the entire run**. That is the same argument
as the single-threaded case with a different thread in the role — a
single-writer object needs no synchronization because there is no second writer
to synchronize with. The caller constructs the pipeline before spawning the book
thread and touches nothing but the const accessors after joining, so the
"single writer" claim is a property of the ownership table above rather than a
convention.

The alternative — several consumer threads, or a caller that applies messages
itself — would require a lock on the book or a redesign of the book, and would
be measuring a different system.

### Why the book is not a template parameter

`ThreadedMdPipeline` hardcodes `FlatOrderBook`. It is deliberately not
templated, because the tempting move — push every threaded message into *both*
a flat and a map book from the consumer thread and require them to agree — would
change the runtime under test into a dual-book validation pipeline. The work
being exercised would stop being "decode, enqueue, apply" and become "decode,
enqueue, apply twice, compare", and any timing-sensitive property measured later
on that shape would describe the comparison harness, not the pipeline. The
cross-book agreement check already exists where it belongs: in Phase 1, single
threaded, against `tests/md_oracle.h`.

## The SPSC boundary

```cpp
using Queue = lltl::SpscSeparatedBaselineRingBuffer<MdMessage, Capacity>;
```

which is `RemoteCursorRingBuffer<MdMessage, Capacity, RemoteCursorMode::Direct,
false>` — the **final frozen Experiment-02 selection**, the uncached
separated-cursor baseline. Not the cached-remote-cursor variant, not a
cursor-layout variant, not an instrumented build, and not a copy: the header is
included and instantiated. `Capacity` must be a power of two, which is asserted
at compile time rather than documented and hoped for.

No lock is added around the queue, no CAS is introduced, and no memory order,
cursor placement or capacity semantic is altered. Phase 2 is a *user* of that
queue, not a revision of it.

## Partial frames: the carry

`decode_one` reads one message from the front of a span and answers
`NeedMoreData` for a prefix — but it holds no state, so it cannot itself
remember a prefix across calls. A caller looping over socket reads needs one
more thing: somewhere to keep the tail of a frame that got cut in half.

`StreamDecoder` is that thing and nothing else. It does the framing; it does not
parse. Every complete frame it assembles is handed to the frozen `decode_one`,
so there is exactly one implementation of the binary layout in the repository
and no second opinion about byte order, field widths or the sequence domain.

The carry is a **fixed** `std::array<std::byte, 29>`. The largest message the
protocol defines is `kLevelMessageSize`, so a partial frame is at most 28 bytes
and one array holds every prefix that can exist. A growing receive buffer would
allocate on the steady path and would put an unbounded, remotely-driven size in
the hot loop for no benefit. Consequence worth stating: at rest `carry_size()`
is always `< 29`, because 29 bytes is always either a complete Level or a
terminal length error — one of which leaves the carry empty. The suite asserts
exactly that across a byte-at-a-time feed.

The steady state — a chunk that begins and ends on frame boundaries — copies
nothing at all. That is the reason for the two-path structure: with the carry
empty, the chunk is decoded in place; only a retained prefix pays for a copy,
and only up to 29 bytes.

The carry is also the only state the framer has, which is what makes
`finish()` — the EOF check that distinguishes a stream waiting for more bytes
from a stream that will never get them — a single comparison on `carry_size_`.
See *Truncated input is not a clean session* below.

## Backpressure: no message is ever dropped

Every decoded message is pushed with **retry until success**. A full queue makes
the producer spin, never discard, because a dropped inbound message is not a
lost update — it is a lost *sequence number*, which the pipeline downstream can
only read as a gap, forcing a full snapshot recovery the feed never asked for.
Dropping under load would convert a transient queue-full into a self-inflicted
outage, and the recovery would be far more expensive than the spin.

The retry policy is Experiment 02's, unchanged:

```
on failed push:  ++producer_full_retries; ++consecutive
                 after 1024 consecutive failures: yield, reset consecutive
on success:      reset consecutive
```

and the consumer mirrors it with `consumer_empty_retries`. The counter is
*consecutive*, not cumulative: a producer that is keeping up never yields,
however many times it retried over the course of a session.

`producer_full_retries` and `consumer_empty_retries` are **diagnostic
correctness context, not a performance result**. Nothing in this phase is timed,
and no retry count here is a throughput or latency claim. They appear in test
output only as evidence that the path was exercised at all.

## Malformed wire is terminal

A session can end badly in two different ways, and this phase keeps them
**distinct** rather than collapsing them into one "error" flag:

| | MALFORMED | TRUNCATED |
|---|---|---|
| what happened | the bytes present *prove* the frame is not a message of this protocol | the byte source ended while a partial frame was still retained |
| what is wrong | the data | nothing — the stream simply stopped |
| depends on bytes not yet arrived? | no | yes; mid-stream this state is the ordinary wait |
| detected by | `feed` returning an `Invalid*` status | `finish()`, and only at a declared EOF |
| how the session ends | immediately, terminally | the prefix is delivered, then the session ends |
| resynchronisation | not attempted (see below) | not applicable — there is nothing to resync |

The next two sections take them one at a time. This one is MALFORMED.

The decoder thread distinguishes two things that both look like failure:

- **`NeedMoreData` is not an error.** It is the ordinary answer for a partial
  read, and in this design it never even escapes `feed`: a partial frame becomes
  the retained carry, and `feed` returns `Ok`. A chunk boundary landing
  mid-message is a normal event, not a fault. Calling it an error would classify
  the commonest thing a socket does as a failure.
- **Every other non-`Ok` status is terminal.** The bytes are malformed as a
  property of themselves; re-reading them cannot help. The decoder thread
  records the status, publishes nothing further, and **stops accepting that
  session**.

What it deliberately does **not** do is resynchronise. Skipping a byte and
continuing is the obvious-looking alternative and it is rejected on a specific
ground: `consumed` is 0 on malformed input, so the protocol offers no defensible
resynchronisation point. Picking one is guessing, and a wrong guess silently
converts a detectable corruption into a plausible-looking stream — the failure
becomes invisible in exactly the case where visibility matters most. The bytes
after a malformed frame may well be a valid message; without a framing-level
resync protocol there is no way to *know* they are aligned, and treating them as
aligned is an assumption, not a recovery.

## Truncated input is not a clean session

The mirror case is the one `feed` cannot see. A frame cut in half is the
*ordinary* state of a framer mid-stream — it is what the carry is for — so a
stream that never delivers the rest looks, from every per-chunk return value,
exactly like a stream that is about to. The decoder thread stops calling `feed`,
the carry still holds bytes, and nothing in the return values ever said so.

That is not a hypothetical. It was a real defect in this phase's first cut, and
it had the worst shape a data-integrity bug can take: **a session that lost a
message reported a clean success.**

```
before the fix    terminal_error = false    terminal_decode_status = Ok
                  decoded=5  enqueued=5  consumed=5   ... and 20 bytes
                  of a real frame silently discarded
```

Everything visible was correct. The prefix decoded, the queue delivered, the book
matched the reference, all twenty counters closed, and the missing message
appeared in no count, no counter and no log. The session simply claimed to have
finished.

The fix is a **finalization step that only the caller can perform**, because only
the caller knows the source is exhausted:

```cpp
[[nodiscard]] DecodeStatus finish() const noexcept {
    return carry_size_ == 0 ? DecodeStatus::Ok : DecodeStatus::NeedMoreData;
}
```

and in the decoder thread, the empty-chunk branch asks it *before* declaring
success:

```cpp
if (chunk.empty()) {
    status = framer.finish();   // was: break, leaving status == Ok
    break;
}
```

`NeedMoreData` is the honest answer rather than a new status: those bytes *are* a
strict prefix of a message, and the only thing that makes them an error is that
nothing more will ever arrive. `decode_one` answers the same way for the same
bytes, so the two agree, and no fourth layer of validity is invented.

Four properties of `finish` are load-bearing:

- **It does not clear the carry.** A finalization that tidied up by dropping the
  partial frame and returning `Ok` would *be* the bug, wearing a different hat.
  Because it only reads a field, the same decoder that reports `NeedMoreData` at
  a cut goes on to report `Ok` once the remainder arrives — pinned for every one
  of the 28 possible cut positions.
- **It allocates nothing and parses nothing.** It is one comparison on one
  member. It cannot fail, so it cannot itself introduce a way for a session to
  end badly.
- **It is `const`.** A query, not a transition: asking twice says the same thing
  and changes nothing, so it is safe to call from a `const` context and safe to
  call twice.
- **Calling it early is harmless.** On a live socket mid-stream, `finish()`
  returning `NeedMoreData` means "the stream is currently mid-frame", which is
  true and is not a claim about the future. The meaning is supplied entirely by
  the caller having declared EOF. That is exactly why the caller, not
  `StreamDecoder`, decides.

**The valid prefix is never disturbed.** Everything decoded before the truncated
frame was already pushed, and is drained normally; the partial frame is never
published, so it reaches neither the counts nor the book. The precedent is
Phase 1B's composed invariant, unchanged:

> decode failure → no typed message is published → no sequencer or book mutation

A truncated frame is not a decode failure — it produced no verdict at all — but
the same invariant covers it, for the same structural reason: a message that was
never published cannot be applied.

### How this is tested

Two layers, because the defect had two halves — a missing call and a missing
answer — and each half fails differently.

**Unit, single-threaded** (suite *stream finalization*, 482 checks). Exhaustive
rather than sampled, because the range is 28 bytes wide: every 1..11 prefix of a
12-byte header, every 1..28 prefix of a 29-byte Level (the cuts named in the
brief — 12, 13, 20, 28 — are inside that range), and every exact frame boundary
including the empty stream. The clean cases are the negative control: a `finish`
that answered `NeedMoreData` unconditionally would pass every truncation case and
still be worthless, so it is sabotaged and must fail here.

**End-to-end, threaded** (suite *truncated input at EOF*, 952 checks). A valid
prefix — `SnapshotBegin 1`, two levels, `SnapshotEnd 4`, one live update — reaches
`LIVE`, then the byte source stops inside a `Level` whose price would be plainly
visible in the book if it were ever published. Every one of the 28 cut positions,
under four chunk plans (one large chunk, one byte, the awkward plan, a seeded
random plan), must produce:

```
terminal_error         == true
terminal_decode_status == NeedMoreData
decoded == enqueued == consumed == 5      the prefix, and nothing else
final state            == the single-thread reference of the PREFIX ALONE
```

The second half of that is what keeps the first half honest. A "fix" that dropped
the last valid message, reported an error on clean input, or disturbed the
prefix's effect on the book would satisfy the truncation assertion and fail here.
The negative control runs the same prefix with no truncated frame appended under
all four plans and requires a clean `Ok` — so neither answer can be hardcoded.

## Termination, and the happens-before argument

End-of-input is a runtime fact, not a market-data message, so it is **not** a new
`MdKind` and not a wire message. Adding `EndOfStream` to the protocol would put
a control-plane concept into the typed vocabulary every layer shares, and would
make the wire format depend on how a particular process happens to be
structured. Instead the producer publishes one atomic flag after its last
successful push:

```cpp
producer:  ... all pushes done ...
           producer_done_.store(true, std::memory_order_release);

consumer:  ... try_pop fails ...
           if (producer_done_.load(std::memory_order_acquire)) {
               final try_pop;  if that fails too, exit
           }
```

**The happens-before argument.** The release store and the acquire load form a
release/acquire pair on the same atomic, so everything sequenced-before the
store in the producer happens-before everything sequenced-after the load in the
consumer. Every `try_push` the producer will ever perform is sequenced before
that store; therefore every one of them happens-before the consumer's
subsequent `try_pop`. Two consequences follow, and they are the whole protocol:

- The consumer's final `try_pop` observes the queue as of **after** the
  producer's last push. A failure there is not a race that more yielding could
  win — it means the queue is empty and no further push can ever make it
  non-empty.
- The flag is what makes that emptiness **final**. The queue's own
  release/acquire on its cursors is what makes pushed payload bytes *visible*;
  the flag is orthogonal to that and does not duplicate it. A push does not need
  to be ordered against the flag by the queue — the ordering comes from the
  release/acquire pair, not from the queue's cursors.

`queue.empty()` is deliberately **not** the mechanism. A queue that is empty now
may be non-empty a microsecond later, so emptiness alone can never terminate a
consumer; only "empty *and* the producer has stopped" can. Using emptiness as
the sole signal is the classic lost-final-message bug, and the termination suite
pins it directly.

## The composed invariant, now across threads

Phase 1B established, single-threaded:

> decode failure → no typed message is published → no sequencer or book mutation

Phase 2 has to preserve that across the thread boundary, and it does so
structurally rather than by a check: the decoder thread's only path to the book
thread is `queue_.try_push`, and a message that failed to decode is never handed
to the sink, so there is nothing to push. The malformed-wire suite asserts the
consequence — the producer's decoded and enqueued counts both equal the valid
prefix length, the consumer consumes exactly that many, and the final book
equals the single-threaded reference computed from the prefix alone.

## Verification

Eight suites in `market-data-pipeline/tests/md_threaded_pipeline_tests.cpp`, all
green:

| Suite | What it pins |
|---|---|
| stream framing | chunk-boundary semantics, single-threaded, against the frozen `decode_one` |
| chunk plan equivalence | the same encoded stream under 6 chunk plans must decode identically |
| threaded vs single-thread reference | the threaded run must match the Phase-1 result on every observable value |
| backpressure, no message dropped | capacity 2 and 4, consumer gated until the ring is provably full |
| termination keeps the final message | a message pushed immediately before `producer_done` must still be consumed |
| malformed wire terminates the session | a terminal status ends the session without resynchronisation, after the valid prefix is delivered |
| stream finalization | every partial prefix is `NeedMoreData` at EOF; every exact frame boundary is `Ok` |
| truncated input at EOF | a session that stops mid-frame is reported as truncated, and the prefix it did deliver still matches the reference |

The **reference is single-threaded**: the same messages applied straight to a
`MarketDataPipeline<FlatOrderBook>` with no queue and no threads, then compared
field by field — state, `last_applied_seq`, `expected`, best bid, best ask, level
count, and all twenty counters by name. The threaded path therefore has an oracle
that shares none of its machinery, instead of being compared against itself.

That comparison is doing more work than "no crash". The pipeline is
**order-sensitive by construction** — a duplicate, a stale or an out-of-order
sequence produces different outcomes and different counters — so a threaded run
that matches the reference on every counter *and* the final book can only have
delivered the messages in exactly the order they were decoded. A single swap
would move `applied`, `stale` or `gap_detected` and change the book. That is the
FIFO claim, tested by consequence rather than by inspection.

The fixture is written out message by message rather than generated, so it is
legible which case is which; it exercises an initial multi-level snapshot, live
incrementals, a delete, a stale replay, a sequence gap, incrementals suppressed
while out of sync, a recovery snapshot, and a live incremental after recovery.
The reference's **counts are asserted as predictions** (20 messages, 10 staged,
4 applied, 2 snapshot commits, 1 stale, 1 gap, 2 rejected, 1 outage, 1 recovery)
rather than observed from a run, so the fixture cannot quietly degrade into a
trivial one whose threaded result would match for the wrong reason.

### Backpressure is made deterministic, not waited for

Waiting a while and hoping the ring filled is not a test. The producer sets a
test-visible flag the first time a push finds the ring full, and the consumer
thread spins on a **test-only gate** before its first pop. The test releases the
gate only after observing that flag, so the ring is provably full and the retry
path has provably run before the consumer takes anything. Neither the gate nor
the flag is part of the production algorithm: with no gate set, the pipeline runs
identically without them, and they exist so that the ordering of the pressure is
a fact rather than a probability.

The wait for that flag is bounded, and the bound is deliberately **small** — it
is not patience, it is a failure detector, so that a producer which never
signals (because it dropped instead of retrying, say) is reported as a failed
check rather than hanging the suite. The threaded CTest entry also carries a
120-second timeout, because a hang in the consumer loop is a realistic failure
mode for this phase and should report as a test failure rather than stalling CI.

### Sabotage results

A green suite is not evidence until it has been shown to fail. Eight deliberate
defects were introduced, one at a time, and reverted:

| Sabotage | Detected by |
|---|---|
| drop the message when the push finds the ring full | backpressure suite, deterministically, both capacities |
| remove the decisive final `try_pop` after `producer_done` | termination suite |
| treat a terminal decode status as non-terminal and continue | malformed-wire suite |
| return `NeedMoreData` instead of retaining the carry | framing suite and every chunk-plan run |
| (found in the harness) a wait bound of 2×10⁹ iterations | **not** a detection — it hung instead of failing, and was reduced |
| ignore the carry at EOF — the pre-fix code | truncated-EOF suite, 112 checks |
| `finish()` always answers `Ok` | finalization suite (95) and truncated-EOF suite (112) |
| `finish()` always answers `NeedMoreData` | finalization suite (39), truncated-EOF suite (8) and the pre-existing reference suite (8) |

The last three are aimed at the two halves of the truncation fix — the missing
call and the missing answer — plus the boundary itself, and each fails in a
different place. **Ignoring the carry at EOF fails only the truncated-EOF
suite**: the finalization unit tests still pass, because `finish()` is correct
and it is the *call site* that is wrong. That separation is the reason the two
suites exist rather than one, and it is the same shape as the defect itself,
which was a correct framer inside a caller that never asked it anything.

The third sabotage is the one that justifies the negative controls. A `finish`
that always answered "truncated" would satisfy every truncation assertion in the
phase — which is why the clean-stream cases are pinned in the finalization unit
suite, in the truncated-EOF suite's negative control, and (already, from the
first cut) in the threaded reference suite's `!terminal_error` checks. It fails
in all three.

The third and fourth are the interesting ones. Under a *single* chunk plan the
terminal-status sabotage was caught only by the status checks, because the bytes
following the malformed frame happened to resynchronise onto nothing decodable —
the message counts looked innocent. Running the same malformed stream under all
five chunk plans is what turns "the session ended" from a claim about a flag into
a claim about the messages and the book: under two of those plans a byte-skip
invents messages. The suite was strengthened for that reason, and re-sabotaged to
confirm it now fails behaviourally.

The fifth is a defect in the *test*, found by the sabotage of the code — the same
thing that happened in Phase 1B, when sabotaging the decoder exposed a
malformed-case frame that was 21 bytes instead of 29 and had been passing
vacuously.

## Sanitizers

| Sanitizer | Result |
|---|---|
| ASan (`-fsanitize=address -fno-omit-frame-pointer -g`) | clean; 35/35 CTest (the Phase-2 set) |
| UBSan (`-fsanitize=undefined -fno-sanitize-recover=all -g`) | clean; 35/35 CTest (the Phase-2 set) |
| **TSan** (`-fsanitize=thread -fno-omit-frame-pointer`) | **runs, and reports no races** — 20 consecutive runs of the threaded suite, and the full CTest set, with zero diagnostics and zero non-zero exits |

TSan is the one that matters for this phase, and it is reported as a real result
rather than assumed: the toolchain here builds and runs it, so the claim is
"TSan ran and found nothing", not "TSan was unavailable". The threaded suite is
run repeatedly under it because a race that manifests once in twenty runs would
otherwise be reported as a pass.

**A positive control, because a clean TSan report is only evidence if the tool
was looking.** Zero diagnostics could equally mean "no races" or "the threads
were never instrumented". So TSan's ability to see a race in *this* binary was
demonstrated rather than trusted: a write to the non-atomic
`producer_.decoded_messages` was injected into `book_thread()`, where it races
with the decoder thread's increment. TSan reported six data races, each pointing
at `ThreadedMdPipeline<1024ul>::book_thread()` — the sabotaged thread — with the
decoder thread named as the other party. The sabotage was then reverted and the
clean result re-confirmed. Both worker threads are genuinely under
instrumentation, so the zero above is a negative and not a blind spot.

## What cannot be claimed

- **No performance claim of any kind.** No number in this section is a duration
  or a rate. The retry counters are evidence that a path was exercised; they are
  not throughput, and comparing capacity 2 against capacity 4 on them would be
  meaningless.
- **No claim that the queue is the right one for this workload.** Its selection
  was Experiment 02's result under Experiment 02's workload; Phase 2 uses it as a
  frozen component and makes no claim that it is optimal here.
- **No claim about a real socket.** The chunk source is in-process and
  deterministic. Real ingress has partial reads, backpressure at the NIC, and
  arrival jitter, none of which are modelled.
- **No claim about fairness or scheduling.** Nothing is pinned, nothing is
  affinity-bound, and the retry policy is a politeness heuristic, not a
  latency guarantee.
- **No claim that malformed input is *recoverable*.** This phase ends the session.
  Wire resynchronisation is not implemented, and no statement is made about how a
  real feed handler should do it.
- **No claim to detect truncation *in a live stream*.** The caller must declare
  the source exhausted; until then, a stream mid-frame and a stream that has
  silently died are the same observation, and no protocol-level trick separates
  them. What this phase guarantees is narrower and honest: *given* that the byte
  source is finished, a session that ended mid-frame is reported as such rather
  than as a clean one. Detecting a dead peer needs heartbeats or sequence
  timeouts, which are not in this contract.
- **No claim that TSan exhausts the race space.** It reports races it observes on
  the schedules it produces; absence of a report is evidence, not proof.

## Status

**Phase 2 — Threaded Decoder → SPSC → Book Correctness: COMPLETE / FROZEN.**

- `market-data-pipeline/include/md_stream_decoder.h` — the framing component:
  arbitrary chunks in, complete messages out, at most one retained partial frame,
  plus `finish()` for the EOF question `feed` cannot answer.
- `market-data-pipeline/include/md_threaded_pipeline.h` — the ownership split, the
  producer and consumer loops, the retry policy, the completion protocol, and the
  EOF finalization that separates truncated input from a clean session.
- `market-data-pipeline/tests/md_threaded_pipeline_tests.cpp` — eight suites, 2952
  checks, all green, with the `LLDB_SELFTEST_FAIL=1` exit-code guard.
- `market-data-pipeline/CMakeLists.txt` — the new target, strict flags,
  `Threads::Threads`, the exit-code guard and a 120-second timeout.
- `market-data-pipeline/include/md_encoder.h` — comment-only: two stale claims
  corrected, no behaviour change.

Full CTest: **35/35** in a clean Release build with `BUILD_BENCHMARKS=ON`, under
ASan and under UBSan, and in the CTest set under TSan; including every
pre-existing Phase-1A and Phase-1B test. (That is the count as of Phase 2; the
repository total is 40 today, after Phase 3A added one benchmark target, one
smoke cell and four off-cell rejection guards.) No Experiment 01 or 02 file is
touched.
There is still no benchmark target **in this directory**, and no
`BENCH_ARCH_FLAGS` entry for one, because every phase described in this document
measures nothing. Phase 3A's benchmark lives in the root `benchmark/` directory
with every other benchmark in this repository, never in a correctness test
binary.

One correctness hole was found and closed after the phase's first cut, and it is
recorded in full under *Truncated input is not a clean session* above: a session
whose byte source ended mid-frame reported a clean success, dropping the partial
frame without a trace. That is why the phase's status was not stated as frozen
until the finalization call, its two test suites, and the sabotages that prove
those suites fail without it, were all in place.

**Phase 3A — Integrated Market-Data Pipeline Throughput Baseline: IMPLEMENTED /
MEASURED.** Phases 1 and 2 deliberately left the measurement surfaces alone: no
clock is read, no sample is collected, and no warm-up or steady-state concept
exists in any component described above. Phase 3A adds a measurement **around**
this contract without changing it — it integrates the frozen framer, the frozen
decoder, Experiment 02's frozen queue, the sequencer above and Experiment 01's
frozen book into one two-thread system and times the whole path end to end. It
modifies none of them.

Phase 3A measures **throughput only**, at session granularity: 34.909762
ns/message and 28,646,780 messages/second (median of four session medians) over
5,000,000 live Level messages, with a 3.676 % spread across session medians. It
is **not** a latency, **not** a percentile and **not** a per-message cost — no
component here gained a timestamp and no side timestamp array exists. See
`docs/MARKET_DATA_THROUGHPUT.md` for the timed boundary, the one canonical cell
and the limits on the result, and `docs/results/market-data-throughput/` for the
dataset.

**Phase 3 is NOT complete.** Phase 3B — per-message latency and percentiles — is
NOT STARTED, and is not opened here. Neither is any controlled comparison of
chunk sizes, queue capacities, books or queue variants: Phase 3A has one cell
and compares nothing.
