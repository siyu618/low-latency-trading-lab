# Experiment 02 Phase 3A — paired-session comparison

## What is being compared

The **only** difference between the two implementations compared here is
the cache-line placement of the two SPSC cursors. Both come from one
algorithm body parameterised by a cursor layout policy, so the algorithm,
the payload storage and indexing, the full/empty semantics, the memory
orders, the retry policy and the message types are identical by
construction. There is no cached remote cursor, no batching, no CAS and no
affinity in either variant.

`ratio` = **separated median / same_line median** within a session.
**`ratio < 1` means separated cursors completed a message faster** in that
session.

## Why paired, and not the pooled table

Under the balanced AB/BA design, the same-line process and the separated
process for a given `(message_bytes, capacity)` run as **adjacent
processes**, with implementation order swapped between sessions.
**Adjacent execution reduces temporal drift between the two legs but
cannot guarantee identical scheduler, DVFS, thermal, or background-system
state** — the two processes are still separated by a full benchmark run,
and each leg can be placed, migrated or frequency-scaled independently.
The pairing narrows the gap; it does not close it.

Pooling the raw repetitions instead would treat the 5 repetitions
inside one process as independent placements. They are not: a repetition
does create a fresh producer/consumer thread pair and a fresh queue object,
but it shares its process's address space, allocator state and thermal
history with its siblings. The pooled view is kept in `MATRIX.md` as a
**secondary, descriptive** metric.

## Per-session detail

`first` is the layout that ran first in that session's pair.

| bytes | capacity | session | first | same_line ns/msg | separated ns/msg | ratio |
|---|---|---|---|---|---|---|
| 8 | 1024 | 1 | same_line | 51.408700 | 12.958017 | 0.2521 |
| 8 | 1024 | 2 | separated | 52.114592 | 12.867850 | 0.2469 |
| 8 | 1024 | 3 | separated | 52.981225 | 14.279596 | 0.2695 |
| 8 | 1024 | 4 | same_line | 50.009258 | 12.236521 | 0.2447 |
| 8 | 4096 | 1 | same_line | 38.948975 | 15.138225 | 0.3887 |
| 8 | 4096 | 2 | separated | 44.112788 | 14.844158 | 0.3365 |
| 8 | 4096 | 3 | separated | 42.890967 | 13.045517 | 0.3042 |
| 8 | 4096 | 4 | same_line | 42.563692 | 12.733196 | 0.2992 |
| 8 | 65536 | 1 | same_line | 35.822396 | 11.310571 | 0.3157 |
| 8 | 65536 | 2 | separated | 37.276171 | 13.647475 | 0.3661 |
| 8 | 65536 | 3 | separated | 38.983063 | 12.478238 | 0.3201 |
| 8 | 65536 | 4 | same_line | 37.955050 | 12.074350 | 0.3181 |
| 32 | 1024 | 1 | same_line | 28.702783 | 29.296425 | 1.0207 |
| 32 | 1024 | 2 | separated | 30.249500 | 25.224967 | 0.8339 |
| 32 | 1024 | 3 | separated | 33.158400 | 38.428925 | 1.1589 |
| 32 | 1024 | 4 | same_line | 32.880229 | 30.459646 | 0.9264 |
| 32 | 4096 | 1 | same_line | 26.512129 | 42.579571 | 1.6060 |
| 32 | 4096 | 2 | separated | 25.820704 | 41.861917 | 1.6213 |
| 32 | 4096 | 3 | separated | 25.555508 | 40.242162 | 1.5747 |
| 32 | 4096 | 4 | same_line | 24.518433 | 41.661912 | 1.6992 |
| 32 | 65536 | 1 | same_line | 21.530825 | 26.708521 | 1.2405 |
| 32 | 65536 | 2 | separated | 21.873375 | 21.060479 | 0.9628 |
| 32 | 65536 | 3 | separated | 22.106987 | 36.651246 | 1.6579 |
| 32 | 65536 | 4 | same_line | 25.881629 | 39.757162 | 1.5361 |
| 64 | 1024 | 1 | same_line | 31.581658 | 39.889833 | 1.2631 |
| 64 | 1024 | 2 | separated | 32.519867 | 38.316267 | 1.1782 |
| 64 | 1024 | 3 | separated | 32.015937 | 36.189037 | 1.1303 |
| 64 | 1024 | 4 | same_line | 33.217767 | 36.044229 | 1.0851 |
| 64 | 4096 | 1 | same_line | 24.517046 | 21.025604 | 0.8576 |
| 64 | 4096 | 2 | separated | 25.162633 | 21.036458 | 0.8360 |
| 64 | 4096 | 3 | separated | 25.113083 | 18.552212 | 0.7387 |
| 64 | 4096 | 4 | same_line | 23.285696 | 17.602504 | 0.7559 |
| 64 | 65536 | 1 | same_line | 24.247929 | 15.898942 | 0.6557 |
| 64 | 65536 | 2 | separated | 21.698667 | 16.180221 | 0.7457 |
| 64 | 65536 | 3 | separated | 22.918837 | 19.106096 | 0.8336 |
| 64 | 65536 | 4 | same_line | 24.763379 | 19.689067 | 0.7951 |

## Per-cell summary across the 4 session ratios

MEDIAN / MIN / MAX are over the 4 paired ratios. SIGN counts how many
sessions put separated ahead (< 1) and how many put same-line ahead (> 1).
A cell whose 4 ratios do not all point the same way is **not
directionally stable**, whatever its median ratio says. This is a
descriptive criterion, not a significance test.

| bytes | capacity | median ratio | min | max | separated faster | same_line faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 0.249487 | 0.244685 | 0.269522 | 4 | 0 | stable — separated 4/4 |
| 8 | 4096 | 0.320330 | 0.299156 | 0.388668 | 4 | 0 | stable — separated 4/4 |
| 8 | 65536 | 0.319108 | 0.315740 | 0.366118 | 4 | 0 | stable — separated 4/4 |
| 32 | 1024 | 0.973532 | 0.833897 | 1.158950 | 2 | 2 | SPLIT 2-2 |
| 32 | 4096 | 1.613647 | 1.574696 | 1.699208 | 0 | 4 | stable — same_line 4/4 |
| 32 | 65536 | 1.388297 | 0.962836 | 1.657903 | 1 | 3 | SPLIT 1-3 |
| 64 | 1024 | 1.154293 | 1.085089 | 1.263070 | 0 | 4 | stable — same_line 4/4 |
| 64 | 4096 | 0.795978 | 0.738747 | 0.857591 | 4 | 0 | stable — separated 4/4 |
| 64 | 65536 | 0.770383 | 0.655682 | 0.833642 | 4 | 0 | stable — separated 4/4 |

## Reading these numbers

- **MEDIAN RATIO** is the headline paired figure: < 1 means separated was
  faster, > 1 means same-line was faster.
- **MIN / MAX** show whether that median is representative or is averaging
  over sessions that disagreed. They are the extremes of 4
  correlated observations, not a confidence interval.
- **DIRECTION STABILITY** is the attribution check. `stable` means all
  4 sessions agreed; `SPLIT` means they did not, and **no
  directional claim should be made for that cell** regardless of its median.
- These are **descriptive** statistics over 4 paired observations per
  cell. They are not a hypothesis test and no p-value is implied.

## Limitations

- 4 paired observations per cell is a small sample; `stable` means
  "4 out of 4 agreed here", not "the effect is proven".
- No CPU pinning or affinity is used or claimed; macOS may migrate threads
  mid-run and may place the two processes' threads on different core types.
- The two implementations are not the same shape of code: the layout policy
  changes cursor placement only, but the same-line process's threads both
  touch the same line for their own cursor accesses, which is exactly the
  effect under study.
- Padding removes FALSE sharing only. The producer must still observe the
  consumer's tail cursor and vice versa; those remote observations are
  required for correctness and remain in both variants. See
  `docs/SPSC_FALSE_SHARING.md`.
- The frozen natural Phase-1/2 SPSC is **not** a control in this
  comparison. It is unpadded, but adjacency is not proof of same-line
  placement and Phase 2 recorded no cursor addresses.
