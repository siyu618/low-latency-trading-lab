# Phase 3 — profiling the Experiment 01 benchmark

Phase 3 provides the tooling to answer the "why" questions that Phase 2's
throughput numbers raised but could not answer. Phase 2 measured *how fast*
`apply()` is for each design; Phase 3 profiles *where the time goes*.

## Platform decision (Phase 3M / Phase 3L)

Phase 2's canonical numbers were measured on **Apple Silicon (Apple M3 Max,
macOS)** — see `docs/results/phase2-m3max/`. The currently available profiling
environment is that **same macOS machine**. Two profiling tracks therefore
exist:

- **Phase 3M — macOS / Apple Silicon profiling (ACTIVE).** Because the
  available host is the same machine that produced the Phase 2 dataset, Phase 3M
  profiles that machine with **Apple's native tools (Instruments)**. Latency
  (Phase 2 ns/update) and the microarchitectural / call-tree observations
  (Instruments) therefore come from the **same platform** — the M3 Max and its
  memory system — rather than from a different machine whose counters would not
  explain these numbers. Phase 3M uses an opt-in **os_signpost interval**
  (`LLOB_SIGNPOSTS=1`, Apple-only) around the timed `apply()` block so
  Instruments can scope a profile to exactly the measured region. See
  `MACOS_INSTRUMENTS.md`.
- **Phase 3L — Linux perf profiling (DEFERRED).** The Linux `perf` harness
  (`scripts/perf-profile.sh`) and its guide are preserved and ready, but there
  is **no Linux host** with perf on this project, so **no Linux PMU numbers
  exist**. Phase 3L will run on a real Linux machine when one is available.

Two honesty rules follow:

1. **Apple Instruments metrics and Linux perf PMU events are NOT directly
   equivalent.** They are different tools on different ISAs with different
   counter definitions (e.g. "CPU Counters" bottleneck categories vs perf's
   `cycles`/`cache-misses`). Do not treat a Phase 3M observation as if it were a
   Linux perf counter, and do not splice the two.
2. **Do not run Linux PMU experiments inside a VM and present them as native
   hardware measurements.** A hypervisor does not expose authentic hardware
   counters. Linux PMU data, when it exists, must come from a real Linux host.

## Status & honesty box

**No profiling numbers (perf OR Instruments) are presented anywhere in this
repository — none have been measured.** The Phase 3M tooling is authored and
committed, and the os_signpost marker is compile-validated on this Mac, but no
Instruments trace has been captured on it (this host has Command Line Tools
only — no full Xcode/Instruments). The Phase 3L harness is authored to run on a
Linux host and has not been executed against a real PMU. Any illustrative
output you may see in other docs is a labeled placeholder, never a measured
claim. Real Phase 3M results belong in
`docs/results/phase3-macos-apple-silicon/`; real Phase 3L results in
`docs/results/phase3-linux-<machine>/`.

The benchmark CLI and the normal Phase 2 hot path are FROZEN. Phase 3 added
strictly opt-in profiling markers (below); a normal run — no `LLOB_PERF_CONTROL`
env var, no `LLOB_SIGNPOSTS` — is byte-for-byte unchanged.

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

## How the measurement boundary works (Phase 3.1 / 3L)

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

**Boundary overhead is real but small and fixed.** The gate is tightly aligned
around the apply() block, but it is not literally instruction-for-instruction
identical to the `[t0, t1]` chrono window: there is a small fixed cost between
perf enable/ack and `t0`, and between `t1` and the actual perf disable (the
handshake itself). This boundary overhead is **per timed block, not per update**,
and the measured block is large (2,000,000 updates by default), so the overhead
is negligible relative to the block. It is outside both the chrono and the PMU
window, so it does not change the reported ns/update's meaning — but a profile
must be read at block granularity, not as if every boundary instruction were
part of an update.

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

### macOS (Phase 3M): the os_signpost marker

On Apple platforms the benchmark offers a separate, macOS-only opt-in marker for
the same timed region:

```
LLOB_SIGNPOSTS=1 ./build-perf/orderbook_bench map C 1000000 updates=2000000 reps=1
```

With `LLOB_SIGNPOSTS=1` the benchmark wraps the identical apply() block (begin
just before `t0`, end just after `t1`) in an **os_signpost interval** named
`llob.apply.block` on the default log. Instruments (Time Profiler / CPU Counters)
and signpost-aware `log` queries can then scope a recording to exactly that
interval. Like the perf gate it is **strictly opt-in** — unset (the default)
emits nothing and normal runs are byte-for-byte unchanged — and it sits outside
the per-update loop, adding only the same small fixed per-block boundary cost
described above. It is compiled only on `__APPLE__` and is never present on
Linux. See `MACOS_INSTRUMENTS.md` for the full workflow and how this CLT-only
host limits what can be captured here.

## Prerequisites (Linux — Phase 3L)

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

## How to run (Linux — Phase 3L)

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

### Same-host Linux throughput baseline (Phase 3L)

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

The same principle holds for Phase 3M: Apple Instruments observations are read
against the same-host macOS ns/update — the Phase 2 canonical M3 Max numbers —
not against a Linux number.

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

**Which latency do counters explain?** A profiling observation must be paired
with the latency measured on the **same machine, compiler, and build**. Because
Phase 3M and Phase 3L are on different platforms, each has its own pairing:

- **Phase 3M (macOS, this machine)** — Instruments observations are read
  against the **same-host macOS ns/update** (the Phase 2 canonical M3 Max
  numbers, or a fresh same-host run with the same build). The M3 Max is the only
  machine that produced Phase 2 latency, so Phase 3M observations can explain it.
- **Phase 3L (Linux, future)** — Linux perf counters must be paired with
  **Linux** latency from the **same Linux machine/compiler/build** (a same-host
  Linux baseline or the profiled cell's own gated chrono ns/update). Linux
  counters must NOT be used to explain the absolute M3 Max Phase 2 numbers: those
  come from a different CPU, ISA, compiler, and memory system.

The measurement families play distinct roles:

- **Apple M3 Max Phase 2** (`docs/results/phase2-m3max/`) — the canonical
  throughput dataset, measured once, on the M3 Max. Frozen.
- **Phase 3M results** (`docs/results/phase3-macos-apple-silicon/`) — Apple
  Instruments observations on that same M3 Max host (Time Profiler call trees,
  CPU Counters where supported), read against the same-host Phase 2 ns/update.
- **Linux Phase 3L host** (`docs/results/phase3-linux-<machine>/`) — a Linux
  throughput baseline **plus** Linux PMU counters, from the same machine /
  compiler / build. All mechanistic conclusions made on Linux use **Linux
  latency ↔ Linux counters**.

Cross-platform, the M3 Max dataset may be used only for **high-level trend
comparison** (e.g. "map scales with book size on both platforms, flat is flat on
both"), never as the absolute latency paired with another platform's counters.
Observations explain the time that was measured on the same host they were taken
on; they do not replace it.

## Recording real results

The two tracks record under different `docs/results/` trees.

### Phase 3M (macOS) — `docs/results/phase3-macos-apple-silicon/`

This tree exists now (empty of data until a real recording). After an
Instruments capture is inspected, commit per cell:

```
docs/results/phase3-macos-apple-silicon/<cell>/        e.g. map_A_1000000/
  host.txt                  scripts/collect-macos-profile-metadata.sh output
  command.txt               the exact benchmark + instrumentation command(s)
  bench_stdout.txt          the benchmark's same-host ns/update row(s)
  time_profiler_<n>.txt     exported Time Profiler summary (call tree)
  cpu_counters_<n>.txt      exported CPU Counters summary (where supported)
  trace_notes.md            what was recorded, template, signpost interval used
```

The Phase 2 canonical M3 Max CSV (`docs/results/phase2-m3max/`) is the
same-host latency baseline this track reads against. Each cell's `host.txt`
records macOS/chip/build provenance via
`scripts/collect-macos-profile-metadata.sh`. The per-cell
`PHASE3_MACOS_ANALYSIS.md` (top-level of the track) must label every claim
`MEASURED` / `OBSERVED IN INSTRUMENTS` / `INTERPRETATION` / `LIMITATION` and
never present a hypothesis as measured fact.

### Phase 3L (Linux) — `docs/results/phase3-linux-<machine>/`

When run on a real Linux host, commit the measured artifacts under
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

The provenance split keeps the measurement families clean:

- `docs/results/phase2-m3max/` — the Phase 2 canonical **M3 Max** throughput
  dataset (CSV + metadata; Apple M3 Max, macOS, Apple clang). Frozen,
  independent — used only as the same-host latency baseline for Phase 3M and for
  high-level cross-platform trend comparison.
- `docs/results/phase3-macos-apple-silicon/` — Phase 3M **Apple Instruments**
  data on the M3 Max host. Empty until a real recording.
- `docs/results/phase3-linux-<machine>/` — Phase 3L **Linux** data: the same-host
  Linux throughput baseline **plus** the PMU counters, from whatever Linux box
  actually ran perf. Each cell's `host.txt` records that box's
  compiler/build/flags. Linux counters are paired with Linux latency from the
  same machine/compiler/build — never spliced against M3 Max numbers as if they
  were one environment.

A results `README.md` under each tree adds the per-cell table plus a metadata
block in the style of `RESULTS_METADATA.md`. **Only commit after inspection**:
for Linux the raw stat text shows the counters are real (`<not supported>` would
be visible there), and the bench row is present. Never invent numbers; only what
a real tool reported.

## Scope notes

- This profiles the **steady-state timed region** of the benchmark. Phase 3L
  gates perf stat/record to the apply() block with the fifo control interface;
  Phase 3M scopes an Instruments capture to the os_signpost `llob.apply.block`
  interval. Both keep the untimed snapshot/stream setup out of the observed
  region.
- Single-core, single-writer, mean-throughput (same scope caveats as Phase 2).
  The Linux harness optionally pins via `taskset --cpu=N`; the macOS track does
  NOT claim core pinning (Apple Silicon P/E cores and the OS scheduler move
  threads freely — see `collect-macos-profile-metadata.sh` and
  `MACOS_INSTRUMENTS.md`). Neither tool changes system settings.
- Phase 3L (Linux) status — **tooling READY, native measurement DEFERRED** (no
  Linux host). Phase 3M (macOS) status — **workflow READY, no Instruments trace
  captured yet** on this CLT-only host. No profiling numbers exist anywhere in
  this repository.
- Phase 4 (latency percentiles) and Phase 5 (write-up) are not started.
