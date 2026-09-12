# Experiment 02 Phase 3B — MEASURED MECHANISM

> **This file is not the canonical result.** The numbers here come from
> `--instrument=1` processes, which are a DIFFERENT instantiation from the
> one the canonical throughput figures come from. The `ns_per_message`
> values below are recorded for completeness and **must not be quoted as the
> Phase-3B throughput result**.

## What is measured

How many times each thread ACTUALLY read the opposite thread's cursor. In
the baseline this equals the `try_push`/`try_pop` call count by
construction. In the cached variant it counts real refresh loads — which is
the mechanism the experiment is about.

The counters are **ordinary non-atomic members owned by one thread each**,
incremented on the hot path and read only AFTER both threads have been
joined. No global atomic was added to the timed hot path to count these.

## What this can and cannot show

- It CAN show that the cached variant performs fewer remote cursor loads,
  and by roughly how much, per cell.
- It CANNOT show why: there are no hardware performance counters here. This
  file does not measure cache misses, coherence transactions, or line
  invalidations, and nothing here may be described in those terms.
- **A reduction in remote loads does not imply a reduction in wall clock.**
  The cached variant can refresh on every failed attempt while the queue is
  genuinely full or empty, because the real remote cursor has not moved and
  so the cached copy cannot improve. Section "Where the mechanism does not
  pay" below is the place to look for exactly that.

## Remote loads per cell

| bytes | capacity | variant | producer remote tail loads | consumer remote head loads | per message (prod) | per message (cons) |
|---|---|---|---|---|---|---|
| 8 | 1024 | baseline | 46353397 | 97021272 | 1.545113233 | 3.234042400 |
| 8 | 1024 | cached | 2662516 | 184507872 | 0.088750533 | 6.150262400 |
| 8 | 4096 | baseline | 30851725 | 90345087 | 1.028390833 | 3.011502900 |
| 8 | 4096 | cached | 538408 | 222776285 | 0.017946933 | 7.425876167 |
| 8 | 65536 | baseline | 31966439 | 82997389 | 1.065547967 | 2.766579633 |
| 8 | 65536 | cached | 456 | 342416970 | 0.000015200 | 11.413899000 |
| 32 | 1024 | baseline | 30240679 | 321408313 | 1.008022633 | 10.713610433 |
| 32 | 1024 | cached | 10226969 | 232939827 | 0.340898967 | 7.764660900 |
| 32 | 4096 | baseline | 30021884 | 364186255 | 1.000729467 | 12.139541833 |
| 32 | 4096 | cached | 237211 | 307306074 | 0.007907033 | 10.243535800 |
| 32 | 65536 | baseline | 30007168 | 444628934 | 1.000238933 | 14.820964467 |
| 32 | 65536 | cached | 456 | 479647739 | 0.000015200 | 15.988257967 |
| 64 | 1024 | baseline | 69563843 | 37395698 | 2.318794767 | 1.246523267 |
| 64 | 1024 | cached | 187241776 | 32351 | 6.241392533 | 0.001078367 |
| 64 | 4096 | baseline | 59911914 | 35709203 | 1.997063800 | 1.190306767 |
| 64 | 4096 | cached | 184641105 | 8319 | 6.154703500 | 0.000277300 |
| 64 | 65536 | baseline | 60819455 | 30000079 | 2.027315167 | 1.000002633 |
| 64 | 65536 | cached | 180835106 | 668 | 6.027836867 | 0.000022267 |

## Reduction factor, cached vs baseline

| bytes | capacity | producer loads: baseline -> cached | reduction | consumer loads: baseline -> cached | reduction |
|---|---|---|---|---|---|
| 8 | 1024 | 46353397 -> 2662516 | 17.410x | 97021272 -> 184507872 | 0.526x |
| 8 | 4096 | 30851725 -> 538408 | 57.302x | 90345087 -> 222776285 | 0.406x |
| 8 | 65536 | 31966439 -> 456 | 70101.840x | 82997389 -> 342416970 | 0.242x |
| 32 | 1024 | 30240679 -> 10226969 | 2.957x | 321408313 -> 232939827 | 1.380x |
| 32 | 4096 | 30021884 -> 237211 | 126.562x | 364186255 -> 307306074 | 1.185x |
| 32 | 65536 | 30007168 -> 456 | 65805.193x | 444628934 -> 479647739 | 0.927x |
| 64 | 1024 | 69563843 -> 187241776 | 0.372x | 37395698 -> 32351 | 1155.936x |
| 64 | 4096 | 59911914 -> 184641105 | 0.324x | 35709203 -> 8319 | 4292.487x |
| 64 | 65536 | 60819455 -> 180835106 | 0.336x | 30000079 -> 668 | 44910.298x |

## Cells where the cached variant did MORE remote loads than the baseline

These are the cases where the cached copy could not help: the remote
cursor genuinely was not moving, so every failed attempt refreshed
anyway. They are expected in this design and are called out rather than
left for the reader to notice.

| bytes | capacity | side | baseline loads | cached loads |
|---|---|---|---|---|
| 8 | 1024 | consumer | 97021272 | 184507872 |
| 8 | 4096 | consumer | 90345087 | 222776285 |
| 8 | 65536 | consumer | 82997389 | 342416970 |
| 32 | 65536 | consumer | 444628934 | 479647739 |
| 64 | 1024 | producer | 69563843 | 187241776 |
| 64 | 4096 | producer | 59911914 | 184641105 |
| 64 | 65536 | producer | 60819455 | 180835106 |

## Where the mechanism does not pay

Compare the two tables above with `summary.csv`. The important case is a
cell where the producer's remote loads fall dramatically and the
throughput does NOT improve, or gets worse. The consumer's refresh count
is the one to watch: a consumer that keeps finding the queue empty cannot
learn anything from a cached `head`, because the real `head` has not
moved — so it refreshes on every failed attempt, which is exactly as many
remote loads as the baseline performs, plus the extra comparison. That is
a real property of this design and is reported as such, not smoothed over.
