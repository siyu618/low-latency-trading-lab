# Experiment 02 Phase 3A — results metadata

CONTROLLED false-sharing experiment. ONE variable: the cache-line
placement of the two SPSC cursors.

| item | value |
|---|---|
| matrix | 2 cursor layouts (`same_line`, `separated`) x 3 message sizes (8, 32, 64) x 3 capacities (1024, 4096, 65536) = 18 cells |
| message count | 10000000 per repetition |
| measured repetitions | 5 per process |
| warm-up | 1 per process, excluded from all reported and derived data |
| sessions | 4, balanced AB/BA |
| processes | 72 = 9 cells x 2 layouts x 4 sessions |
| implementations per process | exactly 1 |
| host-reported cache-line size | 128 bytes |
| compile-time layout assumption | 128 bytes |
| runtime layout verification | every measured repetition, every process |
| observational natural rows | 0 (1 = present, excluded from `summary.csv`) |
| UTC | 2026-09-12T02:13:37Z |

The SPSC variants are `SpscSameLineRingBuffer` and
`SpscSeparatedCursorRingBuffer` in
`include/spsc_cursor_layout_ring_buffer.h`. The cache-line query and the
guards live in `include/cache_line.h`. The frozen Phase-1
`include/spsc_ring_buffer.h` was NOT modified and is NOT one of the two
controls.

Analysis, methodology and limitations: `docs/SPSC_FALSE_SHARING.md`.
