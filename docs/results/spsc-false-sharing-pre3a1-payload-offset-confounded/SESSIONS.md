# Experiment 02 Phase 3A — per-process/session medians

Each session is one process; each process ran exactly one cursor layout and
created a **fresh producer/consumer thread pair and a fresh queue object for
every repetition**. A session is therefore NOT a fixed thread placement: it
is a grouping of repetitions inside one process/address-space lifetime.
These are **per-process medians**, not fixed-placement medians.

Differences between sessions may reflect scheduler placement, thread
migration, P/E-core selection, DVFS and thermal state, allocator/address
placement, or background system activity. Phase 3A does not identify which
factor caused any particular fast or slow run.

All values are ns per message, END-TO-END. Session order in each row is
session 1..4; see `command.txt` for which layout ran first in each
session.

| impl | bytes | capacity | session 1 | session 2 | session 3 | session 4 | pooled |
|---|---|---|---|------|------|------|------|
| same_line | 8 | 1024 | 51.408700 | 52.114592 | 52.981225 | 50.009258 | 51.976257 |
| separated | 8 | 1024 | 12.958017 | 12.867850 | 14.279596 | 12.236521 | 13.215408 |
| same_line | 8 | 4096 | 38.948975 | 44.112788 | 42.890967 | 42.563692 | 42.499232 |
| separated | 8 | 4096 | 15.138225 | 14.844158 | 13.045517 | 12.733196 | 13.850998 |
| same_line | 8 | 65536 | 35.822396 | 37.276171 | 38.983063 | 37.955050 | 38.262917 |
| separated | 8 | 65536 | 11.310571 | 13.647475 | 12.478238 | 12.074350 | 12.141273 |
| same_line | 32 | 1024 | 28.702783 | 30.249500 | 33.158400 | 32.880229 | 30.734319 |
| separated | 32 | 1024 | 29.296425 | 25.224967 | 38.428925 | 30.459646 | 30.146629 |
| same_line | 32 | 4096 | 26.512129 | 25.820704 | 25.555508 | 24.518433 | 25.910600 |
| separated | 32 | 4096 | 42.579571 | 41.861917 | 40.242162 | 41.661912 | 41.681641 |
| same_line | 32 | 65536 | 21.530825 | 21.873375 | 22.106987 | 25.881629 | 22.206206 |
| separated | 32 | 65536 | 26.708521 | 21.060479 | 36.651246 | 39.757162 | 33.983616 |
| same_line | 64 | 1024 | 31.581658 | 32.519867 | 32.015937 | 33.217767 | 32.177127 |
| separated | 64 | 1024 | 39.889833 | 38.316267 | 36.189037 | 36.044229 | 37.147787 |
| same_line | 64 | 4096 | 24.517046 | 25.162633 | 25.113083 | 23.285696 | 24.704486 |
| separated | 64 | 4096 | 21.025604 | 21.036458 | 18.552212 | 17.602504 | 19.526692 |
| same_line | 64 | 65536 | 24.247929 | 21.698667 | 22.918837 | 24.763379 | 23.407010 |
| separated | 64 | 65536 | 15.898942 | 16.180221 | 19.106096 | 19.689067 | 18.781311 |
