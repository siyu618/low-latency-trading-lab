# Experiment 02, Phase 2 — canonical throughput results (Apple M3 Max)

The canonical **two-thread end-to-end message-transfer throughput** dataset for
the frozen Phase-1 SPSC ring buffer against the `MutexBoundedQueue` reference
baseline. Analysis and methodology: `docs/SPSC_THROUGHPUT.md`.

**Status: COMPLETE / FROZEN.** Measured 2026-09-11 (UTC 07:09:52) on the Apple
M3 Max development host. Every number here comes from a real run; nothing is
interpolated, estimated, or copied from elsewhere.

## What is measured

`ns_per_message` = **end-to-end elapsed time / messages delivered** for the
complete producer-to-consumer transfer of N messages by two threads, including
queue synchronization, payload assignment, cache-coherence effects, the
harness's retry/backpressure behaviour and OS scheduling effects. It is **not**
a per-call `try_push`/`try_pop` latency and **not** a one-way handoff latency.

The measured code is the frozen Phase-1 implementation: unpadded, no cached
remote cursors, no batching, no memory-order changes, no CPU affinity. Phase 3
owns all of those.

## Configuration

| item | value |
|---|---|
| matrix | 2 impls (`mutex`, `spsc`) × 3 message sizes (8, 32, 64 bytes) × 3 capacities (1024, 4096, 65536) = 18 cells |
| message count | 10,000,000 per repetition |
| repetitions | 5 per session, 3 sessions (independent processes) per cell — 15 pooled per cell, all kept |
| warm-up | 1 untimed repetition per process, excluded |
| implementations per process | exactly 1 (the two legs are never timed in one interval or one address space) |
| correctness gate | exactly N consumed, sequence strictly increasing, no gaps, no duplicates, independently precomputed checksum — all 54 processes `PASS` |
| total measured volume | 2.7 billion message transfers |

## Data files

- `raw/<impl>_b<bytes>_c<capacity>_s<session>.csv` — 54 files, one row per
  measured repetition (5 rows each) plus a column-header row. Columns: `rep`,
  `impl`, `message_bytes`, `capacity`, `message_count`, `elapsed_ns`,
  `ns_per_message`, `messages_per_second`, `producer_full_retries`,
  `consumer_empty_retries`, `checksum`, `correctness`. **This is the primary
  data; every other file here is derived from it.**
- `summaries/<...>.txt` — per-process summary exactly as reported by the
  benchmark (median / min / max / spread over that process's 5 repetitions).
- `stderr/<...>.txt` — the benchmark's per-repetition progress log.
- `summary.csv` — derived matrix, one row per cell, pooled over all 3 sessions.
- `MATRIX.md` — the same matrix, human-readable.
- `SESSIONS.md` — per-session medians per cell, so process-placement spread is
  visible separately from repetition spread.
- `command.txt` — the exact invocations, one line per process (recorded by the
  runner as it ran them, not reconstructed afterwards).
- `HOST.md` — host and toolchain metadata.

`summary.csv`, `MATRIX.md` and `SESSIONS.md` are produced only **after** all 54
processes finish, and the runner re-verifies every summary's median/min/max
against the raw CSV it claims to summarize before deriving them.

## How it was measured

```sh
scripts/spsc-throughput.sh            # canonical matrix: 18 cells x 3 sessions
```

The runner builds a fresh Release tree, forces `-O3 -DNDEBUG` on the benchmark
target (`BUILD_BENCHMARKS=ON`), runs one implementation per process, and
preserves raw output before summarizing. The benchmark itself
(`benchmark/spsc_throughput_bench.cpp`) creates both threads, the queue and the
expected-checksum pre-pass **outside** the timed interval, waits for both
threads to signal READY, then releases them and has the consumer record its own
completion timestamp after receiving the final message. `producer_full_retries`
and `consumer_empty_retries` are reported as observable metrics, not failures.

Wall time for the canonical run: 1 m 49 s.

## Environment (2026-09-11)

| item | value |
|---|---|
| machine | Mac15,10 — Apple M3 Max |
| cores | 14 logical — 10 P-cores / 4 E-cores |
| memory | 36 GiB |
| OS | macOS 14.2.1 (Build 23C71), Darwin 23.2.0, arm64 |
| compiler | Apple clang 15.0.0 (clang-1500.3.9.4), C++20 |
| build flags | `-O3 -DNDEBUG` (forced on the benchmark target); `BENCH_ARCH_FLAGS` empty |
| commit | see `HOST.md` (repo HEAD at recording time) |

**Scheduling limitation.** No hard CPU pinning or affinity is implemented or
claimed — macOS offers no supported hard-pin API, this phase contains no
affinity hacks, and the machine's P/E mix, thread migration, DVFS, and
background system load all influence these concurrent measurements. This is why
15 repetitions are pooled per cell and why a directional claim is only made
when the two implementations' 15-repetition `[min, max]` intervals do not
overlap.

## Notes

- **Session-to-session variation dominates some cells.** A single process
  samples one thread placement and one queue placement, fixed for the life of
  the process; the same binary has been observed to differ by ~2× between
  launches. Pooling 3 sessions exists to describe that distribution instead of
  hiding it behind one lucky launch. The evidence is preserved in
  `docs/results/spsc-throughput-superseded-single-session/SUPERSEDED.md`.
- **No result here is attributed to false sharing.** Phase 2 has no
  packed/separated control and no cache-line/address verification; that
  comparison is Phase 3.
- **The mutex baseline is deliberately unoptimized** (`std::mutex` around fixed
  storage). These are numbers for *that* baseline, not for lock-based queues in
  general.
- **`spsc 8 / 4096` and several other SPSC cells are bimodal** (spread up to
  281.6%). Their medians are reported for completeness but support no
  directional conclusion.
- Reproduce at a smaller size with
  `MESSAGES=200000 REPS=3 scripts/spsc-throughput.sh`. A re-run reproduces the
  *distribution*, not the exact values; treat the spread column as part of the
  result.
