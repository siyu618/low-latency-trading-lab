# Experiment 01 Optimization Study — Hierarchical-Occupancy Bitmap (control-isolated)

**WHAT this is.** An internal, post-Phase-4 study of one optimization idea for
the Experiment 01 flat order book: track each side's *occupancy* (which prices
hold a level) in a small hierarchical bitmap, so that after deleting the best
price the next best is found by a bounded word-level descent instead of
`FlatOrderBook`'s adjacent **linear re-scan** over empty slots. The candidate is
`BitsetFlatOrderBook` (`include/bitset_flat_order_book.h`), a semantics-identical
drop-in alternative. **`FlatOrderBook` is unchanged.**

**What this revision adds.** The first version compared `FlatOrderBook` directly
against `BitsetFlatOrderBook` and reported the delta as "what the bitmap costs."
That comparison was **confounded**: the candidate book differs from the frozen
book in *two* ways, not one — (a) a **transition-aware positive path** (it skips
cached-best bookkeeping when a write merely re-quantifies an already-present
level) *and* (b) the occupancy bitmap itself. A measured "bitmap" win could in
fact be entirely the control flow. This revision adds an experimental control —
`TransitionAwareFlatOrderBook` (`include/transition_aware_flat_order_book.h`),
the frozen book with only change (a), **no bitmap anywhere** — and re-measures
so each effect is attributed separately. The result changes the study's
headline: **on the frozen workloads almost all of the previously reported
speedup was the control flow, not the bitmap** (measured below).

**Research questions** (this document answers them from real measurements):

> Q1 — control flow alone: how much does the transition-aware positive path
> (`TransitionAwareFlat`, "tuned") change ordinary-update cost vs `Flat`?
> Q2 — bitmap alone: with the control flow held constant, what does the
> occupancy bitmap add or save (`tuned` vs `bits`)?
> Q3 — end-to-end: `Flat` vs `BitsetFlat`, read only as "control flow + bitmap",
> never as "the bitmap"?
> Q4 — at what next-best price *gap* does the hierarchical lookup beat the flat
> linear re-scan, and does that survive repeated rounds and a fixed-domain
> control?
> Q5 — do the frozen workloads ever reach that gap regime?

**Scope and frozen constraints.** Phase 1 (correctness), Phase 2 (throughput),
Phase 4 (tail latency), the A/B/C/D/E workload generators, `FlatOrderBook` and
`MapOrderBook`, and every canonical result file under `docs/results/` are
untouched and remain canonical. Nothing in this study is a Phase 5.

**Host / toolchain / date of every number in this document:** Apple M3 Max,
macOS 14.2.1 (23C71), Apple clang 15.0.0, Release `-O3 -DNDEBUG`, no arch flag.
Full provenance in `docs/results/orderbook-bitmap-optimization/host.txt`.
Frozen-dataset measurements 2026-09-09; control-isolation measurements
2026-09-09 (same host/build; the numbers below cite which run they came from).

---

## TL;DR (findings)

1. **The control flow, not the bitmap, was the frozen A/D speedup.** The
   transition-aware positive path alone (tuned, no bitmap) is a robust
   **≈ −10 % on A and −15 to −18 % on D**, **+25 to +28 % on B**, ~0 on C/E
   (both measurement methods agree on sign and magnitude). Adding the occupancy
   bitmap on top of that control flow gives **no robust win on any frozen
   workload**: the methods agree it is neutral-to-harmful on B/C/D/E, and on A
   they disagree (between −8 % and +18 %), so A is not robust. The frozen study's
   "bitmap faster on A/D" was the candidate book's *transition-aware path* doing
   the work — the bitmap was along for the ride.

2. **Measured crossover gap ∈ (4, 8], reproducible at g = 8 across 16
   independent rounds.** On a controlled ladder whose every best-delete rescans
   exactly `g` slots, the flat book costs ≈ **0.29 ns per rescanned slot** above
   a per-delete fixed cost; the bitmap book's best-delete is **far less
   sensitive to gap size** (≈ 0.006 ns per slot — roughly fifty times less). The
   median-over-rounds crossover is **g = 8** (bits faster 10/10 variable-domain
   and 6/6 fixed-domain rounds at g = 8; only 1/10 at g = 4). Per-round
   crossover distribution: min 4, median 8, max 8. A fixed-domain control
   (domain pinned so `g` cannot be a memory-span artifact) reproduces it
   row-for-row.

3. **The frozen workloads never reach that regime.** Only workload C deletes the
   best price at all, and **100 % of those best-deletes have re-scan distance
   ≤ 1** (900,944 best-deletes in a 2M-op run, 100 % adjacent; 450,467 @1M in
   the frozen gaps-analysis). A/B/E delete the best zero times; D does 18,
   distance ≤ 1. On the frozen A–E streams the bitmap's *designed* advantage —
   skipping a large gap — is never exercised.

4. **On ordinary updates the bitmap is not a fixed tax, but this revision finds
   no regime on the frozen workloads where the *bitmap* (over the control) is a
   reliable win.** Re-quantifying a present level touches the bitmap never and
   elides flat's best bookkeeping — but that elision is the control flow, which
   `TransitionAwareFlat` reproduces without the bitmap. What the bitmap *itself*
   adds is one occupancy word update per add/delete transition, which the
   delete/add-heavy workloads (B/C/E) pay for and requantify-heavy ones (A/D)
   mostly avoid. Measured net: robustly non-beneficial on B/C/D/E, ambiguous on
   A.

5. **Memory cost is real but small and unchanged:** the occupancy hierarchy is
   **507,952 B (0.48 MiB) per 1M-level book** vs 32,000,000 B of quantity
   arrays = **1.59 %**. It scales with the *addressable domain*, not book
   density. Reproduced by a memory helper, not assumed to be zero.

6. **Recommendation:** keep `FlatOrderBook` as the default. Adopt the
   transition-aware control flow **independently of the bitmap** where the
   production stream is requantify-heavy (A/D-like) and not delete-heavy. Do
   **not** adopt `BitsetFlatOrderBook` for the frozen workload shapes — it buys
   no robust steady-state win over the control and costs +1.59 % book bytes.
   Reserve the bitmap for books that can develop **sparse** best-delete gaps
   beyond ~8 ticks, where its best-delete advantage is real and measured and
   where delete churn is low enough that the per-transition occupancy write is
   not the dominant cost.

---

## 1. WHY — the research question and the confound it had

`FlatOrderBook` caches `best_bid_`/`best_ask_` so reads are O(1). Deleting the
level the cache points at must re-discover the next best. It does that with
`rescan_after_delete` → `scan_best_from`, a **linear walk of the quantity array**
starting at the adjacent slot and stopping at the first non-zero level
(`include/flat_order_book.h`). The walk examines exactly the number of empty
slots between the old best and the next present level — the *gap* `g`. When the
next level is adjacent (`g = 1` — workload C's common case) the scan is ~free;
when the side is sparse (`g` large) it is O(g) array loads.

The study idea: keep the same dense quantity array, but additionally summarize
occupancy in a bitmap so a best-delete can skip straight to the next set bit.

The **confound** this revision removes: `BitsetFlatOrderBook` is not
`FlatOrderBook` + bitmap. Its `apply()` also restructures the *ordinary
positive* path. The frozen book does cached-best bookkeeping on **every**
positive write — even a `positive → positive` re-quantify of a present, beaten
level, where that bookkeeping is provably dead work (a present non-best level
cannot become best by re-quantification: the best is a property of the occupied
price *set*, which a present→present write does not change). The bitmap book
instead branches on the occupancy **transition**: a `0 → positive` write (a
level appears) is the only positive write that can move the best, so it promotes
if needed; a `positive → positive` write is a pure quantity store touching
neither the bitmap nor the cached-best index.

So the first-version flat-vs-bits rows bundled two independent changes. To
attribute them, `TransitionAwareFlatOrderBook` ("tuned") reproduces the frozen
book *and only that one restructure*:

| comparison | what it isolates | implementation reality |
|---|---|---|
| `flat` vs `tuned` | transition-aware **control flow** alone | same contiguous qty arrays, same adjacent linear best re-scan on delete, same external semantics; only the positive path branches on `qty==0` |
| `tuned` vs `bits` | occupancy **bitmap** alone | same control flow and qty layout; bits additionally maintains the 3-level occupancy index and replaces the linear best-delete re-scan with hierarchical descent |
| `flat` vs `bits` | **end-to-end** candidate delta | control flow + bitmap; must NOT be read as "the bitmap" |

`FlatOrderBook` was **not modified**; `TransitionAwareFlatOrderBook` is a new,
deliberately non-production fixture class used only by the tests and benchmarks.
All three are verified four-way behaviorally identical (same op result, sync
state, sequence, best bid/ask price *and* qty, level count, live levels) on every
scenario and across every A–E stream at every scale (`--check`).

## 2. WHAT — the three implementations

All three books implement the identical `llob::` contract: integer-tick prices,
a single addressable band `[tick_min, tick_max]`, absolute quantities stored in
a contiguous `int64[]` (slot = `price − tick_min`), `qty == 0` deletes a level,
`best_*()` are O(1) cached reads, snapshot-driven resync, and the same sequence /
gap / invalid-update / out-of-range semantics. `BitsetFlatOrderBook` and
`TransitionAwareFlatOrderBook` are fresh classes — **no inheritance, no
virtuals, no polymorphic types** anywhere.

| | `FlatOrderBook` (frozen) | `TransitionAwareFlat` (control) | `BitsetFlatOrderBook` (candidate) |
|---|---|---|---|
| quantity storage | preallocated `int64[span]` per side | identical | identical |
| positive write path | store + cached-best bookkeeping on *every* write | branch on `qty==0`: `0→positive` promotes if needed; `positive→positive` = pure store | identical to tuned + occupancy set/clear on transitions only |
| occupancy index | none | none | 3-level bitmap (below) |
| find next best after best-delete | adjacent **linear scan** of empty qty slots, O(g) | identical to flat | **hierarchical descent** over set bits |
| best reads | cached, O(1) | cached, O(1) | cached, O(1) |
| apply() allocation | none after construction | none after construction | none after construction |

### The occupancy hierarchy (bits only; per side, over the same slot span)

```
L0   one bit per slot        — set  ⇔ qty[slot] != 0
L1   one bit per L0 word     — set  ⇔ that 64-slot L0 word is non-empty
L2   one bit per L1 word     — set  ⇔ that L1 word is non-empty
```

**Maintenance is transition-only** — the whole design point
(`include/bitset_flat_order_book.h`, `set_occ`/`clear_occ`):

- `0 → positive` (level appears): set the L0 bit; **if** that L0 word went
  `0 → nonzero`, set the L1 bit; propagate upward only as needed.
- `positive → 0` (level deleted): mirror image.
- `positive → positive` (re-quantify a present level): **the bitmap is not
  touched at all.** Re-quantifying a present level never stores a single bitmap
  byte.

Best-price discovery after deleting the current best:
- *Bid:* search the lower bits of the same L0 word (`msb` of a masked word);
  if empty, walk `L1`/`L2` to the preceding non-empty L0 word and descend.
- *Ask:* mirrored toward higher slots.
- No linear scan over empty price slots ever runs.

**Top-of-hierarchy fallback (documented, not hand-waved):** the descent is a
bounded few word steps in the common case (the next set bit lies in the same L0
word, or one `L1`/`L2` step away). It is **not always exactly O(1)..O(3)**: when
the search must leave the current L0 word at the coarsest level, `prev_occ2()` /
`next_occ2()` fall back to a **bounded linear walk over the adjacent L2 words**.
That walk is over *summary* words only — `occ2` has `ceil(L1words/64)` words per
side, **8 words at a 2M-slot-per-side domain** — never over the empty price
slots themselves. There is no L3 and no unbounded scan; the class comment and
these two functions document the bound precisely.

The whole per-side hierarchy at a 2N-slot domain is ≈ (span/64 + span/4096)
words — far smaller than the quantity array (see §5). The bitmap answers one
question only: *"highest occupied slot below / lowest above X?"* Quantities are
never encoded in it.

## 3. Measurement discipline and a host finding this revision must report

Methodology is the frozen study's, with the control added:

- **Per-process canonical** (Phase-2 convention): one implementation per
  process, best-of-reps inside the process, **median-of-rounds across 5
  processes**, impl start order rotated round to round (`run.sh`).
- **In-process interleaved delta** (drift-free): the selected implementations
  replay identical streams in *one* process, fresh snapshot per block, start
  order rotated per block, **median over 24–32 blocks** (`run_inproc3.sh`; the
  frozen two-way `--inproc` is the same idea without tuned).
- A finding is **robust** only when both methods agree on its sign — the
  robustness rule the frozen study already used.

**Limitation discovered while running (must be read before quoting any bits
number).** On this host the per-process `ns/apply` of the *bitmap* book is not
stable round to round even for byte-identical input in one session: `bits_A`
`best_ns_per_update` across the 5 rounds spanned **3.864–6.182 ns (±40 %)**
while `flat_A` stayed within **4.164–4.435 ns (±3 %)**. flat and tuned
single-process numbers are tight; bits is not (its larger footprint + 3-level
index make it the most sensitive to per-launch code/data alignment). Further,
the bitmap's absolute values drift *across measurement windows*: bits_C was
6.832 ns in the frozen two-way inproc and 8.864 ns in this three-way inproc;
bits_A cross-process was 3.108 ns frozen vs 4.604 ns here. flat's cross-process
values reproduce much better — A–D within ~±3 % (flat_A 4.296 frozen vs 4.376
here; flat_D 4.423 vs 4.474), E the loosest at ~+10 % (5.196 vs 5.712) — and
per-round dispersion is tight (flat_A 5 rounds within ±3 % vs bits_A within
±40 %). So flat-vs-flat rows are stable across sessions and tuned (measured only
in this session) is expected to share flat's layout stability; **bits rows are
not, and cross-session comparison of bits is meaningless.** Consequences used throughout this document:

- the in-process interleaved delta is the **authoritative** cross-impl signal
  (one clock state, one process image);
- no claim is called robust unless *both* methods agree on its sign;
- no absolute bits number is quoted as a point estimate — only deltas inside
  one measurement context, or medians with their observed dispersion.

Full raw values (5 per cell) are committed so the dispersion is inspectable.

## 4. Measured results

Raw output, commands, per-round logs, and provenance live under
`docs/results/orderbook-bitmap-optimization/` (§6). Every table cell below
appears in a committed file with its command and host next to it.

### 4.1 Correctness and hygiene (all green)

- **Four-way differential.** `tests/bitset_order_book_tests.cpp` (**116 checks**)
  runs every scenario against `MapOrderBook` (oracle), `FlatOrderBook`,
  `TransitionAwareFlatOrderBook` and `BitsetFlatOrderBook` and requires
  identical observable state (op result, sync state, sequence, best bid/ask
  price *and* qty) after every step — snapshot load/reject, sequence-gap
  desync, large-domain best deletion, an added suite hammering
  `positive → positive` re-quantifies of present non-best levels (the path the
  control-flow change restructures), and a random differential fuzzer with
  periodic resyncs.
- **Clean build, full ctest, clean sanitizers, clean strict warnings** (rerun
  2026-09-09 on freshly rebuilt binaries): `ctest` 5/5 in a fresh Release build;
  the same 5/5 under a fresh combined ASan+UBSan Debug build
  (`ASAN_OPTIONS=detect_leaks=0`); the new translation units compile clean under
  exactly
  `clang++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`
  (the two benchmark TUs and the four-way test TU — the latter instantiates all
  four books).
- **`orderbook_bitmap_bench --check`** replays every A–E stream at all four
  scales through flat, tuned and bits and requires agreement:
  `--check PASSED: Flat, Tuned, and Bits agree on every stream`.

### 4.2 Steady throughput on the frozen workloads, 1M levels — the isolation result

Two independent methodologies (per-process medians-of-rounds and in-process
interleaved). Three implementations, so each of the three study deltas is
reported on its own row pair.

**MEASURED** — per-process medians of `best_ns_per_update` over 5 rounds
(`control-isolation/steady-throughput-1M/medians.csv`); ns/`apply()`, lower is
better:

| wl | updates | flat | tuned | bits | flat→tuned % | tuned→bits % | flat→bits % |
|----|--------:|-----:|------:|-----:|-------------:|-------------:|------------:|
| A  | 2,000,000 | 4.376 | 3.911 | 4.604 | **−10.6** | +17.7 | +5.2 |
| B  | 500,000 | 4.470 | 5.750 | 7.252 | **+28.6** | +26.1 | **+62.2** |
| C  | 2,000,000 | 6.060 | 6.169 | 6.591 | +1.8 | +6.8 | +8.8 |
| D  | 2,000,000 | 4.474 | 3.672 | 3.772 | **−17.9** | +2.7 | **−15.7** |
| E  | 2,000,000 | 5.712 | 5.686 | 6.129 | −0.5 | +7.8 | +7.3 |

**MEASURED** — drift-free in-process deltas, median over 24 blocks (32 for B)
(`control-isolation/steady-throughput-1M/inproc3.csv`):

| wl | flat | tuned | bits | flat→tuned % | tuned→bits % | flat→bits % |
|----|-----:|------:|-----:|-------------:|-------------:|------------:|
| A  | 4.699 | 4.215 | 3.895 | **−10.3** | −7.6 | −17.1 |
| B  | 5.346 | 6.710 | 6.715 | **+25.5** | +0.1 | +25.6 |
| C  | 7.116 | 7.129 | 8.864 | +0.2 | +24.3 | +24.6 |
| D  | 5.130 | 4.367 | 4.734 | **−14.9** | +8.4 | −7.7 |
| E  | 8.178 | 7.949 | 9.600 | −2.8 | +20.8 | +17.4 |

**CONTROL RESULT** (what each delta isolates, per §1):

| | flat → tuned (control flow alone) | tuned → bits (bitmap alone) | flat → bits (end-to-end) |
|---|---|---|---|
| A (update-only) | **−10.6 / −10.3 ROBUST win** | +17.7 / −7.6 **methods disagree → not robust** | +5.2 / −17.1 disagree |
| B (10% deletes) | **+28.6 / +25.5 ROBUST loss** | +26.1 / +0.1 → no help | +62.2 / +25.6 robust loss |
| C (best churn) | +1.8 / +0.2 ~0 | +6.8 / +24.3 → no help (hurt in-proc) | +8.8 / +24.6 loss |
| D (top-of-book) | **−17.9 / −14.9 ROBUST win** | +2.7 / +8.4 → mild hurt | −15.7 / −7.7 win |
| E (uniform) | −0.5 / −2.8 ~0 | +7.8 / +20.8 → hurt | +7.3 / +17.4 loss |

**INTERPRETATION.** The control flow alone reproduces essentially all of the
old flat-vs-bits *win* on A and D, and the code shows why: flat never reads the
old slot on a positive write — it stores, then runs cached-best bookkeeping
(load `best_idx_`, compare, branch) on *every* write; tuned loads the slot once
to classify, and on a `positive → positive` write (the only kind workload A
contains; the majority in D) does a pure store with no best bookkeeping at all.
On B the same classification is a net *loss*: B is 90 % genuine `0 → positive`
adds (delete/restore at near-full density), where the bookkeeping tuned skips is
not redundant — so tuned pays the extra slot-load per add that flat never pays,
and both then do the same best bookkeeping. C (55 % LIFO refill of the
just-vacated, cache-hot best) and E land near zero because the extra load is
cheap or the add fraction lower. The sign of the control-flow effect is purely a
property of the requantify-vs-transition mix.

The bitmap over that control adds one occupancy word update per add/delete
transition (plus, on best-delete, its hierarchy) and removes the linear
re-scan. On the frozen workloads — where best-deletes are adjacent and thus the
flat re-scan is ~free — the bitmap's designed payoff is absent, so its marginal
effect is what the per-transition occupancy writes cost: robustly zero or
positive (slower) on B/C/D/E, contested on A.

**LIMITATION.** (1) Per-process `bits` rows carry ±40 % round-to-round noise and
are window-dependent across sessions; the two steady tables above are the same
session but *different* process/interleave contexts, and they disagree on the
`tuned→bits` *magnitude* on C/E and on its *sign* on A — which is precisely why
A is reported not-robust and only sign-agreement is claimed elsewhere. (2) The
frozen dataset's flat-vs-bits inproc rows (two-way interleave) are *not*
directly comparable to these three-way inproc rows for the bitmap (e.g. bits_C
6.832 frozen-two-way vs 8.864 three-way): the interleave context itself shifts
the large-footprint impl. flat rows are far more stable across contexts (A–D
cross-process within ~±3 %; flat_E is the loosest at ~+10 %), which is what lets
the flat-vs-tuned rows above carry across sessions; tuned is measured only this
session. (3) Workload B runs 500,000 updates for wall-time reasons only (B's
off-clock stream generation degenerates at near-full density); the timed compare
is identical streams at identical updates across the three impls.

**Harness cross-check:** the frozen Phase-2 / steady references are reproduced
for the frozen book: flat_A_1M = 4.296–4.435 ns here vs 4.327 (Phase 2);
flat_D 4.423–4.559 vs 4.27 (Phase 2 era). The steady bench measures the same
thing Phase 2 measured.

### 4.3 Q1/Q2/Q3 — the frozen study's flat-vs-bits numbers, re-attributed

**CONTROL RESULT.** The frozen steady dataset reported flat→bits A ≈ −28 %
(cross) / −8.2 % (in-proc) and D ≈ −18 % / −24 % and called the bitmap "faster"
on the requantify-heavy workloads. Re-attributed with the control, those rows
are: control flow **A −10 %, D −15/ −18 % (robust)**; bitmap marginal over the
control **no robust win on any workload, neutral-to-harmful elsewhere**. The
frozen A/D numbers were real end-to-end effects; the *bitmap* was not their
cause. (The frozen B row, flat→bits +36 %, decomposes to +25 % control-flow loss
and ~0–+26 % bitmap — i.e. even the bitmap's *penalty* on B was mostly control
flow.)

**INTERPRETATION.** `BitsetFlatOrderBook` was designed transition-aware from the
start, so it inherited the beneficial control-flow restructure as a side effect
of its occupancy model (it must know whether a level appears or disappears to
maintain the bitmap — the same classification tuned performs for free). That
side effect, not the bitmap, dominated its steady-state performance on A/D.

### 4.4 Q4 — at what gap does the bitmap beat the adjacent scan? (hardened)

Controlled ladder sweep (`benchmark/order_book_gap_bench.cpp`): each row loads
an ask side of K+1 levels spaced exactly `g` ticks apart (floor level survives),
so **every** one of the K timed best-deletes rescans exactly `g` slots in the
flat book and exactly one `g`-tick skip in the bitmap. Flat, tuned-independent
(no bitmap in this experiment), and bits replay byte-identical books and delete
streams, interleaved block-by-block in one process (128 fresh blocks/impl/gap;
K = 4096 deletes per block). To harden the crossover claim (the frozen study ran
one forward ladder), this revision ran **10 independent rounds** — 5 sweeping
gaps forward (1…1024) and 5 reverse — and reports the median-over-rounds p50
plus, per gap, in how many rounds the "bits faster" sign held
(`control-isolation/gap-crossover/`, `analysis-var-domain.txt`):

| gap `g` | flat p50 | bits p50 | Δ % | rounds bits faster (of 10) |
|--------:|---------:|---------:|----:|---------------------------:|
| 1  | 3.09 | 3.86 | +24.7 | 0 |
| 2  | 3.27 | 4.10 | +25.5 | 0 |
| 4  | 4.23 | 4.41 | +4.2 | 1 |
| **8**  | **5.08** | **4.37** | **−13.8** | **10** |
| 16 | 7.53 | 4.75 | −37.0 | 10 |
| 32 | 12.15 | 5.49 | −54.8 | 10 |
| 64 | 20.42 | 5.18 | −74.6 | 10 |
| 128 | 43.54 | 5.56 | −87.2 | 10 |
| 256 | 84.93 | 6.09 | −92.8 | 10 |
| 512 | 158.18 | 7.33 | −95.4 | 10 |
| 1024 | 298.68 | 10.16 | −96.6 | 10 |

**MEASURED.** Median-over-rounds crossover gap **g = 8**. Per-round crossover
distribution over 10 rounds: **min 4, median 8, max 8** — no round crossed
before g = 4 and every round had crossed by g = 8. Flat is linear in the gap:
a least-squares fit over g ∈ [64, 1024] gives **≈ 0.29 ns per rescanned slot**
above a per-delete fixed cost (g = 1024 ≈ 299 ns ≈ fixed + 0.29 × 1024). The
bitmap's best-delete is **far less sensitive to gap size than linear slot
scanning**: it rises only ≈ 0.006 ns per gap slot (4.4 ns @ g = 8 → 10.2 ns @
g = 1024 — ~50× flatter than flat). It is *not* claimed gap-invariant at a
fixed 4–6 ns; its mild rise at the top of the ladder is part of the measured
curve.

**CONTROL RESULT (fixed-domain).** The default ladder grows the book domain with
`g` (domain = 1+(K+1)·g), coupling re-scan distance to active memory span.
`--fixed-domain` re-runs every gap on the single largest domain so only `g`
changes (`fixed-domain-gap-validation/`, 6 rounds — 3 forward, 3 reverse).
Result: the crossover is **identical** — median-over-rounds crossover g = 8,
bits 6/6 rounds faster at g = 8, 0/6 at g = 4, per-round distribution min/
median/max = 8 — and the rows track the variable-domain rows closely (g = 1024
flat 303.2 vs 298.7; g = 4 flat 4.34 vs 4.23). **INTERPRETATION:** on this
machine, at this K, the flat book's per-gap cost is a property of the re-scan
distance `g`, not of the domain size; the crossover claim is not a
memory-span artifact.

**LIMITATION.** The `g = 1` row has both books above their own `g = 2..8` trend
in the frozen sweep; in these multi-round runs it is less anomalous (flat 3.1 vs
3.3 @ g = 2) but still shows bits losing more at g = 1 than g = 2 — every delete
stays inside a single 64-slot cache region. It does not affect the crossover,
which sits at g = 8 with flat still ahead at g = 4. All ladder rows delete only
the best price; ordinary-update cost is measured separately (§4.2), so the gap
result and the steady result cannot be conflated.

### 4.5 Q5 — do the real workloads reach the winning-gap regime?

Best-delete re-scan distances actually produced by the frozen workloads
(`orderbook_bitmap_bench --gaps`, off-clock stream analysis; frozen
`gaps-analysis/*.log`, re-verified 2026-09-09):

| wl | best-deletes in stream | re-scan distance |
|----|----------------------:|------------------|
| A  | 0 | — (update-only) |
| B  | 0 | — |
| C  | 900,944 @2M (450,467 @1M) | **100 % ≤ 1** |
| D  | 18 | 100 % ≤ 1 |
| E  | 0 | — |

**Only C deletes the best price in volume, and every one of those deletes finds
the next level in the adjacent slot** (distance 1). The flat book never rescans
more than one slot on the frozen streams; the bitmap's designed payoff — skipping
a *large* gap — is **never exercised** by A–E. Its measured A/D wins are the
control flow (§4.3), its measured B/C/E non-wins the transition mix (§4.2) — all
orthogonal to best-price discovery. This is the central, measurement-grounded
conclusion: **on the frozen production workloads the bitmap's headline feature
is unused**, and its steady-state merit on them is decided entirely by the
add/delete mix, where it does not beat the control.

### 4.6 Memory cost (reported, not assumed zero)

At N = 1M levels the book's domain is `[1, 2N]` = 2,000,000 slots per side.
`orderbook_bitmap_bench --memory`
(`docs/results/orderbook-bitmap-optimization/memory/run.log`, reproduced
2026-09-09):

| | bytes | notes |
|---|---:|---|
| quantity arrays (2 sides × 2,000,000 × 8 B) | 32,000,000 | 30.52 MiB (15.26 MiB/side) |
| occupancy L0 (2 × 31,250 words) | 500,000 | 244.14 KiB/side |
| occupancy L1 (2 × 489 words) | 7,824 | 3.82 KiB/side |
| occupancy L2 (2 × 8 words) | 128 | 64 B/side |
| **occupancy total** | **507,952** | **0.48 MiB = 1.59 % of quantity bytes** |

The hierarchy is `span/64` (L0) + `span/4096` (L1) + ~0 (L2) words per side, so
it scales with the **addressable domain**, independent of how many levels are
actually live and of the machine's cache-line size (bit packing is
fixed-width). `TransitionAwareFlat` adds no memory over `Flat`.

## 5. VERDICT — the five questions, answered from the measurements above

1. **How much of A's ~28 % speedup was control flow vs bitmap?**
   Essentially all of it was **control flow**. The frozen end-to-end A number was
   not reproduced as a stable point (per-process bits spans 3.9–6.2 ns), but the
   decomposition is stable where it matters: tuned alone (no bitmap) is a robust
   **−10 %** on A, and the bitmap's marginal contribution over tuned on A is
   ambiguous this session (−7.6 % in-process, +17.7 % cross-process → **not
   robust**). The honest statement: on A the measurable, reproducible win is the
   transition-aware control flow; the bitmap neither reliably adds to it nor is
   proven to subtract from it on A. (D decomposes cleanly: control **−15 to
   −18 %** robust win; bitmap marginal **+3 to +8 %** mild penalty.)

2. **What is the actual bitmap maintenance tax vs TransitionAwareFlat on B/E?**
   On **B the bitmap is ~costless over the control**: tuned→bits +0.1 %
   in-process (+26 % cross-process, but the in-process drift-free read is
   authoritative and the whole B penalty is the control flow: +25.5 %). On **E
   the bitmap is a measured penalty over the control**: +20.8 % in-process /
   +7.8 % cross — both methods positive, magnitude uncertain. So the "bitmap tax"
   is workload-shaped: ~0 on B's delete/restore, real on E's half-delete
   uniform stream, and the *old* flat-vs-bits penalties on B/E were mostly the
   control-flow change, not the bitmap.

3. **Does the bitmap materially help C after controlling the non-bitmap path?**
   **No.** C is where the bitmap *should* pay — 900k adjacent best-deletes — and
   over the control it does not help: +6.8 % cross-process / +24.3 % in-process
   (both methods non-negative; the drift-free read says a real penalty). The
   best-deletes are all distance 1, where flat's linear re-scan is ~free and the
   bitmap still pays its per-transition occupancy writes; the hierarchical
   descent never earns its keep.

4. **Does repeated gap testing still place the M3 crossover around 4–8 ticks?**
   **Yes — now firmly at g = 8.** 10 independent variable-domain rounds
   (forward + reverse) cross at median g = 8 with a per-round distribution of
   4…8 (10/10 rounds bits-faster at g = 8; 1/10 at g = 4). A 6-round
   fixed-domain control (domain pinned, decoupling `g` from memory span)
   reproduces the crossover at g = 8 exactly. Claim: **the crossover is in the
   4–8 tick range and reproducibly lands at g = 8 on this host.**

5. **Is BitsetFlatOrderBook justified as a FlatOrderBook replacement given the
   real A–E gap distribution (almost all best-deletes at distance 1)?**
   **No, as a default.** On the frozen workloads it is a robust loss or wash on
   B/C/E, no better than the control on D, and no better than the control on A —
   while costing +1.59 % book bytes and being the least measurement-stable
   implementation on this host. **But the control-flow restructure it happens to
   contain is justified independently**: tuned reproduces the A/D wins without
   the bitmap. The defensible recommendation: keep `FlatOrderBook` as the
   frozen default; adopt a transition-aware `FlatOrderBook`-equivalent where the
   production stream is requantify-heavy and not delete-heavy; select the bitmap
   book only for sparse best-delete-gap regimes beyond ~8 ticks (where §4.4
   measures a real, flat-best-delete-eliminating advantage) with low enough
   transition churn that §4.2's per-transition occupancy writes do not dominate.
   Per the study's own framing this is the expected outcome — the goal was never
   to prove the bitmap faster, and the control now shows exactly which of its
   two changes was doing the measured work.

## 6. HOW — methodology and reproducibility

**Everything is measured, nothing is invented** (the repo honesty rule; a number
only enters this document if it appears in a committed run under
`docs/results/orderbook-bitmap-optimization/` with its command and host next to
it).

- **Steady throughput** (`benchmark/order_book_bitmap_bench.cpp`, Phase-2
  methodology): identical deterministic A–E streams (same seed, same generator —
  `benchmark/stream_gen.h`) replayed through all three books; all updates
  pre-generated before any clock; fresh snapshot cold-start per block; compiler
  barrier per op; best-of-reps inside a process, median-of-rounds across
  processes; `--inproc`/`--inproc=N` drift-free interleave. Workload B runs
  500,000 updates only to bound wall time (B's *stream generation* is
  pathologically slow at near-full density — an off-clock,
  implementation-neutral artifact); the timed compare is identical streams at
  identical updates.
- **Gap sweep / crossover** (`benchmark/order_book_gap_bench.cpp`):
  deterministic ladder (K+1 levels, `g` apart, floor survives), K=4096
  best-deletes per fresh block, 128 interleaved blocks per impl/gap;
  `--fixed-domain` pins the domain for the memory-span control.
- **Gap analysis** (`orderbook_bitmap_bench --gaps`): off-clock scan of each
  generated stream's best-deletes and their re-scan distances.
- **Memory** (`orderbook_bitmap_bench --memory`): sums the actual allocated
  `vector` bytes reported by the book's accounting helpers.
- **Robustness rule:** a finding is called robust only when the per-process and
  in-process methods agree on its sign; per-round dispersion is committed with
  the data, and no cross-session absolute for the bitmap is quoted.

### Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON && cmake --build build
ctest --test-dir build --output-on-failure

# canonical control-isolation steady run: 3 impls, one per process, 5 rotating
# rounds per workload (A/C/D/E @ 2M ops, B @ 500k); + drift-free 3-way inproc
bash docs/results/orderbook-bitmap-optimization/control-isolation/steady-throughput-1M/run.sh
bash docs/results/orderbook-bitmap-optimization/control-isolation/steady-throughput-1M/run_inproc3.sh

# gap-crossover hardening: 5 fwd + 5 rev rounds (variable domain), then
# 3 fwd + 3 rev rounds with --fixed-domain (the memory-span control)
bash docs/results/orderbook-bitmap-optimization/control-isolation/gap-crossover/run.sh   # outputs both
python3 docs/results/orderbook-bitmap-optimization/control-isolation/gap-crossover/analysis.py

# single ladder (any one round), and the fixed-domain variant
./build/orderbook_gap_bench --impl=both --blocks=128 --deletes=4096
./build/orderbook_gap_bench --impl=both --blocks=128 --deletes=4096 --fixed-domain

# four-way semantic agreement + best-delete distance + memory
./build/orderbook_bitmap_bench flat all all --check
./build/orderbook_bitmap_bench flat C 1000000 updates=2000000 --gaps
./build/orderbook_bitmap_bench both 1000000 --memory
```

Note: the committed `gap-crossover/run.sh` writes into the study's
`results/control-isolation/` transient area (git-ignored) and prints the
variable- and fixed-domain rounds to `var-domain/` and `fixed-domain/`
subdirectories; the fixed-domain rounds are also committed under
`fixed-domain-gap-validation/rounds/`. The per-dir analysis summaries
(`analysis-var-domain.txt`, `analysis-fixed-domain.txt`) were produced by
pointing `analysis.py` at each round directory (its `main()` expects
`var-domain/` + `fixed-domain/` siblings).

### Data tree

```
docs/results/orderbook-bitmap-optimization/
├── host.txt                              # machine/OS/compiler/git-state/date
├── steady-throughput-1M/                 # FROZEN two-impl dataset (unchanged)
│   ├── run.sh, command.txt               # how the canonical flat/bits rows were produced
│   ├── raw/{flat,bits}_{A..E}.log        # 5 per-process rounds per cell
│   ├── medians.csv, inproc.csv           # cross-process + two-way in-process
│   └── flat.csv / bits.csv               # one-round per-impl samples
├── control-isolation/                    # THIS REVISION's evidence
│   ├── steady-throughput-1M/
│   │   ├── run.sh, run_inproc3.sh, command.txt
│   │   ├── raw/{flat,tuned,bits}_{A..E}.log   # 5 per-process rounds per (impl,wl)
│   │   ├── medians.csv                   # 3-impl cross-process medians (Table §4.2)
│   │   └── inproc3.csv                   # 3-impl in-process drift-free deltas (§4.2)
│   └── gap-crossover/
│       ├── run.sh, command.txt, analysis.py
│       ├── var-domain/{fwd,rev}-{1..5}.log   # 10 rounds (Table §4.4)
│       └── analysis-var-domain.txt
├── fixed-domain-gap-validation/          # memory-span control (§4.4)
│   ├── command.txt
│   ├── rounds/{fwd,rev}-{1..3}.log       # 6 rounds, --fixed-domain
│   └── analysis-fixed-domain.txt
├── gap-sweep/run.log, command.txt        # FROZEN single forward ladder (superseded by §4.4; kept)
├── gaps-analysis/{A..E}.log              # Table §4.5 (frozen; re-verified)
└── memory/run.log                        # §4.6
```

## 7. Files changed

New (this study):

- `include/bitset_flat_order_book.h` — the candidate book (semantics-identical,
  transition-only hierarchical occupancy; no inheritance). **Edited this
  revision:** overclaim removed — the header now documents that the top-level
  L2 fallback in `prev_occ2()`/`next_occ2()` linearly walks a bounded number of
  `occ2` summary words (8 at a 2M-slot-per-side domain) and states explicitly
  that there is no L3; no functional change.
- `include/transition_aware_flat_order_book.h` — **the control** (this
  revision): the frozen book's layout and linear re-scan with only the
  transition-aware positive path; no bitmap; a benchmark/test fixture only.
- `tests/bitset_order_book_tests.cpp` — four-way differential + boundary tests
  (116 checks), incl. a requantify-control suite that hammers the restructured
  path.
- `benchmark/order_book_bitmap_bench.cpp` — impl grammar `flat|tuned|bits|
  both|all`; `--check` verifies flat/tuned/bits; `--inproc` generalized to N-way.
- `benchmark/order_book_gap_bench.cpp` — `--fixed-domain` mode.
- `docs/ORDERBOOK_BITMAP_OPTIMIZATION.md` — this document.
- `docs/results/orderbook-bitmap-optimization/control-isolation/`,
  `.../fixed-domain-gap-validation/` — this revision's raw data, commands, host
  provenance (nothing under the pre-existing `docs/results/` was touched).

Modified (register/plumb the new targets; doc pointers; no behavioral change to
any frozen implementation or result):

- `CMakeLists.txt` — register the bench targets under `BUILD_BENCHMARKS`
  (forced `-O3 -DNDEBUG`) + `BENCH_ARCH_FLAGS` propagation.
- `README.md`, `docs/results/README.md` — layout entries and the study pointer.
- `benchmark/order_book_bench.cpp`, `cmake/assert_nonzero_exit.cmake`,
  `docs/profiling/README.md` — pre-existing working-tree exit-code-guard and
  wording changes; not part of this study and unchanged by it.
- `include/bitset_flat_order_book.h` — comment-only honesty edits (above).

**Untouched:** `include/flat_order_book.h`, `include/map_order_book.h`,
`include/types.h`, `benchmark/stream_gen.h`, `tests/order_book_tests.cpp`,
Phase 4 files, and every canonical result file.

## 8. Status

Complete. All four books are behaviorally identical (four-way differential,
sanitizer-clean, strict-warning-clean). The control shows the frozen A/D
flat-vs-bits wins were **control flow, not bitmap**; the bitmap's incremental
effect on the frozen workloads is no robust win (neutral-to-loss on B/C/D/E,
ambiguous on A). The gap crossover is hardened to **g = 8** across 16
independent rounds including a fixed-domain memory-span control; the frozen
workloads never reach it (C's best-deletes are all distance 1). Memory overhead
is small, reported, and unchanged. The verdict is a *regime-dependent
alternative* — and with the control, the study can now say precisely which of the
candidate's two changes earns its keep where. No Phase 5 work was started;
nothing was committed without the user's request.
