# Phase 3M — macOS / Apple Silicon profiling results

**Status: EMPTY — no real Instruments recording exists yet.**

This tree is the committed home for Phase 3M (Apple Instruments on the M3 Max)
measurements. It is intentionally **not populated**: this host is Command Line
Tools only (no Instruments / xctrace), so nothing has been captured. It is
filled only after a real recording is inspected, following the workflow in
`docs/profiling/MACOS_INSTRUMENTS.md`.

The Phase 2 canonical dataset (`../phase2-m3max/`) was measured on this same
Apple M3 Max / macOS machine, so it is the **same-host latency baseline** that
Phase 3M observations are read against.

## What belongs here after a real recording

One subdirectory per profiled cell (e.g. `map_A_1000000/`), each containing:

- `host.txt` — `scripts/collect-macos-profile-metadata.sh` output (macOS, chip,
  Xcode/Instruments availability, clang, commit).
- `command.txt` — the exact benchmark + instrumentation command(s).
- `bench_stdout.txt` — the cell's same-host ns/update row(s).
- `time_profiler_<n>.txt` — exported Time Profiler summary (call tree).
- `cpu_counters_<n>.txt` — exported CPU Counters summary, when the instrument is
  available.
- `trace_notes.md` — template used, signpost interval used, update count, and a
  `RECORD-ONLY` label if a record experiment used a different update count than
  the Phase 2 methodology.

Plus a top-level `PHASE3_MACOS_ANALYSIS.md` answering the Phase 2 "why"
questions with this machine's own evidence.

## Honesty rule (the analysis must label every claim)

`PHASE3_MACOS_ANALYSIS.md` (and every per-cell note) must keep these categories
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
