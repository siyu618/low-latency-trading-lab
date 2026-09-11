# Experiment 02 Phase 2 — per-process/session medians

Each session is one process; each process ran exactly one implementation
and created a **fresh producer/consumer thread pair for every repetition**.
A session is therefore NOT a fixed thread placement: it is a grouping of
repetitions inside one process/address-space lifetime. These are
**per-process medians**, not fixed-placement medians.

Differences between sessions may reflect scheduler placement, thread
migration, P/E-core selection, DVFS and thermal state, allocator/address
placement, or background system activity. Phase 2 does not identify which
factor caused any particular fast or slow run.

All values are ns per message, END-TO-END. Session order in each row is
session 1..4; see `command.txt` for which implementation ran first
in each session.

| impl | bytes | capacity | session 1 | session 2 | session 3 | session 4 | pooled |
|---|---|---|---|------|------|------|------|
| mutex | 8 | 1024 | 52.901396 | 46.045887 | 51.033996 | 47.681471 | 49.147048 |
| spsc | 8 | 1024 | 53.334288 | 32.658508 | 37.474746 | 25.706225 | 36.089410 |
| mutex | 8 | 4096 | 24.220312 | 22.315604 | 22.601950 | 23.212133 | 23.203554 |
| spsc | 8 | 4096 | 18.803863 | 17.568854 | 22.308971 | 25.430604 | 20.835223 |
| mutex | 8 | 65536 | 18.752450 | 18.651013 | 19.695667 | 19.308267 | 19.216639 |
| spsc | 8 | 65536 | 46.765629 | 41.987929 | 32.182779 | 32.603200 | 41.636314 |
| mutex | 32 | 1024 | 34.714233 | 36.510083 | 33.905412 | 32.729642 | 34.270564 |
| spsc | 32 | 1024 | 36.512813 | 30.409246 | 36.833888 | 28.284137 | 32.248656 |
| mutex | 32 | 4096 | 23.208533 | 22.536371 | 23.008662 | 23.035908 | 22.962935 |
| spsc | 32 | 4096 | 38.657083 | 30.794013 | 31.839842 | 29.590233 | 31.707723 |
| mutex | 32 | 65536 | 21.154521 | 21.184287 | 21.245821 | 21.123167 | 21.169404 |
| spsc | 32 | 65536 | 45.690729 | 46.311138 | 50.303925 | 47.873488 | 47.766427 |
| mutex | 64 | 1024 | 45.965779 | 46.005479 | 42.710242 | 42.235937 | 45.924261 |
| spsc | 64 | 1024 | 39.305588 | 37.063596 | 41.044083 | 37.996883 | 38.913777 |
| mutex | 64 | 4096 | 28.987629 | 27.474596 | 27.897471 | 27.982283 | 27.939877 |
| spsc | 64 | 4096 | 13.210817 | 15.741288 | 15.614413 | 14.718087 | 14.353310 |
| mutex | 64 | 65536 | 23.443221 | 22.191250 | 23.365517 | 23.910758 | 23.210036 |
| spsc | 64 | 65536 | 14.567746 | 14.992900 | 14.068888 | 17.016492 | 14.780323 |
