# Experiment 02 Phase 4 — session design

| session | traversal | first cell | last cell | processes |
|---|---|---|---|---|
| 1 | forward | 16 B / 1024 | 64 B / 65536 | 9 |
| 2 | reverse | 64 B / 65536 | 16 B / 1024 | 9 |
| 3 | forward | 16 B / 1024 | 64 B / 65536 | 9 |
| 4 | reverse | 64 B / 65536 | 16 B / 1024 | 9 |

Every cell appears once per session, so each is measured **4 times** as a
process: twice early in a session and twice late, in two forward and two
reverse passes.

**What the ordering buys:** it prevents a cell's temporal position inside a
session from being confounded with the cell's identity. Every cell is
measured both first and last across the four sessions.

**What it does NOT buy:** it does not eliminate scheduler variation, core
migration, DVFS or thermal drift, and it does not make the sessions
independent of each other. This is a single-host, non-pinned measurement.
No claim of a controlled thermal or frequency regime is made anywhere in
this dataset.

**AB/BA is not applicable.** There is one queue in this matrix, so there is
no treatment pair to counterbalance. The two traversal directions balance
cell POSITION, and nothing more is claimed for them.
