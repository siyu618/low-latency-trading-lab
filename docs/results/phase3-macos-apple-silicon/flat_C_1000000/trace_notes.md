# Trace notes — `flat C 1000000` (RECORD-ONLY)

## Provenance

| | |
|---|---|
| Cell | `flat C 1000000` |
| Recording command | `xctrace record --template 'Time Profiler' --launch -- ./build-perf/orderbook_bench flat C 1000000 updates=20000000 reps=1` |
| Template | Time Profiler |
| Signpost interval | none captured (see the CLI-recording limitation in `docs/profiling/MACOS_INSTRUMENTS.md` §3) |
| Recording time | 2026-09-09 10:47 (UTC+8) |
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
flat,C,1000000,2000000,13.395,6.697,149311762
```

`6.697 ns/update` is the **same-host latency baseline** this trace's
observations are read against. This is the Phase 2 "what does the flat book's
best-price rescan on workload C actually cost" cell (flat C ≈ 6.7 ns vs
flat A ≈ 5.1 ns at 1M).

## Observed facts from the trace (headless export)

These facts are derived from the committed `recording.trace` by exporting the
`time-sample` table (`xctrace export --xpath ...`) and resolving each row's
inline-or-referenced ids. Export is headless; no symbolization is available for
the stripped Release binary, so this is **not** a call-tree inspection (see
limitation below).

| Fact | Value |
|---|---|
| Process | `orderbook_bench` pid 42028, exit 0 |
| Process duration | 0.77 s |
| Time-sample rows | 693 |
| Sample kind | 693 × Timer Fired |
| Thread state | 693 × Running |
| Core mix | 693 P-core / 0 E-core, across 9 distinct cores |
| Distinct code fragments | 12 (unresolved PCs; see limitation) |

The thread stayed Running throughout and was sampled exclusively on the
P-cores. Like `flat A 1000000`, the sample count (693) is small because the flat
apply block is only ~0.13 s of the ~0.77 s process; the rest is the untimed 1M-node
snapshot load + process startup. All facts above are `OBSERVED IN INSTRUMENTS`
(time-sample table), not derived latency metrics.

## LIMITATION — no symbolized call tree (needs Instruments GUI)

Headless `xctrace export` on a **Release build** yields only raw PC addresses /
fragment ids, not symbol names; symbolication of the time-sample call stacks is
**not possible without the Instruments GUI** on this host (the Release binary is
stripped and the `.trace`'s symbolsarchive is only a UUID reference). So this
recording proves *that* the process ran hot and where the samples landed at the
page level, but the **heavy-path / call-tree attribution is pending a GUI
inspection** (open `recording.trace` in Instruments and read the call tree for
`FlatOrderBook::apply` and its best-price rescan path). Until then, no
per-function percentage is claimed from this trace. This is a tooling
limitation, not a measurement gap of the code.

## Cross-reference

- Host / tool availability: `host.txt`
- Exact effective command: `command.txt`
- Raw recording stdout note: `bench_stdout.txt`
- Workflow / methodology: `docs/profiling/MACOS_INSTRUMENTS.md`
