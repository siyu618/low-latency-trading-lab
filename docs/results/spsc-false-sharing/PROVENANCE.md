# Experiment 02 Phase 3A — provenance of this dataset

Recorded so that the exact code which produced these numbers can be
identified later, even if the working tree was not committed when the run
happened.

## Revision

```
git HEAD            : 6916a804a5a98421cb8ce2f6ada00f2adc7e8685
git describe        : 6916a80-dirty
working tree        : DIRTY (uncommitted changes present)
run started (UTC)   : 2026-09-12T03:13:49Z
```

### `git status --porcelain` as recorded

```
 M README.md
 M benchmark/spsc_false_sharing_bench.cpp
 M docs/SPSC_FALSE_SHARING.md
 M docs/results/README.md
R  docs/results/spsc-false-sharing/HOST.md -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/HOST.md
R  docs/results/spsc-false-sharing/LAYOUT_VERIFICATION.md -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/LAYOUT_VERIFICATION.md
R  docs/results/spsc-false-sharing/MATRIX.md -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/MATRIX.md
R  docs/results/spsc-false-sharing/PAIRED_COMPARISON.md -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/PAIRED_COMPARISON.md
R  docs/results/spsc-false-sharing/RESULTS_METADATA.md -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/RESULTS_METADATA.md
R  docs/results/spsc-false-sharing/SESSIONS.md -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/SESSIONS.md
R  docs/results/spsc-false-sharing/command.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/command.txt
R  docs/results/spsc-false-sharing/invariants.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/invariants.txt
R  docs/results/spsc-false-sharing/paired_summary.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/paired_summary.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c1024_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c1024_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c1024_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c1024_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c1024_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c1024_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c1024_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c1024_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c4096_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c4096_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c4096_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c4096_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c4096_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c4096_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c4096_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c4096_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c65536_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c65536_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c65536_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c65536_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c65536_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c65536_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b32_c65536_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b32_c65536_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c1024_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c1024_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c1024_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c1024_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c1024_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c1024_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c1024_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c1024_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c4096_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c4096_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c4096_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c4096_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c4096_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c4096_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c4096_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c4096_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c65536_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c65536_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c65536_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c65536_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c65536_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c65536_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b64_c65536_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b64_c65536_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c1024_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c1024_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c1024_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c1024_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c1024_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c1024_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c1024_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c1024_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c4096_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c4096_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c4096_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c4096_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c4096_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c4096_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c4096_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c4096_s4.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c65536_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c65536_s1.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c65536_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c65536_s2.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c65536_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c65536_s3.csv
R  docs/results/spsc-false-sharing/raw/same_line_b8_c65536_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/same_line_b8_c65536_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c1024_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c1024_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c1024_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c1024_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c1024_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c1024_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c1024_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c1024_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c4096_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c4096_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c4096_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c4096_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c4096_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c4096_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c4096_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c4096_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c65536_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c65536_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c65536_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c65536_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c65536_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c65536_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b32_c65536_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b32_c65536_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c1024_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c1024_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c1024_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c1024_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c1024_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c1024_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c1024_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c1024_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c4096_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c4096_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c4096_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c4096_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c4096_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c4096_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c4096_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c4096_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c65536_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c65536_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c65536_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c65536_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c65536_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c65536_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b64_c65536_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b64_c65536_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c1024_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c1024_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c1024_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c1024_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c1024_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c1024_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c1024_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c1024_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c4096_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c4096_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c4096_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c4096_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c4096_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c4096_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c4096_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c4096_s4.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c65536_s1.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c65536_s1.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c65536_s2.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c65536_s2.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c65536_s3.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c65536_s3.csv
R  docs/results/spsc-false-sharing/raw/separated_b8_c65536_s4.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/raw/separated_b8_c65536_s4.csv
R  docs/results/spsc-false-sharing/run_order.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/run_order.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c1024_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c1024_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c1024_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c1024_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c4096_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c4096_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c4096_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c4096_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c65536_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c65536_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c65536_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b32_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b32_c65536_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c1024_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c1024_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c1024_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c1024_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c4096_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c4096_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c4096_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c4096_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c65536_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c65536_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c65536_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b64_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b64_c65536_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c1024_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c1024_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c1024_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c1024_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c4096_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c4096_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c4096_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c4096_s4.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c65536_s1.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c65536_s2.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c65536_s3.txt
R  docs/results/spsc-false-sharing/stderr/same_line_b8_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/same_line_b8_c65536_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c1024_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c1024_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c1024_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c1024_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c4096_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c4096_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c4096_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c4096_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c65536_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c65536_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c65536_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b32_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b32_c65536_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c1024_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c1024_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c1024_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c1024_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c4096_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c4096_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c4096_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c4096_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c65536_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c65536_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c65536_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b64_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b64_c65536_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c1024_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c1024_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c1024_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c1024_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c4096_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c4096_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c4096_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c4096_s4.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c65536_s1.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c65536_s2.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c65536_s3.txt
R  docs/results/spsc-false-sharing/stderr/separated_b8_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/stderr/separated_b8_c65536_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c1024_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c1024_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c1024_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c1024_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c4096_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c4096_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c4096_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c4096_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c65536_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c65536_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c65536_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b32_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b32_c65536_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c1024_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c1024_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c1024_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c1024_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c4096_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c4096_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c4096_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c4096_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c65536_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c65536_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c65536_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b64_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b64_c65536_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c1024_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c1024_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c1024_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c1024_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c4096_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c4096_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c4096_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c4096_s4.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c65536_s1.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c65536_s2.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c65536_s3.txt
R  docs/results/spsc-false-sharing/summaries/same_line_b8_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/same_line_b8_c65536_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c1024_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c1024_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c1024_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c1024_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c4096_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c4096_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c4096_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c4096_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c65536_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c65536_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c65536_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b32_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b32_c65536_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c1024_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c1024_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c1024_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c1024_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c4096_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c4096_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c4096_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c4096_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c65536_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c65536_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c65536_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b64_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b64_c65536_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c1024_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c1024_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c1024_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c1024_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c1024_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c1024_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c1024_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c1024_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c4096_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c4096_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c4096_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c4096_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c4096_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c4096_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c4096_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c4096_s4.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c65536_s1.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c65536_s1.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c65536_s2.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c65536_s2.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c65536_s3.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c65536_s3.txt
R  docs/results/spsc-false-sharing/summaries/separated_b8_c65536_s4.txt -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summaries/separated_b8_c65536_s4.txt
R  docs/results/spsc-false-sharing/summary.csv -> docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/summary.csv
 M include/cache_line.h
 M include/spsc_cursor_layout_ring_buffer.h
 M scripts/spsc-false-sharing.sh
 M tests/spsc_false_sharing_tests.cpp
?? docs/results/spsc-false-sharing-pre3a1-payload-offset-confounded/SUPERSEDED.md
?? docs/results/spsc-false-sharing/
```

The same output with entries under the archived pre-3A.1 dataset
removed, so the entries that identify the CODE are visible:

```
 M README.md
 M benchmark/spsc_false_sharing_bench.cpp
 M docs/SPSC_FALSE_SHARING.md
 M docs/results/README.md
 M include/cache_line.h
 M include/spsc_cursor_layout_ring_buffer.h
 M scripts/spsc-false-sharing.sh
 M tests/spsc_false_sharing_tests.cpp
?? docs/results/spsc-false-sharing/
```

If this run happened with a non-empty status, HEAD does **not** identify
the code that produced this dataset. The hashes below do.

## SHA-256 of the artefacts that produced the data

```
b4c83b92af8c66c7a61a2103ccf90d18ce4bb307064df3cd81244b0a7f6d7783  build-spsc-false-sharing/spsc_false_sharing_bench
0c75e6bd6d0bde7e61af71e4ac38fce864a17b4d115044193f6abd4f8335ebf5  include/cache_line.h
7f74cd1e3f0d2364769dd5278248ce198e9f58cfd3320b84700149fbd8599096  include/spsc_cursor_layout_ring_buffer.h
175b090afe3eacc664f98b33be1a4a4b001f3d85c40d67a0717e56f3a32b906f  benchmark/spsc_false_sharing_bench.cpp
8a42b62ccfdba7513c0d197d0be3836bb6963af5eaf0c4066f859f656438d103  scripts/spsc-false-sharing.sh
```

The benchmark executable hash covers the whole translation unit as
compiled, so it changes if any header it includes changes, not only if the
`.cpp` does.

## Build

```
build system        : CMake, fresh build directory (build-spsc-false-sharing, removed first)
CMAKE_BUILD_TYPE    : Release
BENCH_ARCH_FLAGS    : '' (empty for the canonical run)
target              : spsc_false_sharing_bench
configure           : cmake -S . -B build-spsc-false-sharing -DCMAKE_BUILD_TYPE=Release -DBENCH_ARCH_FLAGS=
build               : cmake --build build-spsc-false-sharing --target spsc_false_sharing_bench -j
compiler            : Apple clang version 15.0.0 (clang-1500.3.9.4)
```

## Command order

The exact effective invocations, in execution order, are in
`command.txt`; the parsed execution order with the per-cell first layout
is in `run_order.txt`.

## Host and reported cache-line size

- host-reported cache-line size: **128 bytes**, taken from the
  `reported_cache_line_size` column of the raw data itself.
- compile-time layout assumption: 128 bytes.
- full host/toolchain metadata: `HOST.md`.

Analysis, methodology and limitations: `docs/SPSC_FALSE_SHARING.md`.
