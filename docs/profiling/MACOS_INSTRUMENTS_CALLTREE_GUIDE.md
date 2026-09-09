# Phase 3M — GUI call-tree read (the missing call-tree step)

The six committed `recording.trace` bundles under
`docs/results/phase3-macos-apple-silicon/<cell>/` were recorded at commit
`8383dc8` from a Release `build-perf/orderbook_bench` whose build UUID is
**`71FA34F8-C407-3B81-AC75-F65607749366`**. That UUID is present in every
cell's `.symbolsarchive`, so **the GUI can symbolize these traces as-is** —
open the `.trace`, and Instruments matches the recorded binary to the on-disk
one by UUID and resolves every sample PC to a symbol. **No re-record is
needed; the call trees are already in the traces.**

## Why headless cannot do this (LIMITATION, documented)

Headless `xctrace export` of these **deferred** recordings yields only raw PCs /
fragment ids. The blocker is not missing symbols (the Release binary keeps them)
but the **per-run ASLR slide**: each sample PC must be relocated by its run's
real load address, and these `.trace`s did not persist the process image list
(the `dyld-library-load` / kdebug tables are empty in a deferred recording).
`atos` therefore cannot be pointed at a trustworthy base, and the recorded
`.symbolsarchive`s store only image UUIDs + segment layout, not the symbol
table. Instruments resolves the slide internally from the image list it holds
in memory while the trace is open. This is a **tooling limitation**, not a gap
in the data.

## Prerequisite for symbolization

The GUI must be able to find a binary with the recorded UUID
`71FA34F8-C407-3B81-AC75-F65607749366`. Keep the same build available, e.g.:

```sh
# reproduce the exact recorded build if build-perf has since been rebuilt
rm -rf build-perf
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS=
cmake --build build-perf
dwarfdump --uuid build-perf/orderbook_bench   # must print 71FA34F8-...
```

Instruments' default symbol search includes the process's original path
(`./build-perf/orderbook_bench`) and Spotlight; if a cell shows unsymbolized
addresses, point Instruments at `build-perf/orderbook_bench` (or its dSYM, if
you add `-g` to a dedicated profiling build) via
File → Symbols → the recorded UUID.

> To make symbolization fully robust for *future* recordings, add a **dSYM**:
> `cmake --build build-perf` with `CMAKE_BUILD_TYPE=RelWithDebInfo` or
> `-g`-style debug info, and keep `build-perf/*.dSYM` next to the binary when
> recording. This trace set was recorded without one, so rely on the UUID match
> above.

## Per-cell GUI procedure (repeat for each of the six cells)

1. Open the trace:
   ```sh
   open docs/results/phase3-macos-apple-silicon/map_A_1000/recording.trace
   ```
   (substitute each cell; do one at a time).

2. In the Time Profiler detail, select the **`orderbook_bench` (pid …)**
   process and make sure the call tree is **symbolized** (see prerequisite). If
   frames show as `0x…`, fix the symbol source first.

3. **Scope to the timed `apply()` block.** The CLI recordings carry **no
   signpost interval** (see `docs/profiling/MACOS_INSTRUMENTS.md` §3), so you
   cannot select `llob.apply.block` to trim the read. Instead:
   - **Hide system libraries** (the "Hide System Libraries" toggle) and read the
     call tree for the `orderbook_bench` frames. Note that in the short, fast
     flat cells a large share of even the sampled wall-time is the **untimed
     1M-node snapshot load + process startup**, not `apply()` — do not read a
     whole-process percentage as an apply percentage (see step 5).

4. For each cell record, per the labels in the honesty rule:
   - **Top call-tree rows**: the few functions at the top of the heavy path
     with their **self % and total %** (as Instruments reports them).
   - The **% of samples in `MapOrderBook::apply` / `FlatOrderBook::apply`**
     (and, for workload E, whether allocator frames like `operator new` /
     `__tree` node allocation appear; for flat C, the inward best re-scan
     path — `FlatOrderBook::rescan_after_delete` / `scan_best_from`).
   - The **weight of `std::__1::map`/`__tree` internals**
     (`__find_equal`, `insert_or_assign`, `__tree_remove`,
     `__tree_balance_after_insert`) vs `MapOrderBook::apply` itself.

5. **Frames that are NOT the timed block** (label them LIMITATION if they
   dominate): the 1M-node snapshot build goes through
   `vector<unsigned char>::assign` / `load_snapshot`, and process startup /
   stream generation through `StreamGen::*`. For the **flat** cells especially,
   most sampled wall-time is these untimed phases (a 20M-update flat apply is
   only ~0.1 s of a ~1 s process), so a flat "whole process" call tree is mostly
   snapshot load, NOT apply. Read flat cells as apply only if you can separate
   the timed block; otherwise record the split honestly and label it.

## What to write down (the deliverable)

Into each cell's `trace_notes.md` (or a new `call_tree_<cell>.txt`), record:

```
cell: <cell>
top_frames_by_self:   <function> <self%>  / <function> <self%> ...
apply_share_of_samples: <MapOrderBook|FlatOrderBook>::apply  <NN.N>%
tree_internals_share:  <__find_equal|insert_or_assign|...>   <NN.N>%
allocator_frames_seen: <yes|no> (workload E)
rescan_frames_seen:    <...> (flat C)
notable_system_share:  <NN.N>%  (unscoped reads only)
scope_note: how the timed block was (or was not) isolated
recorded_uuid_match: 71FA34F8-C407-3B81-AC75-F65607749366
```

Then update `PHASE3_MACOS_ANALYSIS.md`: promote the three `INTERPRETATION`
rows to `OBSERVED IN INSTRUMENTS` where the call tree confirms or refutes them
(Q1 map scale growth = where the samples concentrate; Q2 map-C-cheap = tree
internals vs apply self; Q3 flat-C-rescan = the rescan path's measured weight),
and add the LIMITATION that unscoped flat reads include snapshot load.

## Cells & what to look for (the three "why" questions)

| Cell | What to confirm/refute in the call tree |
|---|---|
| `map_A_1000` | where apply's samples concentrate at small scale (shallow tree) |
| `map_A_1000000` | apply self vs tree internals; does depth push weight into `__find_equal`? |
| `map_C_1000000` | why C is cheaper — do samples stay in apply / shallow path? |
| `map_E_1000000` | does uniform churn surface allocator frames? where does the wide tail go? |
| `flat_A_1000000` | near-zero apply share of process (mostly snapshot load) — read with care |
| `flat_C_1000000` | the rescan path's measured weight vs flat A |
