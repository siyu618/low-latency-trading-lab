# Experiment 03 Phase 1 — Market Data Pipeline (Sequencer and Correctness)

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
There is no benchmark target and no `BENCH_ARCH_FLAGS` entry, because there is
nothing yet to time.

**A later phase may add measurement** — ingress-to-book latency, sequencer cost
per message, or the cost of a snapshot commit — on top of this contract. No such
phase is opened here.
