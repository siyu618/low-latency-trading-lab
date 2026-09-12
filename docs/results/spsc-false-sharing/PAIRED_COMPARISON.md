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
| 8 | 1024 | 1 | same_line | 47.718192 | 15.642708 | 0.3278 |
| 8 | 1024 | 2 | separated | 47.878533 | 11.252279 | 0.2350 |
| 8 | 1024 | 3 | separated | 50.915900 | 13.296042 | 0.2611 |
| 8 | 1024 | 4 | same_line | 56.068742 | 11.979058 | 0.2136 |
| 8 | 4096 | 1 | same_line | 38.649483 | 10.974029 | 0.2839 |
| 8 | 4096 | 2 | separated | 39.303017 | 12.150071 | 0.3091 |
| 8 | 4096 | 3 | separated | 40.725942 | 14.180917 | 0.3482 |
| 8 | 4096 | 4 | same_line | 37.040363 | 14.565454 | 0.3932 |
| 8 | 65536 | 1 | same_line | 32.720858 | 13.792004 | 0.4215 |
| 8 | 65536 | 2 | separated | 33.766537 | 12.614883 | 0.3736 |
| 8 | 65536 | 3 | separated | 38.413275 | 12.738358 | 0.3316 |
| 8 | 65536 | 4 | same_line | 36.105700 | 11.043058 | 0.3059 |
| 32 | 1024 | 1 | same_line | 31.585763 | 28.336392 | 0.8971 |
| 32 | 1024 | 2 | separated | 27.297904 | 29.259421 | 1.0719 |
| 32 | 1024 | 3 | separated | 29.653625 | 23.759817 | 0.8012 |
| 32 | 1024 | 4 | same_line | 38.399400 | 31.849642 | 0.8294 |
| 32 | 4096 | 1 | same_line | 24.619467 | 37.934092 | 1.5408 |
| 32 | 4096 | 2 | separated | 31.901796 | 37.011362 | 1.1602 |
| 32 | 4096 | 3 | separated | 24.392108 | 35.216300 | 1.4438 |
| 32 | 4096 | 4 | same_line | 23.823471 | 33.535979 | 1.4077 |
| 32 | 65536 | 1 | same_line | 22.250092 | 20.897046 | 0.9392 |
| 32 | 65536 | 2 | separated | 19.965200 | 41.638787 | 2.0856 |
| 32 | 65536 | 3 | separated | 22.777525 | 32.057642 | 1.4074 |
| 32 | 65536 | 4 | same_line | 22.400958 | 18.576062 | 0.8293 |
| 64 | 1024 | 1 | same_line | 36.609446 | 32.155896 | 0.8783 |
| 64 | 1024 | 2 | separated | 33.397129 | 33.902296 | 1.0151 |
| 64 | 1024 | 3 | separated | 39.545533 | 32.781996 | 0.8290 |
| 64 | 1024 | 4 | same_line | 33.975967 | 32.344212 | 0.9520 |
| 64 | 4096 | 1 | same_line | 23.543417 | 13.834758 | 0.5876 |
| 64 | 4096 | 2 | separated | 20.517192 | 18.325933 | 0.8932 |
| 64 | 4096 | 3 | separated | 26.350129 | 14.148429 | 0.5369 |
| 64 | 4096 | 4 | same_line | 24.516967 | 15.727467 | 0.6415 |
| 64 | 65536 | 1 | same_line | 26.276525 | 14.708375 | 0.5598 |
| 64 | 65536 | 2 | separated | 26.059942 | 14.553754 | 0.5585 |
| 64 | 65536 | 3 | separated | 28.827246 | 17.789783 | 0.6171 |
| 64 | 65536 | 4 | same_line | 24.042733 | 15.500388 | 0.6447 |

## Per-cell summary across the 4 session ratios

MEDIAN / MIN / MAX are over the 4 paired ratios. SIGN counts how many
sessions put separated ahead (< 1) and how many put same-line ahead (> 1).
A cell whose 4 ratios do not all point the same way is **not
directionally stable**, whatever its median ratio says. This is a
descriptive criterion, not a significance test.

| bytes | capacity | median ratio | min | max | separated faster | same_line faster | direction |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 0.248077 | 0.213649 | 0.327814 | 4 | 0 | stable — separated 4/4 |
| 8 | 4096 | 0.328671 | 0.283937 | 0.393232 | 4 | 0 | stable — separated 4/4 |
| 8 | 65536 | 0.352602 | 0.305854 | 0.421505 | 4 | 0 | stable — separated 4/4 |
| 32 | 1024 | 0.863278 | 0.801245 | 1.071856 | 3 | 1 | SPLIT 3-1 |
| 32 | 4096 | 1.425723 | 1.160165 | 1.540817 | 0 | 4 | stable — same_line 4/4 |
| 32 | 65536 | 1.173307 | 0.829253 | 2.085568 | 2 | 2 | SPLIT 2-2 |
| 64 | 1024 | 0.915161 | 0.828968 | 1.015126 | 3 | 1 | SPLIT 3-1 |
| 64 | 4096 | 0.614560 | 0.536940 | 0.893199 | 4 | 0 | stable — separated 4/4 |
| 64 | 65536 | 0.588435 | 0.558472 | 0.644702 | 4 | 0 | stable — separated 4/4 |

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
- Separating the cursor lines removes the colocated line-granularity
  interference component between the two independent cursor writes. It does
  NOT remove the required remote observations: the producer still reads
  `tail` at the reuse gate and the consumer still reads `head` at the
  availability gate, in both variants and with the same memory orders.
  Separation also changes whether those two legitimately shared cursor values
  occupy one coherence line or two, so a same-line vs separated difference is
  the NET effect of controlled cursor placement, not pure false-sharing cost.
  See `docs/SPSC_FALSE_SHARING.md`.
- The frozen natural Phase-1/2 SPSC is **not** a control in this
  comparison. It is unpadded, but adjacency is not proof of same-line
  placement and Phase 2 recorded no cursor addresses.
