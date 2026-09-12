# Experiment 02 Phase 3A — canonical cursor-placement matrix

DERIVED from the raw per-repetition CSVs in this directory (`raw/`). All
measured repetitions of all sessions for a cell are POOLED; nothing is
dropped and no session is preferred. `ns/msg` is END-TO-END elapsed /
messages delivered — not a per-call latency and not a one-way handoff time.

**This pooled table is secondary and descriptive.** Pooling treats the
`5` repetitions inside one process as if they were independent
placements, which they are not. For the `separated` vs `same_line`
direction, read `PAIRED_COMPARISON.md`, which compares each pair of
adjacent processes inside the session where they ran, under a balanced
AB/BA order.

- message_count: 10000000 per repetition
- measured repetitions: 5 per process x 4 processes per cell
- warm-up repetitions (excluded from all reported data): 1 per process
- every row's cursor placement was verified at runtime against the host's
  reported cache-line size (128 bytes); see `invariants.txt`
- UTC: 2026-09-12T03:13:48Z

| impl | bytes | capacity | pooled reps | median ns/msg | min | max | spread | median msg/s | full retries | empty retries |
|---|---|---|---|---|---|---|---|---|---|---|
| same_line | 8 | 1024 | 20 | 50.276873 | 42.035467 | 62.011483 | 47.522% | 19889860.692 | 481931840 | 58984438 |
| separated | 8 | 1024 | 20 | 12.849425 | 7.961887 | 19.945754 | 150.515% | 77824494.092 | 11134899 | 185312340 |
| same_line | 8 | 4096 | 20 | 39.393923 | 29.073642 | 48.041229 | 65.240% | 25384625.949 | 268970743 | 11644984 |
| separated | 8 | 4096 | 20 | 12.539331 | 8.560762 | 16.298475 | 90.386% | 79749067.963 | 1746560 | 206752753 |
| same_line | 8 | 65536 | 20 | 33.616941 | 27.075525 | 44.790171 | 65.427% | 29746906.035 | 156344345 | 2028555 |
| separated | 8 | 65536 | 20 | 12.783183 | 8.015063 | 15.112917 | 88.556% | 78227777.855 | 298656 | 226458677 |
| same_line | 32 | 1024 | 20 | 30.209577 | 25.896792 | 42.363179 | 63.585% | 33102085.474 | 33879537 | 527367103 |
| separated | 32 | 1024 | 20 | 29.174946 | 21.218158 | 34.704412 | 63.560% | 34275984.607 | 8039307 | 479367382 |
| same_line | 32 | 4096 | 20 | 24.768867 | 22.378908 | 33.276562 | 48.696% | 40373264.558 | 45799658 | 121009133 |
| separated | 32 | 4096 | 20 | 35.963912 | 30.880562 | 40.305413 | 30.520% | 27805651.012 | 197021 | 1160772794 |
| same_line | 32 | 65536 | 20 | 22.110734 | 16.317788 | 32.885017 | 101.529% | 45226903.033 | 10266712 | 22943523 |
| separated | 32 | 65536 | 20 | 24.123679 | 12.063621 | 50.365183 | 317.496% | 41453047.025 | 1033981 | 1059557524 |
| same_line | 64 | 1024 | 20 | 35.397579 | 25.520338 | 44.614225 | 74.818% | 28250519.619 | 24241169 | 448224874 |
| separated | 64 | 1024 | 20 | 32.678875 | 29.772550 | 38.143083 | 28.115% | 30600808.626 | 4008663 | 527642774 |
| same_line | 64 | 4096 | 20 | 24.545742 | 17.959942 | 43.414792 | 141.731% | 40740263.627 | 26001980 | 8156713 |
| separated | 64 | 4096 | 20 | 15.002006 | 12.081942 | 22.650938 | 87.478% | 66657752.303 | 8895422 | 57945194 |
| same_line | 64 | 65536 | 20 | 26.326096 | 22.248758 | 34.169421 | 53.579% | 37985123.202 | 16875466 | 2149708 |
| separated | 64 | 65536 | 20 | 15.326596 | 12.561296 | 22.205658 | 76.778% | 65246059.856 | 5317326 | 50484985 |
