# Phase 3 — profiling the Experiment 01 benchmark with Linux `perf`

Phase 3 provides the tooling to answer the "why" questions that Phase 2's
throughput numbers raised but could not answer. Phase 2 measured *how fast*
`apply()` is for each design; Phase 3 profiles *where the time goes*.

Phase 3 has two halves:

- **3A — profiling tooling**: the gated measurement boundary in the benchmark
  (opt-in, no-op for normal runs), the Linux perf harness
  (`scripts/perf-profile.sh`), this guide, and the result-layout under
  `docs/results/`. This is **complete** — the tooling is authored, validated,
  and committed. A final correctness pass aligned the gate with the real perf
  ack protocol (`"ack\n"`, not a command echo), made the PMU window exactly
  match the chrono window, enforced `reps=1`, and removed cross-architecture
  counter/latency pairing.
- **3B — Linux measurements and analysis**: actually running the harness on a
  Linux host with perf and committing the measured counter data. This is
  **pending** — no profile has been captured.

## Status & honesty box

**No perf numbers are presented anywhere in this repository — none have been
measured.** The dev machine is an Apple M3 Max (macOS) with no `perf`, no full
Xcode, and no Linux VM. The harness and this guide are authored to run on a
Linux host and have NOT been executed end-to-end against a real PMU. Any
illustrative perf output you may see in other docs is a labeled placeholder,
never a measured claim. When Phase 3B runs for real on Linux, its measured
`perf stat`/`perf report` output belongs in `docs/results/phase3-linux-<machine>/`
with the provenance discipline below.

The benchmark CLI and the normal Phase 2 hot path are FROZEN. Phase 3.1/3A added
a strictly opt-in profiling gate (see below); a normal run — no
`LLOB_PERF_CONTROL` env var — is byte-for-byte unchanged.

## Why profile? — the open questions from Phase 2

Phase 2 measured steady-state `apply()` throughput (ns/update) and documented a
few observations it deliberately did not try to explain:

1. **Workload C is cheap for the map** (~71 ns at 1M levels) versus map A (~183
   ns) and map D (~128 ns). C repeatedly deletes the current best price and
   refills it. Is the map cheap because the near-touch access pattern improves
   locality, branch predictability, or tree-path locality? Or something else?
2. **The flat book stays ~4.3–6.1 ns at every scale** (1k → 1M levels). It does
   a direct array store plus cached-best bookkeeping. Where does its per-op time
   actually go — and what does workload C's inward best re-scan cost it?
3. **The map grows with scale** (~32 → ~183 ns on update-only A as levels go 1k →
   1M). Why?

These are hypotheses to test with counters, not conclusions.

### What each workload can and cannot tell you (corrected — Phase 3.1)

Workloads differ in *what they make the book do*, and a counter hypothesis must
pick the workload that actually exercises that behavior:

| If you want to study… | use workload | why |
|---|---|---|
| **Traversal / pointer-chasing / cache / TLB / branches / IPC** (a map update touches an existing node) | **A** update-only | A re-quantifies a random *present* level. It never inserts or erases, so it isolates the cost of *finding and touching* an existing tree node from the cost of changing the tree structure. |
| **The map's tree walk shape** | A vs B vs C vs D vs E | the same counters, compared across workloads whose update distributions differ (full-book vs top-of-book vs best-churn). |
| **The allocator / node create+destroy churn** | **B** (10% deletes, conserved) and **E** (uniform deletes/adds) | these actually call `insert_or_assign` on absent prices and `erase()` — the paths that allocate and free tree nodes. A does NOT. |
| The flat book's array store / cache-line behavior | **A** (or C for the re-scan) | flat A is a single-cache-line store; flat C adds the inward best re-scan. |

This corrects an earlier framing: workload A does **not** allocate or erase, so
it cannot speak to allocator cost. Map-A's scale growth (~32 → ~183 ns as levels
go 1k → 1M) must be explained by *traversal* — pointer-chasing cache misses, TLB
pressure, branch behavior — not by allocator churn, because A never changes the
level set. If allocator cost matters, measure it on B or E, which do.

## How the measurement boundary works (Phase 3.1)

Phase 2 measured a **pure `apply()` replay**: the stream is generated up front
(off the clock), the book is snapshot-loaded (untimed), and the timed region is
the apply loop with a per-iteration compiler barrier. For profiling to explain
those ns/update numbers, perf's counters must cover the **same region** — not
the process startup, not the enormous snapshot load (map at 1M levels builds a
million-node tree), not the stream generation.

The harness gates `perf stat` to exactly that region using the **perf control
interface**:

```
perf stat -D -1 --control=fifo:<ctl>,<ack> -e <events> ./build-perf/orderbook_bench ...
```

- `-D -1` starts the counters disabled.
- The benchmark, told the fifo pair via `LLOB_PERF_CONTROL=<base>`, opens
  `<base>_ctl`/`<base>_ack`, writes `enable\n`, waits for perf's
  acknowledgement, runs the timed apply loop, writes `disable\n`, waits for the
  second acknowledgement, and only then does its off-clock end-state reads.
  perf's counters therefore span **only** the timed `apply()` block.
- The acknowledgement is the literal `"ack\n"` for **both** enable and disable
  (perf never echoes the command — see `tools/perf/util/evlist.h`,
  `EVLIST_CTL_CMD_ACK_TAG`). The handshake lives **outside** the timed loop and
  outside the `Clock::now()` window, so the reported ns/update is unchanged in
  meaning.

The PMU window and the chrono window cover **the same apply() block**: enable is
sent before `t0`, disable is sent immediately after `t1`, and the end-state
reads come after disable. What perf counted and what the wall clock timed are the
same loop iterations.

The wall time that pairs with a counter row is the **benchmark's own chrono
ns/update** from that run. perf's "seconds time elapsed" line is NOT used as the
gated wall time (it can span a different enabled/alive window and is not a
chrono measure); perf's time-enabled/time-running/scaling is used for PMU
*quality* (did the pass multiplex? — see below), not as the timing.

Two consequences:

- **`reps=1` is enforced.** A profiling invocation with `reps != 1` is rejected
  by the harness (exit 2). The gate counts a single enable/disable window, and
  perf counters aggregated over several windows could not be paired with a
  best-of-N result. Best-of-N is a Phase 2 methodology for the *timed
  throughput* number; it does not apply to a single counted window. One
  profiling process, one benchmark cell, one measured apply block, one PMU
  counter window.
- **`perf record` is gated when the installed perf supports it.** The harness
  probes `perf record --help` for `--control`; if present, it runs
  `perf record -D -1 --control=fifo:<ctl>,<ack> --call-graph dwarf` with the
  same `LLOB_PERF_CONTROL` gate, so samples cover only the apply() block. On an
  older perf without record control support it **falls back** to a whole-process
  record that is explicitly labeled `record_gated=0` in the metadata — never
  silently claimed gated — and must be read filtered to the `apply()` frames.
  `--call-graph dwarf` is used for the normal optimized binary (no
  `-fno-omit-frame-pointer` rebuild needed); a frame-pointer build is only a
  documented alternative if DWARF unwinding is unavailable.

## Prerequisites (Linux)

- A Linux host (the code is portable C++20; `orderbook_bench` builds the same
  way — see the top-level README Build & test).
- `perf` from `linux-tools-<kernel>` / `linux-perf`, on PATH.
- Counter access: `perf stat`/`perf record` need a low `perf_event_paranoid`.
  As root or with `kernel.perf_event_paranoid <= 1` (often 2 by default blocks
  some events). Check with `cat /proc/sys/kernel/perf_event_paranoid`.
- Release build, one implementation per process (the same methodology as
  `scripts/bench.sh`): profile a single `(impl, workload, scale)` cell at a time.
- The harness uses `taskset` (for the optional `--cpu=N` pin) and records full
  host metadata; it does **not** auto-change system settings.

## How to run

Build and profile one cell (Linux only):

```sh
scripts/perf-profile.sh --run map C 1000000           # map, workload C, 1M levels
scripts/perf-profile.sh --run --cpu=2 map A 1000      # pin to CPU 2
scripts/perf-profile.sh --run --no-record flat A 1000000   # stat passes only
```

The harness is **dry-run by default**: without `--run` it prints the exact
commands without building or executing, so it is safe to inspect on any machine
(including this macOS dev box).

Per cell it runs, in a dedicated `build-perf/` directory (fresh configure —
no stale CMake cache or `BENCH_ARCH_FLAGS` can leak in from a canonical run):

1. **`perf stat` — three passes**, split to REDUCE the risk of multiplexing
   (fewer events per pass than a CPU typically has hardware counters). Each pass
   is a separate process, each gated to the timed apply loop by its own fifo
   pair, each `reps=1`. Splitting does NOT guarantee zero multiplexing — fixed
   counters, the NMI watchdog, or PMU availability can still multiplex a pass —
   so whether a pass actually multiplexed is read from perf's own output
   (time-running vs time-enabled, the `[n%]` scaling annotation), which is
   preserved verbatim. If a pass's running percentage is materially below 100%,
   derived metrics (IPC, miss rates, cycles/update) are unreliable for it and
   must be marked/adjusted in the analysis.
   - `core`: `cycles,instructions,branches,branch-misses` — **required**. After
     the pass the raw stat text is inspected regardless of perf's exit code; if
     it contains `<not supported>`/`<not counted>` or a core event's count is
     absent, the cell FAILS loudly (a profile with no cycles is not a profile).
   - `generic-cache`: `cache-references,cache-misses` — best-effort.
   - `optional-cpu`: `L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses` —
     best-effort; these are CPU-specific. An unsupported event is reported and
     left **out of the analysis** — perf's own raw text (`<not supported>`,
     `<not counted>`, `[n%]`) is preserved verbatim in `stat_<pass>.txt` and is
     the source of truth. The harness **never invents a zero** for an event perf
     could not count, and never relies on perf's exit status alone.
2. **`perf record --call-graph dwarf`** — a DWARF call graph saved to `perf.data`
   for `perf report`. Gated to the apply() block when the installed perf
   supports the record control interface; otherwise a labeled whole-process
   fallback. Skip with `--no-record`.

Output lands in `results/perf_<impl>_<wl>_<scale>_<timestamp>/` (git-ignored),
then is committed under `docs/results/` (see Recording real results).

### Target cell matrix (Phase 3.1)

The focused matrix that answers the three open questions with the fewest runs.
Each cell is one `(impl, workload, scale)`; the harness profiles one cell per
invocation. Each profiled cell's counters are read against the **same-host Linux
throughput baseline** (below), never against the M3 Max CSV.

| Cell | answers |
|---|---|
| `map A 1000` | map traversal at small working set |
| `map A 1000000` | map traversal / pointer-chasing / cache / TLB growth with scale (A never allocates — see correction above) |
| `map C 1000000` | "why is map C cheap" — near-touch locality / branches / tree-path |
| `map D 1000000` | top-of-book concentration contrast |
| `map E 1000000` | uniform random — worst-case pointer chasing **and** allocator churn (B/E do allocate) |
| `map B 1000000` | allocator churn (10% deletes, conserved) |
| `flat A 1000000` | where flat's per-op time goes (single-cache-line store) |
| `flat C 1000000` | cost of the inward best re-scan |

The map scale-growth question (`map A 1k` vs `map A 1M`) needs only the **core +
optional** passes (cycles, instructions, cache/TLB misses). The allocator
question needs **B or E** with the same passes — A cannot answer it. For map-C,
`branch-misses` (core) is the discriminator between a locality story and a
branch-predictability story.

### Same-host Linux throughput baseline

Mechanistic conclusions pair **Linux counters with Linux latency from the same
machine, compiler, and build**. Each `phase3-linux-<machine>/` dataset therefore
carries, alongside the profiled cells, an **ungated throughput baseline** run on
that same Linux host with the same `build-perf` binary and the canonical
methodology (`scripts/bench.sh`-style: per-process, best-of-reps). A cell's
counters are read against that Linux baseline's ns/update for the same
`(impl, workload, scale)`.

A profiled cell's own run also carries a wall time — the benchmark's gated
chrono ns/update from the same measured window — which is directly comparable to
its counters (cycles ÷ updates, etc.). Both are kept: the per-run gated ns/update
and the ungated baseline together anchor the counters in Linux latency.

## What the counters mean for these two designs

- **`instructions`** — total retired instructions. Compare map vs flat per op to
  separate "does more work" from "same work, slower memory". The flat book's
  `apply()` is a tight array store; the map's does a red-black search per op.
- **`cycles` / `IPC`** (`instructions` ÷ `cycles`) — a low IPC hints memory
  stalls or branch mispredicts rather than raw instruction count. The map's
  pointer chasing tends to stall on cache misses; the flat's random single-cache-
  line store may still miss L1 but stream well.
- **`cache-misses` / `LLC-load-misses`** — a level is an 8-byte slot (flat) or a
  heap node (map). Map growth with scale should show rising LLC/cache misses if
  pointer-chasing is the cause; flat's ~flat time should show ~scale-invariant
  misses per op. **Read these on workload A (traversal) or B/E (allocator) — A
  does not allocate, so on A they are pure traversal misses.**
- **`branch-misses`** — workload C's repeated best-delete may mispredict the
  flat's inward re-scan exit, or the map's tree walk. Near-touch repetition may
  *reduce* map mispredicts — a candidate explanation for "map C is cheap".
- **`dTLB-load-misses`** — the flat's 1M-level working set is ~16 MB across both
  sides; the map's nodes scatter. TLB pressure is part of the flat-vs-map gap.

**Which latency do counters explain?** Linux PMU counters must be paired with
**Linux** latency from the **same machine/compiler/build** — the same-host Linux
baseline described above, or the profiled cell's own gated chrono ns/update.
They must NOT be used to explain the absolute ns/update of the Apple M3 Max Phase
2 dataset: those numbers come from a different CPU, ISA, compiler, and memory
system, so a Linux `cache-misses` count has no quantitative meaning against an
M3 Max nanosecond figure.

The two measurement families therefore play distinct roles:

- **Apple M3 Max Phase 2** (`docs/results/phase2-m3max/`) — an independent
  throughput dataset, measured once, on the M3 Max. Frozen.
- **Linux Phase 3 host** (`docs/results/phase3-linux-<machine>/`) — a Linux
  throughput baseline **plus** Linux PMU counters, from the same machine /
  compiler / build. All mechanistic conclusions (pointer-chasing, TLB,
  branches, the "why is map C cheap" question) use **Linux latency ↔ Linux
  counters**.

The M3 Max dataset may be used only for **high-level cross-platform trend
comparison** (e.g. "map scales with book size on both platforms, flat is flat on
both"), never as the absolute latency paired with Linux hardware counters.
Counters explain the time; they do not replace it — and they explain the time
that was measured on the same host they were counted on.

## Recording real results (Phase 3B)

When run on Linux, commit the measured artifacts under
`docs/results/phase3-linux-<machine>/` (e.g. `phase3-linux-cpl-9010/`) — one
subdirectory per cell, named `perf_<impl>_<wl>_<scale>_<timestamp>/` exactly as
the harness produced it:

```
docs/results/phase3-linux-<machine>/perf_map_C_1000000_<ts>/
  bench_stdout_core.txt           benchmark stdout, core pass
  bench_stdout_generic-cache.txt  benchmark stdout, generic-cache pass
  bench_stdout_optional-cpu.txt   benchmark stdout, optional-cpu pass (or absent if unsupported)
  stat_core.txt                   raw perf stat, core pass (source of truth)
  stat_generic-cache.txt          raw perf stat, generic-cache pass
  stat_optional-cpu.txt           raw perf stat, optional-cpu pass (or absent)
  perf.data, record.log           call-graph profile (if --record; record.log records whether it was gated)
  host.txt                        hostname, uname -a, lscpu, compiler+flags, perf --version,
                                  perf_event_paranoid, affinity, governor, date
  command.txt                     the exact command(s) run
```

Each `phase3-linux-<machine>/` dataset also carries the **same-host Linux
throughput baseline**: an ungated `scripts/bench.sh`-style run on that host with
the same build, committed alongside the cells. Every cell's counters are read
against that baseline (or the cell's own gated chrono ns/update), never against
the M3 Max CSV.

The provenance split keeps the two measurement families clean:

- `docs/results/phase2-m3max/` — the Phase 2 canonical **M3 Max** throughput
  dataset (CSV + metadata; Apple M3 Max, macOS, Apple clang). Frozen,
  independent — used only for high-level cross-platform trend comparison.
- `docs/results/phase3-linux-<machine>/` — Phase 3 **Linux** data: the same-host
  Linux throughput baseline **plus** the PMU counters, from whatever Linux box
  actually ran perf. Each cell's `host.txt` records that box's
  compiler/build/flags. Linux counters are paired with Linux latency from the
  same machine/compiler/build — never spliced against M3 Max numbers as if they
  were one environment.

A results `README.md` under each `phase3-linux-<machine>/` adds the per-cell
table (cell, counters, cycles/update, IPC, miss rates) plus a metadata block in
the style of `RESULTS_METADATA.md`. **Only commit after inspection**: the raw
stat text shows the counters are real (`<not supported>` would be visible
there), and the bench row is present. Never invent numbers; only what a real
`perf` reported.

## Scope notes

- This profiles the **steady-state timed region**: with the fifo gate, perf
  stat's counters cover exactly the timed apply loop (the untimed
  snapshot/stream setup runs with counters disabled). perf record is gated to
  the same region when the installed perf supports record `--control`; a
  whole-process fallback (labeled `record_gated=0`) must be read filtered to
  the `apply()` frames.
- Single-core, single-writer, mean-throughput (same scope caveats as Phase 2).
  `--cpu=N` + `taskset` optionally pins to one core; the harness records CPU
  affinity and host metadata but never changes system settings itself.
- Phase 4 (latency percentiles) and Phase 5 (write-up) are not started.
