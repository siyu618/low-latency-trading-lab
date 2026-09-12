# Experiment 02 Phase 3B — per-session process medians

One row per measured process. Every value is that process's own
`median_ns_per_message`, already re-verified against its raw CSV.
Warm-up repetitions are excluded by the benchmark itself.

| session | traversal | first variant | variant | bytes | capacity | median ns/msg | measured reps |
|---|---|---|---|---|---|---|---|
| 1 | forward | baseline | baseline | 8 | 1024 | 46.130529 | 5 |
| 1 | forward | baseline | cached | 8 | 1024 | 48.974521 | 5 |
| 1 | forward | baseline | baseline | 8 | 4096 | 23.473658 | 5 |
| 1 | forward | baseline | cached | 8 | 4096 | 54.584617 | 5 |
| 1 | forward | baseline | baseline | 8 | 65536 | 59.871571 | 5 |
| 1 | forward | baseline | cached | 8 | 65536 | 64.659671 | 5 |
| 1 | forward | baseline | baseline | 32 | 1024 | 54.782050 | 5 |
| 1 | forward | baseline | cached | 32 | 1024 | 49.424254 | 5 |
| 1 | forward | baseline | baseline | 32 | 4096 | 61.296771 | 5 |
| 1 | forward | baseline | cached | 32 | 4096 | 60.547737 | 5 |
| 1 | forward | baseline | baseline | 32 | 65536 | 68.979671 | 5 |
| 1 | forward | baseline | cached | 32 | 65536 | 76.261271 | 5 |
| 1 | forward | baseline | baseline | 64 | 1024 | 54.940583 | 5 |
| 1 | forward | baseline | cached | 64 | 1024 | 67.299892 | 5 |
| 1 | forward | baseline | baseline | 64 | 4096 | 48.358183 | 5 |
| 1 | forward | baseline | cached | 64 | 4096 | 65.747471 | 5 |
| 1 | forward | baseline | baseline | 64 | 65536 | 44.473450 | 5 |
| 1 | forward | baseline | cached | 64 | 65536 | 68.781675 | 5 |
| 2 | reverse | cached | baseline | 8 | 1024 | 43.942800 | 5 |
| 2 | reverse | cached | cached | 8 | 1024 | 48.898000 | 5 |
| 2 | reverse | cached | baseline | 8 | 4096 | 26.291342 | 5 |
| 2 | reverse | cached | cached | 8 | 4096 | 54.894962 | 5 |
| 2 | reverse | cached | baseline | 8 | 65536 | 55.894283 | 5 |
| 2 | reverse | cached | cached | 8 | 65536 | 66.368217 | 5 |
| 2 | reverse | cached | baseline | 32 | 1024 | 42.338229 | 5 |
| 2 | reverse | cached | cached | 32 | 1024 | 48.021963 | 5 |
| 2 | reverse | cached | baseline | 32 | 4096 | 55.813012 | 5 |
| 2 | reverse | cached | cached | 32 | 4096 | 62.317446 | 5 |
| 2 | reverse | cached | baseline | 32 | 65536 | 72.106133 | 5 |
| 2 | reverse | cached | cached | 32 | 65536 | 78.167142 | 5 |
| 2 | reverse | cached | baseline | 64 | 1024 | 54.127321 | 5 |
| 2 | reverse | cached | cached | 64 | 1024 | 68.658725 | 5 |
| 2 | reverse | cached | baseline | 64 | 4096 | 48.564712 | 5 |
| 2 | reverse | cached | cached | 64 | 4096 | 69.215342 | 5 |
| 2 | reverse | cached | baseline | 64 | 65536 | 41.461537 | 5 |
| 2 | reverse | cached | cached | 64 | 65536 | 67.064846 | 5 |
| 3 | forward | cached | baseline | 8 | 1024 | 39.867337 | 5 |
| 3 | forward | cached | cached | 8 | 1024 | 51.329000 | 5 |
| 3 | forward | cached | baseline | 8 | 4096 | 29.585225 | 5 |
| 3 | forward | cached | cached | 8 | 4096 | 56.848758 | 5 |
| 3 | forward | cached | baseline | 8 | 65536 | 59.651129 | 5 |
| 3 | forward | cached | cached | 8 | 65536 | 61.184633 | 5 |
| 3 | forward | cached | baseline | 32 | 1024 | 44.713604 | 5 |
| 3 | forward | cached | cached | 32 | 1024 | 50.487092 | 5 |
| 3 | forward | cached | baseline | 32 | 4096 | 56.814125 | 5 |
| 3 | forward | cached | cached | 32 | 4096 | 60.558437 | 5 |
| 3 | forward | cached | baseline | 32 | 65536 | 72.174425 | 5 |
| 3 | forward | cached | cached | 32 | 65536 | 70.274171 | 5 |
| 3 | forward | cached | baseline | 64 | 1024 | 52.514763 | 5 |
| 3 | forward | cached | cached | 64 | 1024 | 68.094567 | 5 |
| 3 | forward | cached | baseline | 64 | 4096 | 50.200483 | 5 |
| 3 | forward | cached | cached | 64 | 4096 | 69.634304 | 5 |
| 3 | forward | cached | baseline | 64 | 65536 | 43.991125 | 5 |
| 3 | forward | cached | cached | 64 | 65536 | 69.968775 | 5 |
| 4 | reverse | baseline | baseline | 8 | 1024 | 40.749938 | 5 |
| 4 | reverse | baseline | cached | 8 | 1024 | 43.247279 | 5 |
| 4 | reverse | baseline | baseline | 8 | 4096 | 47.945746 | 5 |
| 4 | reverse | baseline | cached | 8 | 4096 | 50.125267 | 5 |
| 4 | reverse | baseline | baseline | 8 | 65536 | 59.210375 | 5 |
| 4 | reverse | baseline | cached | 8 | 65536 | 66.676992 | 5 |
| 4 | reverse | baseline | baseline | 32 | 1024 | 45.747996 | 5 |
| 4 | reverse | baseline | cached | 32 | 1024 | 49.614096 | 5 |
| 4 | reverse | baseline | baseline | 32 | 4096 | 60.054096 | 5 |
| 4 | reverse | baseline | cached | 32 | 4096 | 62.978887 | 5 |
| 4 | reverse | baseline | baseline | 32 | 65536 | 71.766425 | 5 |
| 4 | reverse | baseline | cached | 32 | 65536 | 70.451942 | 5 |
| 4 | reverse | baseline | baseline | 64 | 1024 | 53.073696 | 5 |
| 4 | reverse | baseline | cached | 64 | 1024 | 67.498967 | 5 |
| 4 | reverse | baseline | baseline | 64 | 4096 | 47.602913 | 5 |
| 4 | reverse | baseline | cached | 64 | 4096 | 68.974179 | 5 |
| 4 | reverse | baseline | baseline | 64 | 65536 | 43.876371 | 5 |
| 4 | reverse | baseline | cached | 64 | 65536 | 66.619788 | 5 |
