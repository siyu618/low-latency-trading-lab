# Phase 3M — macOS / Apple Silicon profiling with Instruments

Phase 3M profiles the Experiment 01 benchmark on the **same Apple Silicon
machine that produced the Phase 2 canonical numbers** (Apple M3 Max, macOS —
`docs/results/phase2-m3max/`), using **Apple's native Instruments** tools. Its
observations and the Phase 2 latency come from the same platform, so the
observations can explain that latency. See the platform-decision block in
`README.md` (this directory) for why this is a separate track from the deferred
Linux `perf` work (Phase 3L).

## Honesty box

**No real Instruments trace is committed yet.** The workflow below is authored
and the tooling is committed and compile-validated, but no recording has been
made and inspected. Whether a given machine can record is **detected at
recording time**, never asserted as a permanent host claim in this file:

- `xcrun --find xctrace` — prints a path when full Xcode (Instruments) is
  installed; fails when only the command-line tools are present. A real capture
  needs the former.
- `xcrun xctrace list templates` — the exact template names that machine offers.
  The helper matches a requested name against this list exactly and refuses a
  partial match.
- `scripts/phase3m-instruments.sh` stops (exit 3) with a clear message when
  xctrace is absent instead of guessing.
- The machine/tool availability of the machine that actually records is captured
  at recording time by `scripts/collect-macos-profile-metadata.sh` into that
  recording's `host.txt` — not asserted here.
- Nothing under `docs/results/phase3-macos-apple-silicon/` is populated yet; it
  is filled only after a **real** recording is inspected.
- **Apple Instruments metrics are NOT Linux perf PMU events.** Terminology,
  counter definitions, and what is measurable differ. This guide uses Apple's
  native terms ("CPU Counters", bottleneck categories) and does not pretend to
  produce `cycles`/`cache-misses`/`IPC`/`LLC-misses` unless the installed tool
  genuinely exposes a reliable equivalent. Do not splice Instruments numbers
  into Linux-perf-formatted metrics.

## Apple Silicon scheduling — an experimental limitation

The M3 Max mixes performance (P) and efficiency (E) cores, and macOS schedules
threads across them freely. A benchmark process can migrate between cores across
reps, mixing P-core and E-core timing. Consequences:

- **Strict CPU-core pinning is NOT claimed.** Apple does not expose Linux-style
  `taskset` affinity for unprivileged processes in a way this project relies on.
  If a recording is made with a pinning mechanism, it must be stated and
  verified; otherwise the scheduling caveat stands.
- Record and compare like with like: keep each cell's recording to **one process
  invocation, one measured rep** (so the process is less likely to migrate
  mid-block), and note the core mix in `host.txt` if observable.
- `scripts/collect-macos-profile-metadata.sh` records the P/E topology and flags
  this limitation in its output.

## Prerequisites

- A macOS / Apple Silicon host.
- **Full Xcode** (Instruments.app + `xctrace`). To record on a given machine it
  must have Xcode installed and `xctrace` reachable — check with
  `xcrun --find xctrace`. The helper stops with a clear message when it is not.
- The benchmark builds with the system toolchain as usual (Release, no arch
  flag — the Phase 2 canonical build).

## Workflow

### 1. Build the profiling binary

```sh
rm -rf build-perf
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS=
cmake --build build-perf
```

### 2. Establish same-host latency (the baseline the trace explains)

Run the cell normally (ungated, canonical methodology) to get the ns/update the
Instruments observation will be read against:

```sh
./build-perf/orderbook_bench map C 1000000 updates=2000000 reps=3
```

Record the exact stdout. This is the same-host latency anchor.

### 3. Opening / invoking Instruments

Two ways:

- **GUI**: `open -a Instruments` (or run the bench under Time Profiler manually).
- **CLI** (full Xcode only): `scripts/phase3m-instruments.sh`.

The helper records a cell under a chosen template and prints how to open the
`.trace`:

```sh
scripts/phase3m-instruments.sh --template="Time Profiler" map A 1000000 --signposts
# Time Profiler is the default template; --signposts emits the apply interval.
scripts/phase3m-instruments.sh --template="CPU Counters" flat C 1000000 --signposts
```

Templates are **detected** from `xctrace list templates`, never assumed; if a
template is unavailable the helper lists what is installed and exits 3. A
recording needs a large update count so the apply() block yields enough samples
(see §6).

### 4. Using Time Profiler

- Record the cell with Time Profiler selected.
- In the trace, find the `orderbook_bench` process and drill into its call tree.
- **Scope to the measured block** by selecting the `llob.apply.block` signpost
  interval (Time Profiler shows signpost intervals for the process; selecting
  one restricts the samples to that interval). This keeps the enormous untimed
  snapshot load (map at 1M levels builds a million-node tree) and the stream
  generation out of what you read.
- Read the **call tree / heavy path** for the functions in §7.

### 5. Using CPU Counters (when available)

"CPU Counters" (and the related bottleneck-analysis UI) exist in Instruments on
Xcode versions and Apple Silicon generations that support them. When present:

- Use **Apple's own categories/terminology** (e.g. instruction-delivery,
  instruction-processing / memory-related stalls, branch mispredicts, and the
  workload-size-dependent categories the tool exposes). Do not relabel them as
  Linux perf events.
- Record the raw trace/table. Export the textual summary if the tool offers it.
- Only derive numeric metrics (e.g. a miss/update or a per-update rate) if the
  installed tool genuinely exposes a reliable count to derive from; otherwise
  keep the observation qualitative (e.g. "the map shows high cache-miss stall
  weight that grows with scale") and mark it `OBSERVED IN INSTRUMENTS`.

If the current hardware/Xcode does **not** support CPU Counters, do not fake it:
record Time Profiler only and say the CPU Counters instrument was unavailable.

### 6. Choosing the update count for a recording

- For **perf-stat-style counter passes** use the Phase 2 methodology values
  (`updates=2000000`, `reps=1`) so the trace matches the baseline.
- For **Time Profiler sampling**, a too-short measured block yields too few
  samples. If a flat cell (a few ns/update over 2M ops is still ~10 ms — usually
  enough) produces a sparse call tree, **rerun ONLY the record experiment** with
  a larger update count (e.g. `updates=20000000`, `reps=1`) and label it
  `RECORD-ONLY` in the trace notes. Do not mix that different update count into
  any per-update counter derivation, and do not relabel it as the Phase 2
  methodology.

### 7. What to inspect

Map:

- `MapOrderBook::apply`
- `std::map` / `std::_Rb_tree` lookup, traversal, insertion/rebalancing internals
- `erase` and node allocation/free where a workload actually does them (B/E)

Flat:

- `FlatOrderBook::apply`
- `rescan_after_delete` (workload C's inward best re-scan)
- `scan_best_from`

**Do not predict percentages before measuring.** Record what the trace actually
shows; the analysis under `docs/results/phase3-macos-apple-silicon/` will label
it correctly.

### 8. Comparisons to run

The focused matrix (each an independent process, `reps=1`):

| Cell | Question it addresses |
|---|---|
| `map A 1000` vs `map A 1000000` | Why does map latency grow as the tree grows? (traversal / pointer-chasing / cache / branches / instructions) |
| `map A 1000000` / `map C 1000000` / `map E 1000000` | Why is map C cheaper than map A/E? (locality, branch predictability, tree path) |
| `flat A 1000000` vs `flat C 1000000` | What does the flat book's best-price rescan actually cost? |

Workload A re-quantifies existing nodes and does **not** allocate in the timed
path — do not attribute map-A's scale growth to allocator churn without
evidence. Workload C's frequent best-delete is **not** simply the map erasing
its tree root — check the actual erase/reinsert path and locality. Workload E
uniformly deletes/adds across the whole tree, which is where allocator activity
(if any) would show.

### 9. Exporting / recording useful summaries

- **Time Profiler**: File → Export, or use the textual call-tree summary. Save
  as `time_profiler_<n>.txt` per cell.
- **CPU Counters**: export the table/summary it provides as
  `cpu_counters_<n>.txt`.
- Record the exact command(s), the template, whether the signpost interval was
  used, and the host metadata (`scripts/collect-macos-profile-metadata.sh`) in
  the cell's `trace_notes.md`.

Then commit the inspected output under
`docs/results/phase3-macos-apple-silicon/` (see `README.md` in this directory
for the layout and the `MEASURED` / `OBSERVED IN INSTRUMENTS` / `INTERPRETATION`
/ `LIMITATION` labeling the analysis must use).

### 10. Processor Trace (Apple PT)

Apple Processor Trace is **optional** and is **not** a dependency of Phase 3M.
Only document or use it if this Mac's hardware and the installed macOS/Xcode
actually support recording Processor Trace; otherwise ignore it. Nothing in this
workflow requires it.

## Recording-time environment

Machine/tool availability is **not** asserted in this static file. At recording
time, `scripts/collect-macos-profile-metadata.sh` records the host's actual
state (macOS version, chip/model, compiler, whether Xcode / `xctrace` /
Instruments are present) into the recording's `host.txt`. The os_signpost marker
is compile- and runtime-validated (it emits exactly once per timed block when
`LLOB_SIGNPOSTS=1` and is a strict no-op otherwise); visual confirmation of the
interval inside an Instruments recording is done on the machine that records.
