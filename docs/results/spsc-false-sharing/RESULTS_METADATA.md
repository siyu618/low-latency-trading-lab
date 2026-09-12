# Experiment 02 Phase 3A — results metadata

CONTROLLED cursor-placement (coherence-layout) experiment. ONE variable:
the cache-line placement of the two SPSC cursors, whose policies have the
SAME footprint so the payload array starts at the same object offset in
both variants.

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
| cursor policy footprint | 256 bytes each, identical across the two layouts |
| payload offset | identical across the two layouts, every cell (`invariants.txt`) |
| runtime layout verification | every measured repetition, every process |
| cross-variant footprint gate | before timing, in every control process |
| observational natural rows | 0 (1 = present, excluded from `summary.csv`) |
| UTC | 2026-09-12T03:13:49Z |

The SPSC variants are `SpscSameLineRingBuffer` and
`SpscSeparatedCursorRingBuffer` in
`include/spsc_cursor_layout_ring_buffer.h`. The cache-line query and the
guards live in `include/cache_line.h`. The frozen Phase-1
`include/spsc_ring_buffer.h` was NOT modified and is NOT one of the two
controls.

**Provenance:** `PROVENANCE.md` records the git revision, the
`git status --porcelain` output, the exact build flags and the SHA-256 of
the benchmark executable and of the four key Phase-3A source files. This
directory's data was produced by those bytes, whether or not they were
committed at the time.

Analysis, methodology and limitations: `docs/SPSC_FALSE_SHARING.md`.
