# Phase 3M — macOS / Apple Silicon profiling results

**Status: populated — six real Time Profiler recordings, committed and
cross-checked headlessly. Call-tree symbolization is still PENDING an
Instruments GUI pass** (see the LIMITATION in each cell's `trace_notes.md`).

This tree is the committed home for Phase 3M (Apple Instruments on the M3 Max)
measurements. It holds six real recordings from the **same Apple M3 Max / macOS
14.2.1 machine** that produced the Phase 2 canonical dataset
(`../phase2-m3max/`), recorded at commit `8383dc8` (tree clean). The Phase 2
dataset is therefore the **same-host latency baseline** that these observations
are read against.

## The recordings

Six cells, each an independent process, `reps=1`, Time Profiler template,
recorded 2026-09-09 (UTC+8). Each trace is a **RECORD-ONLY** recording at
`updates=20000000` (not the Phase 2 methodology value `2000000`) so the sampler
had enough time to accumulate samples — see `docs/profiling/MACOS_INSTRUMENTS.md`
§6 and the per-cell notes. The **ns/update anchor** per cell is a separate,
same-host, ungated `updates=2000000 reps=1` run.

| Cell | Anchor ns/update | Recording | Samples | Core mix (P/E) |
|---|---|---|---|---|
| `map_A_1000` | 34.9 | 1.72 s | 1,166 | 1,133 / 33 |
| `map_A_1000000` | 219.7 | 6.30 s | 5,693 | 5,692 / 1 |
| `map_C_1000000` | 75.6 | 2.30 s | 2,209 | 2,209 / 0 |
| `map_E_1000000` | 513.9 | 32.28 s | 32,117 | 32,109 / 8 |
| `flat_A_1000000` | 5.1 | 0.95 s | 545 | 511 / 34 |
| `flat_C_1000000` | 6.7 | 0.77 s | 693 | 693 / 0 |

Anchor numbers are the same-host `best_ns_per_update` at the Phase 2
methodology (`anchor_ns.txt` per cell); all samples were `Running` /
`Timer Fired`.

## What is in each cell directory

- `recording.trace` — the committed Instruments trace (Time Profiler).
- `command.txt` — the exact effective recording command (`--launch` line).
- `bench_stdout.txt` — the recording's own stdout; on this host xctrace does
  **not** forward target stdout as text, so this file carries an explicit note
  rather than a fabricated number.
- `anchor_command.txt` / `anchor_ns.txt` — the same-host, ungated Phase 2
  methodology anchor run.
- `host.txt` — `scripts/collect-macos-profile-metadata.sh` output (macOS, chip,
  Xcode/Instruments availability, clang, commit `8383dc8`, tree clean).
- `trace_notes.md` — per-cell provenance, RECORD-ONLY label, and the headless
  facts derived from the committed trace.

The analysis that answers the Phase 2 "why" questions lives in
`PHASE3_MACOS_ANALYSIS.md`.

## Honesty rule (every claim is labeled)

`PHASE3_MACOS_ANALYSIS.md` (and every per-cell note) keeps these categories
explicit and separate:

- **MEASURED** — a number the benchmark itself reported (ns/update) or a tool
  exported directly.
- **OBSERVED IN INSTRUMENTS** — what a Time Profiler / CPU Counters recording
  actually shows (a call tree, a stall category weight), stated in the tool's own
  terms. Not a derived metric unless the tool exposes the raw count to derive it.
- **INTERPRETATION / HYPOTHESIS** — a proposed explanation. Never presented as a
  measured fact.
- **LIMITATION** — scheduling (P/E cores), sample count, unavailable instrument,
  boundary overhead, anything that qualifies the claim.

**Never present a hypothesis as a measured fact, and never invent an
Instruments number.** If an instrument is unavailable on the recording machine,
say so rather than manufacturing an equivalent.

## Current limitation: no symbolized call tree (GUI pass pending)

Headless `xctrace export` of a **Release** trace yields only raw PC addresses /
fragment ids, not symbol names; symbolication is not possible headlessly on this
host (the Release binary is stripped, and the trace's symbolsarchive is only a
UUID reference). The committed recordings prove the process ran, where its
1 ms samples landed at the page level, and the P/E-core mix — but **per-function
call-tree attribution is not yet derived from them**. That requires opening each
`recording.trace` in the Instruments GUI and reading the heavy path for
`MapOrderBook::apply` / `FlatOrderBook::apply`. Until that pass, this tree holds
verified traces + anchors, and the analysis labels the call-tree gap explicitly.
This is a tooling limitation, not a measurement gap of the code.
