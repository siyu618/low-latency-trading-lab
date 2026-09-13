# Experiment 02 Phase 4 — per-cell latency matrix

**DERIVED** from `summaries/*.csv` by `scripts/analyze-spsc-tail.py`. Not a measurement.

**No distribution is pooled across repetitions.** A "P99" here is the P99 a typical repetition exhibited, not the pooled 99th percentile of every sample in the cell.

## How to read this file: two levels, one of them canonical

The design is **nested** — each cell has 4 sessions, each session has 5 measured repetitions — so "the cell's P99" can be defined two ways, and they give different numbers:

* **PRIMARY — session-blocked.** repetition → median of that session's 5 repetitions → **median of the 4 session medians**. This is the figure to quote.
* **SECONDARY — all repetitions.** repetition → median across all 20 repetition-level statistics. A **diagnostic only**.

These are **not mathematically identical.** A median of medians is not a median of all values: the blocked median weights each *session* equally, the all-20 median weights each *repetition* equally — with 5 repetitions per session the blocked figure is a weighted median of the same 20 numbers. For an odd number of sessions the two coincide; for 4 they generally do not. Where they differ, **the PRIMARY column is the cell's value** and the SECONDARY column is context, never a competing headline.

The session-blocked level is primary because of how the data was collected: the 5 repetitions inside one session share a process, a thread placement and a moment in time, so the 20 repetitions of a cell are not 20 exchangeable observations. Collapsing each session first stops a single session that behaved differently from dominating the cell's summary.

## PRIMARY — session-blocked, per cell (ns)

`min` / `max` are the **minimum and maximum session median** for that metric — the spread the blocked median sits inside, and the first thing to look at before quoting any single number. `spread` is `max/min` across the four session medians.

| cell | metric | blocked | min session | max session | spread |
|---|---|---|---|---|---|
| 16 B / 1024 B | P50 | 125 | 125 | 125 | 1.00x |
| 16 B / 1024 B | P90 | 167 | 167 | 167 | 1.00x |
| 16 B / 1024 B | P99 | 437 | 334 | 833 | 2.49x |
| 16 B / 1024 B | P99.9 | 5896 | 2166 | 10583 | 4.89x |
| 16 B / 1024 B | max | 79062 | 10542 | 148292 | 14.07x |
| 16 B / 4096 B | P50 | 125 | 125 | 125 | 1.00x |
| 16 B / 4096 B | P90 | 167 | 166 | 167 | 1.01x |
| 16 B / 4096 B | P99 | 458 | 292 | 459 | 1.57x |
| 16 B / 4096 B | P99.9 | 8395 | 4083 | 9459 | 2.32x |
| 16 B / 4096 B | max | 23666 | 13500 | 45917 | 3.40x |
| 16 B / 65536 B | P50 | 125 | 125 | 125 | 1.00x |
| 16 B / 65536 B | P90 | 125 | 125 | 166 | 1.33x |
| 16 B / 65536 B | P99 | 645 | 541 | 709 | 1.31x |
| 16 B / 65536 B | P99.9 | 7000 | 4500 | 7667 | 1.70x |
| 16 B / 65536 B | max | 26396 | 18791 | 43167 | 2.30x |
| 32 B / 1024 B | P50 | 125 | 125 | 125 | 1.00x |
| 32 B / 1024 B | P90 | 167 | 166 | 167 | 1.01x |
| 32 B / 1024 B | P99 | 729 | 292 | 1750 | 5.99x |
| 32 B / 1024 B | P99.9 | 11437 | 6792 | 19709 | 2.90x |
| 32 B / 1024 B | max | 75750 | 12000 | 145250 | 12.10x |
| 32 B / 4096 B | P50 | 125 | 125 | 125 | 1.00x |
| 32 B / 4096 B | P90 | 166 | 125 | 167 | 1.34x |
| 32 B / 4096 B | P99 | 708 | 250 | 792 | 3.17x |
| 32 B / 4096 B | P99.9 | 20312 | 8125 | 30750 | 3.78x |
| 32 B / 4096 B | max | 54833 | 16458 | 65292 | 3.97x |
| 32 B / 65536 B | P50 | 125 | 125 | 125 | 1.00x |
| 32 B / 65536 B | P90 | 125 | 125 | 583 | 4.66x |
| 32 B / 65536 B | P99 | 438 | 250 | 270583 | 1082.33x |
| 32 B / 65536 B | P99.9 | 7458 | 4708 | 1817375 | 386.02x |
| 32 B / 65536 B | max | 31729 | 14459 | 1845333 | 127.63x |
| 64 B / 1024 B | P50 | 30042 | 28875 | 31500 | 1.09x |
| 64 B / 1024 B | P90 | 31437 | 30708 | 69417 | 2.26x |
| 64 B / 1024 B | P99 | 71687 | 64333 | 93417 | 1.45x |
| 64 B / 1024 B | P99.9 | 103104 | 89958 | 124917 | 1.39x |
| 64 B / 1024 B | max | 212437 | 111416 | 988250 | 8.87x |
| 64 B / 4096 B | P50 | 120396 | 116459 | 124666 | 1.07x |
| 64 B / 4096 B | P90 | 128562 | 126333 | 130500 | 1.03x |
| 64 B / 4096 B | P99 | 279333 | 261875 | 316250 | 1.21x |
| 64 B / 4096 B | P99.9 | 361625 | 351750 | 446416 | 1.27x |
| 64 B / 4096 B | max | 489375 | 411583 | 633542 | 1.54x |
| 64 B / 65536 B | P50 | 1952833 | 1909250 | 1997792 | 1.05x |
| 64 B / 65536 B | P90 | 2025083 | 2017250 | 2128084 | 1.05x |
| 64 B / 65536 B | P99 | 3771208 | 3564125 | 3885583 | 1.09x |
| 64 B / 65536 B | P99.9 | 4055666 | 3954542 | 4146167 | 1.05x |
| 64 B / 65536 B | max | 4088416 | 3969541 | 4213584 | 1.06x |

## SECONDARY (diagnostic) — all-20 median beside the blocked one

Shown **only** so the size of the aggregation choice is visible. It is not an alternative headline. `delta` is the all-20 median as a percentage of the blocked median.

| cell | metric | blocked (PRIMARY) | all-20 (secondary) | delta |
|---|---|---|---|---|
| 16 B / 1024 B | P50 | 125 | 125 | +0.0% |
| 16 B / 1024 B | P90 | 167 | 167 | +0.0% |
| 16 B / 1024 B | P99 | 437 | 417 | -4.7% |
| 16 B / 1024 B | P99.9 | 5896 | 7000 | +18.7% |
| 16 B / 1024 B | max | 79062 | 49750 | -37.1% |
| 16 B / 4096 B | P50 | 125 | 125 | +0.0% |
| 16 B / 4096 B | P90 | 167 | 167 | +0.0% |
| 16 B / 4096 B | P99 | 458 | 437 | -4.7% |
| 16 B / 4096 B | P99.9 | 8395 | 8520 | +1.5% |
| 16 B / 4096 B | max | 23666 | 26896 | +13.6% |
| 16 B / 65536 B | P50 | 125 | 125 | +0.0% |
| 16 B / 65536 B | P90 | 125 | 125 | +0.0% |
| 16 B / 65536 B | P99 | 645 | 645 | +0.0% |
| 16 B / 65536 B | P99.9 | 7000 | 7021 | +0.3% |
| 16 B / 65536 B | max | 26396 | 26895 | +1.9% |
| 32 B / 1024 B | P50 | 125 | 125 | +0.0% |
| 32 B / 1024 B | P90 | 167 | 167 | +0.0% |
| 32 B / 1024 B | P99 | 729 | 708 | -2.9% |
| 32 B / 1024 B | P99.9 | 11437 | 10895 | -4.7% |
| 32 B / 1024 B | max | 75750 | 47562 | -37.2% |
| 32 B / 4096 B | P50 | 125 | 125 | +0.0% |
| 32 B / 4096 B | P90 | 166 | 166 | +0.0% |
| 32 B / 4096 B | P99 | 708 | 687 | -3.0% |
| 32 B / 4096 B | P99.9 | 20312 | 16770 | -17.4% |
| 32 B / 4096 B | max | 54833 | 58896 | +7.4% |
| 32 B / 65536 B | P50 | 125 | 125 | +0.0% |
| 32 B / 65536 B | P90 | 125 | 125 | +0.0% |
| 32 B / 65536 B | P99 | 438 | 520 | +18.8% |
| 32 B / 65536 B | P99.9 | 7458 | 8521 | +14.2% |
| 32 B / 65536 B | max | 31729 | 37291 | +17.5% |
| 64 B / 1024 B | P50 | 30042 | 30896 | +2.8% |
| 64 B / 1024 B | P90 | 31437 | 31624 | +0.6% |
| 64 B / 1024 B | P99 | 71687 | 75500 | +5.3% |
| 64 B / 1024 B | P99.9 | 103104 | 107792 | +4.5% |
| 64 B / 1024 B | max | 212437 | 143416 | -32.5% |
| 64 B / 4096 B | P50 | 120396 | 123896 | +2.9% |
| 64 B / 4096 B | P90 | 128562 | 127958 | -0.5% |
| 64 B / 4096 B | P99 | 279333 | 287229 | +2.8% |
| 64 B / 4096 B | P99.9 | 361625 | 367062 | +1.5% |
| 64 B / 4096 B | max | 489375 | 543375 | +11.0% |
| 64 B / 65536 B | P50 | 1952833 | 1988916 | +1.8% |
| 64 B / 65536 B | P90 | 2025083 | 2036833 | +0.6% |
| 64 B / 65536 B | P99 | 3771208 | 3714084 | -1.5% |
| 64 B / 65536 B | P99.9 | 4055666 | 4013438 | -1.0% |
| 64 B / 65536 B | max | 4088416 | 4111250 | +0.6% |

## P50 / P99, per cell and session (ns)

Each session cell is `P50 / P99`, a median across that session's 5 repetitions. The two spread columns are max/min across the four session medians, kept separate because P50 and P99 do not move together: a cell can have a stable tail and a median that moves by several-fold.

| cell | s1 | s2 | s3 | s4 | P50 spread | P99 spread |
|---|---|---|---|---|---|---|
| 16 B / 1024 B | 125 / 334 | 125 / 833 | 125 / 375 | 125 / 500 | 1.00x | 2.49x |
| 16 B / 4096 B | 125 / 292 | 125 / 459 | 125 / 459 | 125 / 458 | 1.00x | 1.57x |
| 16 B / 65536 B | 125 / 709 | 125 / 541 | 125 / 708 | 125 / 583 | 1.00x | 1.31x |
| 32 B / 1024 B | 125 / 1750 | 125 / 750 | 125 / 292 | 125 / 708 | 1.00x | 5.99x |
| 32 B / 4096 B | 125 / 709 | 125 / 250 | 125 / 708 | 125 / 792 | 1.00x | 3.17x |
| 32 B / 65536 B | 125 / 584 | 125 / 250 | 125 / 270583 | 125 / 292 | 1.00x | 1082.33x |
| 64 B / 1024 B | 28875 / 66625 | 29084 / 64333 | 31500 / 93417 | 31000 / 76750 | 1.09x | 1.45x |
| 64 B / 4096 B | 116875 / 276209 | 116459 / 282458 | 124666 / 316250 | 123917 / 261875 | 1.07x | 1.21x |
| 64 B / 65536 B | 1909250 / 3885583 | 1913583 / 3564125 | 1997792 / 3840583 | 1992083 / 3701834 | 1.05x | 1.09x |

## Tail ratios (median across repetitions)

| cell | session | P99/P50 | P99.9/P50 | max/P50 |
|---|---|---|---|---|
| b16_c1024 | 1 | 2.67 | 31.00 | 84.34 |
| b16_c1024 | 2 | 6.66 | 84.66 | 1186.34 |
| b16_c1024 | 3 | 3.00 | 17.33 | 449.34 |
| b16_c1024 | 4 | 4.00 | 63.34 | 815.66 |
| b16_c4096 | 1 | 2.34 | 62.00 | 108.00 |
| b16_c4096 | 2 | 3.67 | 72.33 | 254.00 |
| b16_c4096 | 3 | 3.67 | 75.67 | 367.34 |
| b16_c4096 | 4 | 3.66 | 32.66 | 124.66 |
| b16_c65536 | 1 | 5.67 | 61.34 | 345.34 |
| b16_c65536 | 2 | 4.33 | 51.00 | 244.34 |
| b16_c65536 | 3 | 5.66 | 36.00 | 150.33 |
| b16_c65536 | 4 | 4.66 | 61.00 | 178.00 |
| b32_c1024 | 1 | 14.00 | 157.67 | 1162.00 |
| b32_c1024 | 2 | 6.00 | 83.00 | 204.34 |
| b32_c1024 | 3 | 2.34 | 54.34 | 96.00 |
| b32_c1024 | 4 | 5.66 | 100.00 | 1007.67 |
| b32_c4096 | 1 | 5.67 | 246.00 | 522.34 |
| b32_c4096 | 2 | 2.00 | 65.00 | 131.66 |
| b32_c4096 | 3 | 5.66 | 107.66 | 398.66 |
| b32_c4096 | 4 | 6.34 | 217.34 | 478.66 |
| b32_c65536 | 1 | 4.67 | 55.00 | 228.00 |
| b32_c65536 | 2 | 2.00 | 64.34 | 115.67 |
| b32_c65536 | 3 | 2164.66 | 14539.00 | 14762.66 |
| b32_c65536 | 4 | 2.34 | 37.66 | 279.66 |
| b64_c1024 | 1 | 2.29 | 3.10 | 4.54 |
| b64_c1024 | 2 | 2.21 | 3.15 | 3.85 |
| b64_c1024 | 3 | 2.96 | 3.96 | 31.33 |
| b64_c1024 | 4 | 2.50 | 3.54 | 9.18 |
| b64_c4096 | 1 | 2.36 | 3.18 | 4.69 |
| b64_c4096 | 2 | 2.31 | 3.02 | 3.70 |
| b64_c4096 | 3 | 2.54 | 3.58 | 5.08 |
| b64_c4096 | 4 | 2.11 | 2.83 | 3.32 |
| b64_c65536 | 1 | 2.05 | 2.17 | 2.21 |
| b64_c65536 | 2 | 1.83 | 2.09 | 2.12 |
| b64_c65536 | 3 | 1.92 | 1.98 | 1.99 |
| b64_c65536 | 4 | 1.86 | 2.08 | 2.09 |
