# Experiment 02 Phase 2 — paired-session comparison

## Why this, and not the pooled table

Under the balanced AB/BA design, the mutex process and the SPSC process for
a given `(message_bytes, capacity)` run as **adjacent processes**, with
implementation order swapped between sessions. Both sides of a pair
therefore ran at nearly the same point in the run, on the same machine
state. The ratio of their per-session medians is a **paired observation**.

Pooling the raw repetitions instead would treat the 5 repetitions
inside one process as independent placements. They are not: a repetition
does create a fresh producer/consumer thread pair, but it shares its
process's address space, allocator state and thermal history with its
siblings. The pooled view is kept in `MATRIX.md` as a **secondary,
descriptive** metric.

## Per-session detail

`ratio` = SPSC median / mutex median within that session; **< 1 means SPSC
completed a message faster** in that session. `first` is the implementation
that ran first in that session's pair.

| bytes | capacity | session | first | mutex ns/msg | spsc ns/msg | ratio |
|---|---|---|---|---|---|---|
| 8 | 1024 | 1 | mutex | 52.901396 | 53.334288 | 1.0082 |
| 8 | 1024 | 2 | spsc | 46.045887 | 32.658508 | 0.7093 |
| 8 | 1024 | 3 | spsc | 51.033996 | 37.474746 | 0.7343 |
| 8 | 1024 | 4 | mutex | 47.681471 | 25.706225 | 0.5391 |
| 8 | 4096 | 1 | mutex | 24.220312 | 18.803863 | 0.7764 |
| 8 | 4096 | 2 | spsc | 22.315604 | 17.568854 | 0.7873 |
| 8 | 4096 | 3 | spsc | 22.601950 | 22.308971 | 0.9870 |
| 8 | 4096 | 4 | mutex | 23.212133 | 25.430604 | 1.0956 |
| 8 | 65536 | 1 | mutex | 18.752450 | 46.765629 | 2.4938 |
| 8 | 65536 | 2 | spsc | 18.651013 | 41.987929 | 2.2512 |
| 8 | 65536 | 3 | spsc | 19.695667 | 32.182779 | 1.6340 |
| 8 | 65536 | 4 | mutex | 19.308267 | 32.603200 | 1.6886 |
| 32 | 1024 | 1 | mutex | 34.714233 | 36.512813 | 1.0518 |
| 32 | 1024 | 2 | spsc | 36.510083 | 30.409246 | 0.8329 |
| 32 | 1024 | 3 | spsc | 33.905412 | 36.833888 | 1.0864 |
| 32 | 1024 | 4 | mutex | 32.729642 | 28.284137 | 0.8642 |
| 32 | 4096 | 1 | mutex | 23.208533 | 38.657083 | 1.6656 |
| 32 | 4096 | 2 | spsc | 22.536371 | 30.794013 | 1.3664 |
| 32 | 4096 | 3 | spsc | 23.008662 | 31.839842 | 1.3838 |
| 32 | 4096 | 4 | mutex | 23.035908 | 29.590233 | 1.2845 |
| 32 | 65536 | 1 | mutex | 21.154521 | 45.690729 | 2.1599 |
| 32 | 65536 | 2 | spsc | 21.184287 | 46.311138 | 2.1861 |
| 32 | 65536 | 3 | spsc | 21.245821 | 50.303925 | 2.3677 |
| 32 | 65536 | 4 | mutex | 21.123167 | 47.873488 | 2.2664 |
| 64 | 1024 | 1 | mutex | 45.965779 | 39.305588 | 0.8551 |
| 64 | 1024 | 2 | spsc | 46.005479 | 37.063596 | 0.8056 |
| 64 | 1024 | 3 | spsc | 42.710242 | 41.044083 | 0.9610 |
| 64 | 1024 | 4 | mutex | 42.235937 | 37.996883 | 0.8996 |
| 64 | 4096 | 1 | mutex | 28.987629 | 13.210817 | 0.4557 |
| 64 | 4096 | 2 | spsc | 27.474596 | 15.741288 | 0.5729 |
| 64 | 4096 | 3 | spsc | 27.897471 | 15.614413 | 0.5597 |
| 64 | 4096 | 4 | mutex | 27.982283 | 14.718087 | 0.5260 |
| 64 | 65536 | 1 | mutex | 23.443221 | 14.567746 | 0.6214 |
| 64 | 65536 | 2 | spsc | 22.191250 | 14.992900 | 0.6756 |
| 64 | 65536 | 3 | spsc | 23.365517 | 14.068888 | 0.6021 |
| 64 | 65536 | 4 | mutex | 23.910758 | 17.016492 | 0.7117 |

## Per-cell summary across the 4 session ratios

MEDIAN / MIN / MAX are over the 4 paired ratios. SIGN counts how many
sessions put SPSC ahead (< 1) and how many put the mutex baseline ahead
(> 1). A cell whose 4 ratios do not all point the same way is
**not directionally stable**, whatever its median ratio says. This is a
descriptive criterion, not a significance test.

| bytes | capacity | median ratio | min | max | SPSC faster | mutex faster | sign consistency |
|---|---|---|---|---|---|---|---|
| 8 | 1024 | 0.721785 | 0.539124 | 1.008183 | 3 | 1 | SPLIT 3-1 |
| 8 | 4096 | 0.887163 | 0.776367 | 1.095574 | 3 | 1 | SPLIT 3-1 |
| 8 | 65536 | 1.969901 | 1.634003 | 2.493841 | 0 | 4 | stable (mutex 4/4) |
| 32 | 1024 | 0.957993 | 0.832900 | 1.086372 | 2 | 2 | SPLIT 2-2 |
| 32 | 4096 | 1.375117 | 1.284526 | 1.665641 | 0 | 4 | stable (mutex 4/4) |
| 32 | 65536 | 2.226253 | 2.159856 | 2.367709 | 0 | 4 | stable (mutex 4/4) |
| 64 | 1024 | 0.877370 | 0.805634 | 0.960989 | 4 | 0 | stable (SPSC 4/4) |
| 64 | 4096 | 0.542843 | 0.455740 | 0.572940 | 4 | 0 | stable (SPSC 4/4) |
| 64 | 65536 | 0.648513 | 0.602122 | 0.711667 | 4 | 0 | stable (SPSC 4/4) |

## Reading these numbers

- **MEDIAN RATIO** is the headline paired figure: < 1 favours SPSC, > 1
  favours the mutex baseline.
- **MIN / MAX** show whether that median is representative or is averaging
  over sessions that disagreed.
- **SIGN CONSISTENCY** is the stability test. `stable` means all 4
  sessions agreed on the direction; `SPLIT` means they did not, and no
  directional claim should be made for that cell regardless of the median.
- These are **descriptive** statistics over 4 paired observations
  per cell. They are not a hypothesis test and no p-value is implied.

## Limitations

- 4 paired observations per cell is a small sample; `stable` means
  "4 out of 4 agreed here", not "the effect is proven".
- Adjacency narrows but does not eliminate drift: the two processes are
  still separated by one full benchmark run (tens of seconds).
- No CPU pinning or affinity is used or claimed; macOS may migrate threads
  mid-run and may place the two processes' threads on different core types.
- Phase 2 does not attribute any difference to cache-line interference.
  False sharing in the unpadded Phase-1 layout remains a Phase-3 hypothesis.
