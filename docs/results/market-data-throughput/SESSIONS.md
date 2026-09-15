# Experiment 03 Phase 3A — session design

| session | cell | processes | measured reps | warm-up |
|---|---|---|---|---|
| 1 | 64 KiB chunks / capacity 4096 | 1 | 5 | 1 |
| 2 | 64 KiB chunks / capacity 4096 | 1 | 5 | 1 |
| 3 | 64 KiB chunks / capacity 4096 | 1 | 5 | 1 |
| 4 | 64 KiB chunks / capacity 4096 | 1 | 5 | 1 |

One process per session, and one cell in the whole phase, so every
session measures the **same** configuration at a different point in time.

**What the four sessions buy:** with the cell held fixed, all variation
between session medians is temporal. The dataset reports that spread
instead of folding it into a single number with no error bar.

**What they do NOT buy:** they are run back-to-back and are therefore NOT
independent trials, and this is not a randomised experiment. There is no
AB/BA counterbalance here because there is no treatment pair — Phase 3A
has one cell and compares nothing. No claim of a controlled thermal or
frequency regime is made anywhere in this dataset.

**Published figure:** the per-session median of the 5 measured
repetitions, then the median across the 4 session medians. Every
raw repetition is preserved in `raw/`. No repetition is discarded and
repetitions are never pooled into one giant observation.
