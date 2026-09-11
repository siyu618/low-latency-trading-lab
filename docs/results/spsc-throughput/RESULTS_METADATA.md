# Experiment 02, Phase 2 — throughput results (Apple M3 Max) — CANONICAL

The canonical **two-thread end-to-end message-transfer throughput** dataset for
the frozen Phase-1 SPSC ring buffer against the `MutexBoundedQueue` reference
baseline. Analysis and methodology: `docs/SPSC_THROUGHPUT.md`.

**Status: COMPLETE / FROZEN.** Measured 2026-09-11 (UTC 08:05:08) on the Apple
M3 Max development host with the Phase-2.1 hardened runner. Every number here
comes from a real run; nothing is interpolated, estimated, or copied from
elsewhere.

## What is measured

`ns_per_message` = **end-to-end elapsed time / messages delivered** for the
complete producer-to-consumer transfer of N messages by two threads, including
queue synchronization, payload assignment, cache-coherence effects, the
harness's retry/backpressure behaviour and OS scheduling effects. It is **not**
a per-call `try_push`/`try_pop` latency and **not** a one-way handoff latency.

For `MutexBoundedQueue` it also includes `std::mutex` acquisition, which can
block under contention. `SpscRingBuffer::try_push`/`try_pop` do not lock, spin,
sleep or block.

The measured code is the frozen Phase-1 implementation: unpadded, no cached
remote cursors, no batching, no memory-order changes, no CPU affinity. Phase 3
owns all of those.

## Configuration

| item | value |
|---|---|
| matrix | 2 impls (`mutex`, `spsc`) × 3 message sizes (8, 32, 64 bytes) × 3 capacities (1024, 4096, 65536) = 18 cells |
| message count | 10,000,000 per repetition |
| measured repetitions | 5 per process |
| warm-up | 1 repetition per process, **excluded from all reported and derived data** (internally timed and validated, never published) |
| sessions | 4, run in a **balanced AB/BA** order (§ below) |
| processes | 72 = 9 cells × 2 impls × 4 sessions |
| measured repetitions total | 360 (40 per cell, 20 per implementation) |
| implementations per process | exactly 1 — the two legs are never timed in one interval or one address space |
| correctness gate | exactly N consumed, sequence strictly increasing, no gaps, no duplicates, independently precomputed checksum — all 360 measured repetitions `PASS` |
| total measured volume | **3.6 billion** message transfers |
| wall time | 8 m 03 s |

## The balanced AB/BA run order

The previous canonical pass ran all nine mutex cells before all nine SPSC cells
in every session, confounding implementation with position in time. This run
pairs the two implementations of the same `message_bytes + capacity` as
**adjacent processes** and balances order across four sessions:

| session | traversal | implementation order |
|---|---|---|
| 1 | forward | mutex → spsc |
| 2 | reverse | spsc → mutex |
| 3 | forward | spsc → mutex |
| 4 | reverse | mutex → spsc |

Every cell therefore gets 2 mutex-first and 2 SPSC-first comparisons. The
runner **verifies** this after the fact by parsing the recorded execution order
out of `command.txt`; the canonical run reports
`balanced_implementation_order=PASS`.

## Retry / yield policy (corrected in Phase 2.1)

A failing `try_*` is retried immediately; `std::this_thread::yield()` is called
only after **1024 consecutive** failed attempts, and the consecutive-miss
counter is **reset by every successful** `try_push`/`try_pop`. An earlier
harness revision never reset that counter, so a yield did not actually follow
1024 *consecutive* misses as documented. The queue implementations were not
changed; because the policy changes scheduler interaction, **every canonical
number was regenerated** under the corrected policy rather than patched.

## Dataset invariants (verified — see `invariants.txt`)

The runner refuses to derive any summary unless all of the following hold:

| invariant | required | canonical run |
|---|---|---|
| processes recorded in `command.txt` | 72 | 72 |
| raw CSV files | 72 | 72 |
| measured rows per raw file | exactly 5 | 5 |
| `correctness` on every measured row | `PASS` | `PASS` |
| distinct sessions per cell | exactly 4 | 4 |
| times each implementation ran **first** in a cell | exactly 2 each | 2 each |
| distinct cells | 9 | 9 |
| checksum stable within a cell | yes | yes |
| per-process summary median/min/max vs its own raw CSV | match | match |
| **overall** | — | **`all_invariants=PASS`** |

## Data files

- `raw/<impl>_b<bytes>_c<capacity>_s<session>.csv` — 72 files, one row per
  measured repetition (5 rows each) plus a column-header row. Columns: `rep`,
  `impl`, `message_bytes`, `capacity`, `message_count`, `elapsed_ns`,
  `ns_per_message`, `messages_per_second`, `producer_full_retries`,
  `consumer_empty_retries`, `checksum`, `correctness`. **This is the primary
  data; every other file here is derived from it.**
- `summaries/<...>.txt` — per-process summary exactly as reported by the
  benchmark (median / min / max / spread over that process's 5 repetitions).
- `stderr/<...>.txt` — the benchmark's per-repetition progress log, including
  the excluded warm-up repetition.
- `paired_summary.csv`, `PAIRED_COMPARISON.md` — **the primary implementation
  comparison**: per-session paired ratio (SPSC median ÷ mutex median) for each
  cell, with which implementation ran first, plus per-cell median/min/max of the
  four paired ratios and the sign-consistency tally. Descriptive, not a
  significance test.
- `summary.csv`, `MATRIX.md` — pooled matrix, one row per cell. **Secondary and
  descriptive**: the 5 repetitions inside one process are correlated, so pooled
  `min`/`max` are extremes over correlated samples, not independent ones.
- `SESSIONS.md` — per-process/session medians per cell, so between-process
  movement is visible separately from between-repetition movement. These are
  per-process medians, **not** fixed-placement medians.
- `command.txt` — the exact invocations, one line per process, **in the order
  they ran**, with a session marker before each block (recorded by the runner as
  it ran them, not reconstructed afterwards).
- `run_order.txt` — the execution order parsed back out of `command.txt`; the
  input to the balance check.
- `invariants.txt` — the invariant check results above.
- `HOST.md` — host and toolchain metadata.

`summary.csv`, `MATRIX.md`, `SESSIONS.md` and the paired files are produced only
**after** all 72 processes finish, and only after the runner has re-verified
every summary's median/min/max against the raw CSV it claims to summarize.

## Headline result

Direction is reported per cell from the **paired-session** analysis (median
ratio < 1 ⇒ SPSC completed a message faster):

| bytes | cap | median paired ratio | sessions SPSC faster | sessions mutex faster | direction |
|---|---|---|---|---|---|
| 8 | 1024 | 0.722 | 3 | 1 | SPLIT 3–1 |
| 8 | 4096 | 0.887 | 3 | 1 | SPLIT 3–1 |
| 8 | 65536 | 1.970 | 0 | 4 | stable — mutex 4/4 |
| 32 | 1024 | 0.958 | 2 | 2 | SPLIT 2–2 |
| 32 | 4096 | 1.375 | 0 | 4 | stable — mutex 4/4 |
| 32 | 65536 | 2.226 | 0 | 4 | stable — mutex 4/4 |
| 64 | 1024 | 0.877 | 4 | 0 | stable — SPSC 4/4 |
| 64 | 4096 | 0.543 | 4 | 0 | stable — SPSC 4/4 |
| 64 | 65536 | 0.649 | 4 | 0 | stable — SPSC 4/4 |

**6 of 9 cells hold one direction across all four balanced sessions; 3 are
inconclusive.** Sign consistency across four paired observations is a
descriptive summary, not a significance test.

## How it was measured

```sh
scripts/spsc-throughput.sh            # canonical matrix: 18 cells x 4 sessions
```

The runner builds a fresh Release tree, forces `-O3 -DNDEBUG` on the benchmark
target (`BUILD_BENCHMARKS=ON`), runs one implementation per process in the
balanced order above, and preserves raw output before summarizing. The benchmark
itself (`benchmark/spsc_throughput_bench.cpp`) creates both threads, the queue
and the expected-checksum pre-pass **outside** the timed interval, waits for
both threads to signal READY, then releases them and has the consumer record its
own completion timestamp after receiving the final message.
`producer_full_retries` and `consumer_empty_retries` are reported as observable
metrics, not failures.

## Environment (2026-09-11)

| item | value |
|---|---|
| machine | Mac15,10 — Apple M3 Max |
| cores | 14 logical — 10 P-cores / 4 E-cores |
| memory | 36 GiB |
| OS | macOS 14.2.1 (Build 23C71), Darwin 23.2.0, arm64 |
| compiler | Apple clang 15.0.0 (clang-1500.3.9.4), C++20 |
| build flags | `-O3 -DNDEBUG` (forced on the benchmark target); `BENCH_ARCH_FLAGS` empty |
| commit | `b7e1c5862b14d4818c7adbd0a59cfa7ead5150aa` + the uncommitted Phase-2.1 runner/harness changes (tree recorded DIRTY) |

**Scheduling limitation.** No hard CPU pinning or affinity is implemented or
claimed — macOS offers no supported hard-pin API, this phase contains no
affinity hacks, and the machine's P/E mix, thread migration, DVFS, and
background system load all influence these concurrent measurements. This is why
the balanced multi-session design exists, and why direction is taken from paired
medians rather than from a single launch.

## Notes

- **Each repetition launches a fresh producer/consumer thread pair.** A session
  is a grouping of repetitions inside one process/address-space lifetime; it is
  **not** a fixed thread placement, and this dataset makes no such claim.
  Differences between repetitions and between sessions may reflect scheduler
  placement, migration, P/E-core selection, DVFS, thermal state, allocator and
  address placement, or background system activity. Phase 2 does not identify
  which factor caused any particular slow or fast run.
- **One measured repetition was externally interrupted.** In
  `raw/spsc_b8_c1024_s1.csv`, repetition index 2 recorded an elapsed time of
  347 s (34706 ns/msg) against ~0.4 s for its four siblings, with *lower* retry
  counts than three of them and `correctness=PASS`. It is preserved unedited and
  is visible in the pooled `[min,max]` of that cell. Every implementation
  comparison in this dataset is median-based and is unaffected by it.
- **No result here is attributed to false sharing.** Phase 2 has no
  packed/separated control and no cache-line/address verification; that
  comparison is Phase 3.
- **The mutex baseline is deliberately unoptimized** (`std::mutex` around fixed
  storage). These are numbers for *that* baseline, not for lock-based queues in
  general.
- **The message-size axis carries a second variable**: the consumer's validation
  fold is 1, 4 and 8 words for `Msg8`, `Msg32` and `Msg64`. Within-cell
  mutex-vs-SPSC comparisons are unaffected; absolute throughput is **not**
  compared across message sizes in this dataset.
- Reproduce at a smaller size with
  `MESSAGES=200000 REPS=3 scripts/spsc-throughput.sh`. A re-run reproduces the
  *distribution*, not the exact values; treat the spread as part of the result.

## Superseded datasets

Preserved, labelled, and **not to be cited for comparative claims**:

| directory | why superseded |
|---|---|
| `../spsc-throughput-superseded-fixed-order/` | implementation confounded with run order (all mutex before all SPSC in every session); earlier retry/yield policy |
| `../spsc-throughput-superseded-single-session/` | single session per cell; no cross-session sampling |
