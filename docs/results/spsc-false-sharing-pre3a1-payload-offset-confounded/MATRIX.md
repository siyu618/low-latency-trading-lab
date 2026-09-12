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
- UTC: 2026-09-12T02:13:35Z

| impl | bytes | capacity | pooled reps | median ns/msg | min | max | spread | median msg/s | full retries | empty retries |
|---|---|---|---|---|---|---|---|---|---|---|
| same_line | 8 | 1024 | 20 | 51.976257 | 43.981329 | 57.621358 | 31.013% | 19239554.122 | 444728001 | 49391891 |
| separated | 8 | 1024 | 20 | 13.215408 | 11.714742 | 16.749425 | 42.977% | 75669246.244 | 10419877 | 186820168 |
| same_line | 8 | 4096 | 20 | 42.499232 | 33.756517 | 46.642350 | 38.173% | 23529837.240 | 282665553 | 13587488 |
| separated | 8 | 4096 | 20 | 13.850998 | 10.229467 | 15.759913 | 54.064% | 72196966.320 | 2115564 | 196534802 |
| same_line | 8 | 65536 | 20 | 38.262917 | 34.877842 | 43.113850 | 23.614% | 26134965.431 | 290586862 | 2285002 |
| separated | 8 | 65536 | 20 | 12.141273 | 10.324658 | 15.130487 | 46.547% | 82363686.246 | 1587941 | 131395738 |
| same_line | 32 | 1024 | 20 | 30.734319 | 25.926467 | 34.908371 | 34.644% | 32536917.973 | 35388133 | 392329599 |
| separated | 32 | 1024 | 20 | 30.146629 | 23.016504 | 43.159171 | 87.514% | 33171204.098 | 12061407 | 559578459 |
| same_line | 32 | 4096 | 20 | 25.910600 | 22.806183 | 27.739479 | 21.631% | 38594243.283 | 73933681 | 90753814 |
| separated | 32 | 4096 | 20 | 41.681641 | 36.491788 | 46.001663 | 26.060% | 23991377.595 | 12868733 | 1352007444 |
| same_line | 32 | 65536 | 20 | 22.206206 | 17.520967 | 26.722804 | 52.519% | 45032456.242 | 22531083 | 10005210 |
| separated | 32 | 65536 | 20 | 33.983616 | 13.979183 | 47.939596 | 242.936% | 29425944.116 | 1453535 | 1383821654 |
| same_line | 64 | 1024 | 20 | 32.177127 | 28.796079 | 34.894096 | 21.177% | 31077976.601 | 50458277 | 276974508 |
| separated | 64 | 1024 | 20 | 37.147787 | 31.490621 | 41.873746 | 32.972% | 26919503.618 | 12970906 | 744749030 |
| same_line | 64 | 4096 | 20 | 24.704486 | 18.667721 | 28.967029 | 55.172% | 40478479.100 | 34682644 | 19445316 |
| separated | 64 | 4096 | 20 | 19.526692 | 16.076062 | 24.267758 | 50.956% | 51211951.313 | 18105302 | 209123953 |
| same_line | 64 | 65536 | 20 | 23.407010 | 20.110025 | 25.935508 | 28.968% | 42722243.407 | 17387461 | 2049731 |
| separated | 64 | 65536 | 20 | 18.781311 | 13.551950 | 22.995467 | 69.684% | 53244420.830 | 9510398 | 88005382 |
