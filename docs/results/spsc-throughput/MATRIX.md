# Experiment 02 Phase 2 — canonical throughput matrix

DERIVED from the raw per-repetition CSVs in this directory (`raw/`). All
measured repetitions of all sessions for a cell are POOLED; nothing is
dropped and no session is preferred. `ns/msg` is END-TO-END elapsed /
messages delivered — not a per-call latency and not a one-way handoff time.

**This pooled table is secondary and descriptive.** Pooling treats the
`5` repetitions inside one process as if they were independent
placements, which they are not — a repetition constructs a fresh thread
pair, but shares its process's address space, allocator state and thermal
history with its siblings. For implementation direction, read
`PAIRED_COMPARISON.md`, which compares each mutex/SPSC process pair inside
the session where they ran adjacently, under a balanced AB/BA order.

- message_count: 10000000 per repetition
- measured repetitions: 5 per process x 4 processes per cell
- warm-up repetitions (excluded from all reported data): 1 per process
- UTC: 2026-09-11T08:05:08Z

| impl | bytes | capacity | pooled reps | median ns/msg | min | max | spread | median msg/s | full retries | empty retries |
|---|---|---|---|---|---|---|---|---|---|---|
| mutex | 8 | 1024 | 20 | 49.147048 | 41.818117 | 57.093554 | 36.528% | 20347102.027 | 625701820 | 67771258 |
| spsc | 8 | 1024 | 20 | 36.089410 | 19.239525 | 34706.397113 | 180291.133% | 27708959.114 | 90716956 | 962332897 |
| mutex | 8 | 4096 | 20 | 23.203554 | 21.669479 | 26.471154 | 22.159% | 43096846.285 | 56747827 | 35290716 |
| spsc | 8 | 4096 | 20 | 20.835223 | 13.357483 | 31.852404 | 138.461% | 47995646.603 | 47384231 | 539880511 |
| mutex | 8 | 65536 | 20 | 19.216639 | 17.417792 | 20.580783 | 18.160% | 52038234.885 | 17225271 | 13517561 |
| spsc | 8 | 65536 | 20 | 41.636314 | 28.878817 | 51.380275 | 77.917% | 24017495.593 | 57828473 | 1310425152 |
| mutex | 32 | 1024 | 20 | 34.270564 | 32.029938 | 38.815779 | 21.186% | 29179560.202 | 178249912 | 93987276 |
| spsc | 32 | 1024 | 20 | 32.248656 | 25.254558 | 40.617275 | 60.831% | 31009043.741 | 7396872 | 944478449 |
| mutex | 32 | 4096 | 20 | 22.962935 | 21.843113 | 23.678675 | 8.403% | 43548440.128 | 16123316 | 40082546 |
| spsc | 32 | 4096 | 20 | 31.707723 | 26.161342 | 39.122258 | 49.542% | 31538057.779 | 1754345 | 955240851 |
| mutex | 32 | 65536 | 20 | 21.169404 | 20.553321 | 22.157863 | 7.807% | 47237985.538 | 524970 | 32467774 |
| spsc | 32 | 65536 | 20 | 47.766427 | 43.369496 | 59.732846 | 37.730% | 20935206.228 | 454357 | 2059422907 |
| mutex | 64 | 1024 | 20 | 45.924261 | 38.630533 | 48.859979 | 26.480% | 21774983.181 | 232301361 | 78927192 |
| spsc | 64 | 1024 | 20 | 38.913777 | 35.263804 | 47.041746 | 33.400% | 25697839.281 | 1065777 | 922527851 |
| mutex | 64 | 4096 | 20 | 27.939877 | 24.237221 | 30.841287 | 27.248% | 35791138.236 | 85692298 | 13490192 |
| spsc | 64 | 4096 | 20 | 14.353310 | 12.673488 | 18.744812 | 47.906% | 69670340.848 | 16724620 | 13549466 |
| mutex | 64 | 65536 | 20 | 23.210036 | 21.596004 | 24.826954 | 14.961% | 43084811.309 | 49287408 | 403858 |
| spsc | 64 | 65536 | 20 | 14.780323 | 12.551783 | 19.555683 | 55.800% | 67657520.069 | 7150269 | 21465026 |
