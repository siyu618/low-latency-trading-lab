# Trace notes — `map A 1000000` (RECORD-ONLY)

## Provenance

| | |
|---|---|
| Cell | `map A 1000000` |
| Recording command | `xctrace record --template 'Time Profiler' --launch -- ./build-perf/orderbook_bench map A 1000000 updates=20000000 reps=1` |
| Template | Time Profiler |
| Signpost interval | none captured (see the CLI-recording limitation in `docs/profiling/MACOS_INSTRUMENTS.md` §3) |
| Recording time | 2026-09-09 10:45 (UTC+8) |
| Recorded commit | `8383dc8` (`8383dc88c1999ef471e38fe9be806daa60a29b38`), tree clean |
| Recording host | Apple M3 Max, 14 cores (10 P / 4 E), macOS 14.2.1 — `host.txt` |

## RECORD-ONLY label

This trace is a **RECORD-ONLY** recording: the update count is `20,000,000`
(`reps=1`), **not** the Phase 2 methodology value `2,000,000`. It exists to give
the Time Profiler sampler enough time to accumulate samples inside the measured
apply block (see `MACOS_INSTRUMENTS.md` §6). Per the honesty rules it must not
be used for any per-update counter derivation and must not be relabeled as the
Phase 2 methodology.

## Same-host latency anchor (ungated, Phase 2 methodology)

The recording's own stdout data row is **not** forwarded as text by this
xctrace (`bench_stdout.txt` carries the note, not a number). The ns/update
anchor below was measured **separately, same host, ungated**, at the Phase 2
methodology `updates=2000000 reps=1` on the same commit `8383dc8`:

```
map,A,1000000,2000000,439.468,219.734,4550959
```

`219.734 ns/update` is the **same-host latency baseline** this trace's
observations are read against. This is the Phase 2 "why does map latency grow
with tree size" cell (`map A 1000` ≈ 35 ns → `map A 1000000` ≈ 220 ns).

## Observed facts from the trace (headless export)

These facts are derived from the committed `recording.trace` by exporting the
`time-sample` table (`xctrace export --xpath ...`) and resolving each row's
inline-or-referenced ids. Export is headless; no symbolization is available for
the stripped Release binary, so this is **not** a call-tree inspection (see
limitation below).

| Fact | Value |
|---|---|
| Process | `orderbook_bench` pid 40842, exit 0 |
| Process duration | 6.30 s |
| Time-sample rows | 5,693 |
| Sample kind | 5,693 × Timer Fired |
| Thread state | 5,693 × Running |
| Core mix | 5,692 P-core / 1 E-core, across 11 distinct cores |
| Distinct code fragments | 51 (unresolved PCs; see limitation) |

The thread stayed Running throughout and was sampled almost entirely on the
P-cores (5,692 / 5,693). All facts above are `OBSERVED IN INSTRUMENTS`
(time-sample table), not derived latency metrics.

## LIMITATION — no symbolized call tree (needs Instruments GUI)

Headless `xctrace export` on a **Release build** yields only raw PC addresses /
fragment ids, not symbol names; symbolication of the time-sample call stacks is
**not possible without the Instruments GUI** on this host (the Release binary is
stripped and the `.trace`'s symbolsarchive is only a UUID reference). So this
recording proves *that* the process ran hot and where the samples landed at the
page level, but the **heavy-path / call-tree attribution is pending a GUI
inspection** (open `recording.trace` in Instruments and read the call tree for
`MapOrderBook::apply`). Until then, no per-function percentage is claimed from
this trace. This is a tooling limitation, not a measurement gap of the code.

## Cross-reference

- Host / tool availability: `host.txt`
- Exact effective command: `command.txt`
- Raw recording stdout note: `bench_stdout.txt`
- Workflow / methodology: `docs/profiling/MACOS_INSTRUMENTS.md`
