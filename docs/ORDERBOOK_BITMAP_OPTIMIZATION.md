# Experiment 01 Optimization Study — Hierarchical-Occupancy Bitmap

**WHAT this is.** An internal, post-Phase-4 study of one optimization idea for
the Experiment 01 flat order book: track each side's *occupancy* (which prices
hold a level) in a small hierarchical bitmap, so that after deleting the best
price the next best is found by a bounded word-level descent instead of
`FlatOrderBook`'s adjacent **linear re-scan** over empty slots. The candidate is
`BitsetFlatOrderBook` (`include/bitset_flat_order_book.h`), a semantics-identical
drop-in alternative. **`FlatOrderBook` is unchanged.**

**Research question** (this document answers it from real measurements):

> At what next-best price *gap* does the hierarchical-occupancy lookup become
> cheaper than the flat book's adjacent linear re-scan — and what does the
> bitmap cost ordinary updates? Is that gap a regime the real workloads reach?

The goal is **not** to prove the bitmap is universally faster; the answer below
is explicitly measurement-dependent and mostly *negative* for the frozen
workloads.

**Scope and frozen constraints.** Phase 1 (correctness), Phase 2 (throughput),
Phase 4 (tail latency), the A/B/C/D/E workload generators, `FlatOrderBook` and
`MapOrderBook`, and every canonical result file under `docs/results/` are
untouched and remain canonical. Nothing in this study is a Phase 5.

**Host / toolchain / date of every number in this document:** Apple M3 Max,
macOS 14.2.1 (23C71), Apple clang 15.0.0, Release `-O3 -DNDEBUG`, no arch flag.
Full provenance in `docs/results/orderbook-bitmap-optimization/host.txt`.
Measurements captured 2026-09-09.

---

## TL;DR (findings)

1. **Measured crossover gap ∈ (4, 8] price ticks.** On a controlled ladder whose
   every best-delete rescans exactly `g` slots, the flat book costs
   ≈ **0.26 ns per rescanned slot** above a per-delete fixed cost, while the
   bitmap book's best-delete is **gap-independent** at ~4–6 ns. The sweep
   crosses between `g = 4` (flat slightly faster) and `g = 8` (bitmap faster).

2. **The frozen workloads never reach that regime.** Only workload C deletes the
   best price at all (450,467 best-deletes in a 1M-op run), and **100 % of them
   have re-scan distance ≤ 1**. A/B/E delete the best zero times; D does 18, all
   distance ≤ 1. On the frozen A–E streams the bitmap's *designed* advantage —
   skipping a large gap — is never exercised.

3. **On ordinary (non-best) updates the bitmap is not a fixed tax — its sign
   depends on the stream shape.** Measured at 1M levels on the frozen workloads:
   the bitmap is **faster** where updates mostly re-quantify present levels
   (A ≈ −28 %; D ≈ −18 %) and **slower** where updates delete levels (B ≈ +36 %).
   C and E are **within measurement noise** — the two measurement methods do not
   even agree on their sign.

4. **Why:** re-quantifying a present level (`positive → positive`) touches the
   bitmap **never** — and, per the `-O3` disassembly, `FlatOrderBook` still pays
   its cached-best bookkeeping (reload + compare + branch) on every such write,
   which the bitmap elides. Deleting a present level (`positive → 0`) costs the
   bitmap one extra occupancy read-modify-write (plus rare upward propagation)
   that a non-best delete does not justify. So requantify-heavy streams favor
   the bitmap; delete-heavy streams penalize it.

5. **Memory cost is real but small:** the occupancy hierarchy is
   **507,952 B (0.48 MiB) per 1M-level book** vs 32,000,000 B of quantity
   arrays = **1.59 %**. It scales with the *addressable domain*, not book
   density. Reported by a memory helper, not assumed to be zero.

6. **Recommendation:** keep `FlatOrderBook` as the default (frozen workloads,
   adjacent-churn, zero extra memory). Select `BitsetFlatOrderBook` only when the
   stream is requantify-heavy (A/D-like) **or** the book can develop best-delete
   gaps beyond ~8 ticks (sparse books) — the measured flat penalty there is
   ≈ 0.26 ns per gap slot, and the bitmap removes it entirely.

---

## 1. WHY — the research question

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
Whether that helps on this machine, at these workloads, is a **measurement
question** — which is what the two new benchmarks and this document answer.

## 2. WHAT — the two implementations

Both books implement the identical `llob::` contract: integer-tick prices, a
single addressable band `[tick_min, tick_max]`, absolute quantities stored in a
contiguous `int64[]` (slot = `price − tick_min`), `qty == 0` deletes a level,
`best_*()` are O(1) cached reads, snapshot-driven resync, and the same
sequence / gap / invalid-update / out-of-range semantics. `BitsetFlatOrderBook`
is a fresh class — **no inheritance, no virtuals, no polymorphic types** anywhere.

| | `FlatOrderBook` (frozen) | `BitsetFlatOrderBook` (candidate) |
|---|---|---|
| quantity storage | preallocated `int64[span]` per side | identical |
| occupancy index | none | 3-level bitmap (below) |
| find next best after best-delete | adjacent **linear scan** of empty qty slots, O(g) | **hierarchical descent** over set bits, O(few words), gap-independent |
| best reads | cached, O(1) | cached, O(1) |
| apply() allocation | none after construction | none after construction |

### The occupancy hierarchy (per side, over the same slot span)

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
- No linear scan over empty slots ever runs. Word/hierarchy boundaries, the
  domain ends, and side-empties are handled explicitly.

The whole per-side hierarchy at a 2N-slot domain is ≈ (span/64 + span/4096)
words — far smaller than the quantity array (see §3.6). The bitmap answers one
question only: *"highest occupied slot below / lowest above X?"* Quantities are
never encoded in it.

## 3. Measured results

Full raw output, commands, and provenance live under
`docs/results/orderbook-bitmap-optimization/` (§4).

### 3.1 Correctness and hygiene (all green)

- **Three-way differential.** `tests/bitset_order_book_tests.cpp` (79 checks)
  runs every scenario against `MapOrderBook` (oracle), `FlatOrderBook` and
  `BitsetFlatOrderBook` and requires identical observable state (op result, sync
  state, sequence, best bid/ask price *and* qty) after every step, including
  snapshot load/reject, sequence-gap desync, large-domain best deletion, and a
  random differential fuzzer with periodic resyncs.
- **Clean build, full ctest, clean sanitizers, clean strict warnings** (item 14,
  rerun 2026-09-09 on the rebuilt binaries): `ctest` 5/5 in a fresh Release
  build (both book suites + 2 exit-code self-test guards); ASan+UBSan build
  passes all 5 tests and smoke-runs of all three new executables; the new
  translation units compile **clean under
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`**
  (after two sign-conversion casts in the benchmark code — no functional
  change). `orderbook_bitmap_bench --check` replays A–E streams at all four
  scales through flat and bitmap and requires agreement:
  `--check PASSED: Flat and Bits agree on every stream`.

### 3.2 Steady throughput on the frozen workloads, 1M levels

Two independent methodologies, per the repo's measurement discipline (one-impl
per process is the Phase-2-canonical method; the in-process interleaved read is
added so the flat-vs-bitmap **delta** shares one clock state — separate
processes drift by turbo/frequency on this host by more than the effects under
study). A finding is called **robust** only when both methods agree on its sign.

`docs/results/orderbook-bitmap-optimization/steady-throughput-1M/{medians,inproc}.csv`

| wl | flat (cross, med) | bits (cross, med) | Δ% cross | flat / bits in-proc (median) | Δ% in-proc | verdict |
|----|-------------------:|------------------:|---------:|-----------------------------:|-----------:|---------|
| A  | 4.296 | 3.108 | **−27.7** | 4.837 / 4.439 | −8.2 | **bitmap faster** |
| B  | 4.561 | 6.178 | **+35.5** | 5.742 / 7.055 | +22.9 | **bitmap slower** |
| C  | 6.043 | 6.576 | +8.8 | 7.365 / 6.832 | −7.2 | within noise * |
| D  | 4.423 | 3.637 | **−17.8** | 5.107 / 3.868 | −24.3 | **bitmap faster** |
| E  | 5.196 | 6.257 | +20.4 | 9.866 / 9.457 | −4.1 | within noise * |

Units ns/`apply()`, lower is better. Cross = median over 5 alternating rounds of
per-process best-of-3 (run.sh); in-proc = 24–32 interleaved fresh blocks in one
process. \* = the two methods disagree on sign, so the difference is smaller than
the measurement drift; do not read a speedup/slowdown into C or E.

**Harness cross-check:** the frozen Phase-2 references are reproduced — flat_A_1M
= 4.327 ns (Phase 2) vs 4.296–4.435 here; flat_C_1M = 5.912 (Phase 2) vs 5.997–
6.120 here. The new steady bench is measuring the same thing Phase 2 measured.

### 3.3 Question 1 — at what gap does the bitmap beat the adjacent scan?

Controlled ladder sweep (`benchmark/order_book_gap_bench.cpp`): each row loads
an ask side of K+1 levels spaced exactly `g` ticks apart (floor level survives),
so **every** one of the K timed best-deletes rescans exactly `g` slots in the
flat book and exactly one `g`-tick skip in the bitmap. Flat and bitmap replay
byte-identical books and delete streams, interleaved block-by-block in one
process (128 fresh blocks/impl/gap; K = 4096 deletes per block).

`docs/results/orderbook-bitmap-optimization/gap-sweep/run.log` —
`ns_per_delete_best` = min of the 128 blocks (best-of), plus p50:

| gap `g` | flat best | bits best | flat p50 | bits p50 |
|--------:|----------:|----------:|---------:|---------:|
| 1  | 5.50 | 9.12 † | 6.79 | 11.24 |
| 2  | 2.90 | 3.65 | 4.21 | 6.20 |
| 4  | 3.92 | 4.14 | 4.27 | 4.47 |
| **8**  | **4.82** | **4.00** | **5.17** | **4.39** |
| 16 | 6.99 | 4.38 | 8.37 | 4.77 |
| 32 | 10.92 | 4.96 | 11.97 | 5.42 |
| 64 | 18.91 | 4.80 | 20.61 | 5.20 |
| 128 | 39.88 | 5.12 | 43.01 | 5.55 |
| 256 | 78.67 | 5.45 | 87.59 | 6.17 |
| 512 | 143.65 | 6.37 | 158.13 | 8.33 |
| 1024 | 272.86 | 9.62 | 301.87 | 10.31 |

† the `g = 1` row is anomalous for **both** books (each sits well above its own
`g = 2..8` trend — every delete stays inside a single 64-slot cache region, so
the stream hammers a handful of lines), and is excluded from trend claims. It
does not affect the crossover conclusion, which lies at `g ∈ (4, 8]`.

**Reading the table.**

- **Flat is linear in the gap.** Between ladder points the cost grows ≈
  0.25–0.33 ns per rescanned slot; a least-squares fit over `g ∈ [64,1024]`
  gives **≈ 0.26 ns/slot** above a per-delete fixed cost. `g = 1024` costs
  ≈ 273 ns ≈ fixed + 0.26 × 1024.
- **The bitmap is gap-independent.** Best-delete costs ~4–6 ns across
  `g ∈ [8, 512]` — a bounded few-word descent that does not grow with the gap
  (the mild rise at `g = 1024` is the larger fresh book, not the lookup). Its
  per-delete cost is set by the transition bookkeeping, not the distance.
- **Crossover ∈ (4, 8].** The smallest swept gap where the bitmap's
  best-delete is faster than flat's is `g = 8` (flat 4.82 → bitmap 4.00, −17 %);
  at `g = 4` flat is still ~6 % ahead. Because flat grows ~0.26 ns per extra
  slot while the bitmap is flat, the bitmap's advantage only widens from there.

### 3.4 Question 2 — what does the bitmap add to ordinary updates?

"Ordinary update" is not one thing, and the bitmap is not a fixed tax:

| update kind | flat does | bitmap adds / saves | measured on the frozen workloads |
|---|---|---|---|
| re-quantify a **present** level (`positive → positive`) | store qty **+ reload & branch on cached best** | **bitmap untouched**; also elides flat's best bookkeeping | the only op in **A**; the majority in **D** → bitmap faster |
| **delete** a present level (`positive → 0`) | store qty; only rescans **if it was the best** | + 1 occupancy read-modify-write (occasionally L1/L2 clear) | delete-heavy **B**, part of **E** → bitmap slower / noise |
| **add** at an empty level (`0 → positive`) | store qty + maybe promote best | + 1 occupancy set (occasionally L1/L2 set) | rare in A–E at near-full density |

So the answer to "what does the bitmap cost ordinary updates" is: **nothing on
re-quantifies** (and it actually removes work flat still does), and **one extra
word update on every occupancy transition**. Which dominates is purely a property
of the update stream — requantify-heavy streams (A, D) favor the bitmap;
add/delete-heavy streams (B) penalize it; the near-cancelling mixes (C, E) land
in the noise. There is no universal per-update overhead to quote; §3.5 explains
the mechanism behind the measured A/B/D numbers.

### 3.5 Question 3 — do the real workloads reach the winning-gap regime?

Best-delete re-scan distances actually produced by the frozen workloads at 1M
(`orderbook_bitmap_bench --gaps`, off-clock stream analysis;
`docs/results/orderbook-bitmap-optimization/gaps-analysis/*.log`):

| wl | best-deletes in stream | re-scan distance |
|----|----------------------:|------------------|
| A  | 0 | — (update-only) |
| B  | 0 | — |
| C  | 450,467 | **100 % ≤ 1** |
| D  | 18 | 100 % ≤ 1 |
| E  | 0 | — |

**Only C deletes the best price in volume, and every one of those deletes finds
the next level in the adjacent slot** (distance 1). The flat book never rescans
more than one slot on the frozen streams; the bitmap's designed payoff — skipping
a *large* gap — is **never exercised** by A–E. Its measured wins on A/D come from
the requantify path, and its measured loss on B from the delete path — both
orthogonal to best-price discovery. This is the central, measurement-grounded
conclusion of the study: **on the frozen production workloads the bitmap's
headline feature is unused**, and its overall merit on them is decided entirely
by the add/delete mix.

### 3.6 Question 4 — memory cost (reported, not assumed zero)

At N = 1M levels the book's domain is `[1, 2N]` = 2,000,000 slots per side.
`orderbook_bitmap_bench --memory`
(`docs/results/orderbook-bitmap-optimization/memory/run.log`):

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
fixed-width). For a 2N-slot domain the L0 level is 1/64 of the qty bytes ≈
1.56 %, the rest is ~0.03 %.

### 3.7 Question 5 — where it wins/loses and why (mechanism, from the disassembly)

`FlatOrderBook::apply` computes `is_best = (idx == best_idx_[s])` on **every**
update, and on a positive write at a non-best price calls
`update_best_if_needed`, which reloads `best_idx_[s]` and branches on the side —
even when the write merely re-quantifies an already-present, already-beaten level
(the only kind of write workload A contains). The `-O3` disassembly shows this
redundant best-bookkeeping on every A op. `BitsetFlatOrderBook::apply` on
`positive → positive` is a **pure quantity store**: no `best_idx_` load, no
compare, no side branch — the bit is already set, and the cached best cannot
move because the level set did not change. That is the measured ~8–28 % on the
requantify-only (A) and requantify-mostly (D) streams.

Conversely a `positive → 0` delete of a **non-best** level is a no-op for flat's
best bookkeeping (pure store) but costs the bitmap one occupancy
read-modify-write on the containing L0 word (plus, when that word empties — rare
in the near-full frozen streams — an L1/L2 clear). That is the measured ~+23–36 %
on workload B, whose 10 % of ops are exactly non-best deletes of present levels
restored toward full density.

So the bitmap trades *flat's per-write best bookkeeping* (paid on every update)
for *per-transition occupancy maintenance* (paid only when a level appears or
disappears). Where transitions are rare (A, D) the trade wins; where they are
frequent (B) it loses; where the two balance (C, E) it is a wash.

### 3.8 Question 6 — is it a replacement?

Measured, workload-shaped answer:

- **Do not replace `FlatOrderBook` as the default.** On the frozen A–E
  workloads at 1M the bitmap is only clearly ahead on the requantify-heavy A/D
  and clearly behind on the delete-heavy B; C/E are in the noise; and none of
  A–E produces the sparse-book regime the bitmap is actually for. The frozen
  book stays the frozen book — simpler, zero extra memory, no measured downside
  where it is used today.
- **Select `BitsetFlatOrderBook` when the production stream matches one of two
  shapes:** (a) requantify-heavy, few add/delete transitions (an A/D-like feed —
  persistent levels being repriced/refilled in place), where it removes flat's
  redundant best bookkeeping; or (b) a book that can become **sparse** — wide
  price bands, thin books, bursty level churn — so best-delete re-scan gaps can
  exceed ~8 ticks, where flat pays ≈ 0.26 ns per gap slot and the bitmap is
  gap-independent. Both are dropped in with no semantic change and +1.59 % book
  bytes; `apply()` still performs no allocation.
- The correct mental model is a **crossover**, not a speedup: the bitmap wins
  only when best-deletion gaps are large or transitions are few, and this host's
  measured crossover gap is **between 4 and 8 ticks**.

## 4. HOW — methodology and reproducibility

**Everything is measured, nothing is invented** (the repo honesty rule; a number
only enters this document if it appears in a committed run under
`docs/results/orderbook-bitmap-optimization/` with its command and host next to
it).

- **Steady throughput** (`benchmark/order_book_bitmap_bench.cpp`, reusing the
  Phase 2 methodology): identical deterministic A–E streams (same seed, same
  generator — `benchmark/stream_gen.h`) replayed through both books; all updates
  pre-generated before any clock; fresh snapshot cold-start per block; compiler
  barrier per op; best-of-reps inside a process, median-of-rounds across
  processes. One implementation per process for the canonical (Phase-2-parity)
  numbers; an interleaved `--inproc` mode for the drift-free delta. Workload B
  runs fewer updates only to bound wall time — B's *stream generation* is
  pathologically slow at near-full density (an off-clock, implementation-neutral
  artifact), which is why B's canonical rows use 500k updates; the timed compare
  is still identical streams at identical updates.
- **Gap sweep** (`benchmark/order_book_gap_bench.cpp`): deterministic ladder
  (K+1 levels, `g` apart, floor survives), K=4096 best-deletes per fresh block,
  128 interleaved blocks per impl/gap.
- **Gap analysis** (`orderbook_bitmap_bench --gaps`): off-clock scan of each
  generated stream's best-deletes and their re-scan distances.
- **Memory** (`orderbook_bitmap_bench --memory`): sums the actual allocated
  `vector` bytes reported by the book's accounting helpers.

### Reproduce

```sh
cmake -S . -B build-item14 -DCMAKE_BUILD_TYPE=Release && cmake --build build-item14
ctest --test-dir build-item14 --output-on-failure

# canonical steady run (Phase-2 conventions): one impl per process, 5 alternating
# rounds per workload; A/C/D/E at 2M ops, B at 500k.
bash docs/results/orderbook-bitmap-optimization/steady-throughput-1M/run.sh

# gap ladder sweep (crossover)
./build-item14/orderbook_gap_bench --impl=both --blocks=128 --deletes=4096

# best-delete distance analysis of one frozen workload
./build-item14/orderbook_bitmap_bench both C 1000000 updates=1000000 --gaps

# memory report
./build-item14/orderbook_bitmap_bench both 1000000 --memory
```

### Data tree

```
docs/results/orderbook-bitmap-optimization/
├── host.txt                              # machine/OS/compiler/git-state/date
├── steady-throughput-1M/
│   ├── run.sh, command.txt               # how the canonical rows were produced
│   ├── raw/{flat,bits}_{A..E}.log        # 5 per-process rounds per cell
│   ├── medians.csv                       # cross-process median-of-rounds (Table §3.2)
│   ├── inproc.csv                        # in-process interleaved read
│   └── flat.csv / bits.csv               # one-round per-impl samples
├── gap-sweep/run.log, command.txt        # Table §3.3
├── gaps-analysis/{A..E}.log              # Table §3.5
└── memory/run.log                        # §3.6
```

## 5. Files changed

New (this study):

- `include/bitset_flat_order_book.h` — the candidate book (semantics-identical,
  transition-only hierarchical occupancy; no inheritance).
- `tests/bitset_order_book_tests.cpp` — three-way differential + boundary tests
  (79 checks).
- `benchmark/order_book_bitmap_bench.cpp` — steady flat-vs-bitmap benchmark
  (Phase-2 methodology) + `--check`/`--gaps`/`--memory`/`--inproc`.
- `benchmark/order_book_gap_bench.cpp` — controlled next-best-gap ladder sweep.
- `docs/ORDERBOOK_BITMAP_OPTIMIZATION.md` — this document.
- `docs/results/orderbook-bitmap-optimization/` — all raw data, commands, host
  provenance (nothing under the pre-existing `docs/results/` was touched).

Modified (register/plumb the new targets; doc pointers; no behavioral change to
any frozen implementation or result):

- `CMakeLists.txt` — register `orderbook_bitmap_bench` / `orderbook_gap_bench`
  under `BUILD_BENCHMARKS` (forced `-O3 -DNDEBUG`) + `BENCH_ARCH_FLAGS`
  propagation.
- `README.md` — this study's Layout-tree entries and a short "Optimization
  Study (internal)" pointer, added on top of the pre-existing working-tree
  terminology/doc wording fixes.
- `docs/results/README.md` — index entry for the new data family.
- `benchmark/order_book_bench.cpp`, `cmake/assert_nonzero_exit.cmake`,
  `docs/profiling/README.md` — pre-existing working-tree exit-code-guard and
  wording changes; not part of this study and unchanged by it.

**Untouched:** `include/flat_order_book.h`, `include/map_order_book.h`,
`include/types.h`, `benchmark/stream_gen.h`, `tests/order_book_tests.cpp`,
Phase 4 files, and every canonical result file.

## 6. Status

Complete. The bitmap book is correct (three-way differential, sanitizer-clean,
strict-warning-clean), its memory overhead is small and reported, its measured
crossover gap on this host is (4, 8], and the frozen workloads do not reach it.
The verdict is a *regime-dependent alternative*, not a replacement — and per the
study's own framing that is the expected, honest outcome; the goal was never to
prove the bitmap faster. No Phase 5 work was started; nothing was committed
without the user's request.
