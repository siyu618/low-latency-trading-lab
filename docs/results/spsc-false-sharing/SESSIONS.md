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
| same_line | 8 | 1024 | 47.718192 | 47.878533 | 50.915900 | 56.068742 | 50.276873 |
| separated | 8 | 1024 | 15.642708 | 11.252279 | 13.296042 | 11.979058 | 12.849425 |
| same_line | 8 | 4096 | 38.649483 | 39.303017 | 40.725942 | 37.040363 | 39.393923 |
| separated | 8 | 4096 | 10.974029 | 12.150071 | 14.180917 | 14.565454 | 12.539331 |
| same_line | 8 | 65536 | 32.720858 | 33.766537 | 38.413275 | 36.105700 | 33.616941 |
| separated | 8 | 65536 | 13.792004 | 12.614883 | 12.738358 | 11.043058 | 12.783183 |
| same_line | 32 | 1024 | 31.585763 | 27.297904 | 29.653625 | 38.399400 | 30.209577 |
| separated | 32 | 1024 | 28.336392 | 29.259421 | 23.759817 | 31.849642 | 29.174946 |
| same_line | 32 | 4096 | 24.619467 | 31.901796 | 24.392108 | 23.823471 | 24.768867 |
| separated | 32 | 4096 | 37.934092 | 37.011362 | 35.216300 | 33.535979 | 35.963912 |
| same_line | 32 | 65536 | 22.250092 | 19.965200 | 22.777525 | 22.400958 | 22.110734 |
| separated | 32 | 65536 | 20.897046 | 41.638787 | 32.057642 | 18.576062 | 24.123679 |
| same_line | 64 | 1024 | 36.609446 | 33.397129 | 39.545533 | 33.975967 | 35.397579 |
| separated | 64 | 1024 | 32.155896 | 33.902296 | 32.781996 | 32.344212 | 32.678875 |
| same_line | 64 | 4096 | 23.543417 | 20.517192 | 26.350129 | 24.516967 | 24.545742 |
| separated | 64 | 4096 | 13.834758 | 18.325933 | 14.148429 | 15.727467 | 15.002006 |
| same_line | 64 | 65536 | 26.276525 | 26.059942 | 28.827246 | 24.042733 | 26.326096 |
| separated | 64 | 65536 | 14.708375 | 14.553754 | 17.789783 | 15.500388 | 15.326596 |
