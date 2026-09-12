# Experiment 02 Phase 3B — results metadata

```
# Experiment 02 Phase 3B — dataset invariants
# Every line below was CHECKED, not assumed. Any violation aborted the run.
expected_processes=72
recorded_canonical_invocations=72
canonical_invocations_instrumented=0
raw_csv_files=72
sessions=4
messages_per_repetition=10000000
reps_per_process=5
warmup_reps_per_process=1
mechanism_leg=1
mechanism_processes=18
mechanism_reps=3
session_first_variant_order=baseline cached cached baseline
ab_ba_balance=2 baseline-first + 2 cached-first per cell
pair_adjacency=VERIFIED
footprint_equality_across_pairs=VERIFIED
separated_layout_verified_on_every_row=VERIFIED
cached_placement_verified_on_every_row=VERIFIED
instrumentation_leak_check=PASS
summary_vs_raw_verification=VERIFIED for every process
```

## Files

| path | contents |
|---|---|
| `command.txt` | every effective invocation, in execution order |
| `raw/` | one raw per-repetition CSV per canonical process (72 files) |
| `summaries/` | one process summary per canonical process |
| `stderr/` | the layout-verification trace of each canonical process |
| `mechanism/` | the instrumented leg: raw, summaries and stderr |
| `mechanism/ATTEMPTS.csv` | **DERIVED** attempt-normalized mechanism metrics (`loads_per_attempt`, the primary mechanism metric). Computed from `mechanism/raw/*.csv`; no raw file was edited and no new measurement was taken. |
| `summary.csv` | pooled per-process view (SECONDARY) |
| `paired_summary.csv` | the paired ratios (PRIMARY), machine-readable |
| `PAIRED_COMPARISON.md` | the paired analysis (PRIMARY) |
| `MECHANISM.md` | MEASURED MECHANISM (remote loads), not performance |
| `MATRIX.md` | human-readable pooled matrix |
| `SESSIONS.md` | per-session process medians |
| `LAYOUT_VERIFICATION.md` | runtime placement and footprint evidence |
| `invariants.txt` | the checked dataset invariants |
| `run_order.txt` | the execution order parsed back out of command.txt |
| `HOST.md` | host and toolchain metadata |
| `PROVENANCE.md` | repo state, source and binary hashes, exact commands |

## Status

**Phase 3B: collected.** The canonical status is recorded in
`docs/SPSC_REMOTE_CURSOR_CACHE.md` and in the top-level README, not here.
This file describes the dataset; it does not declare the phase complete.
