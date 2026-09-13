# Experiment 02 Phase 4 — provenance

Collected **2026-09-13**, 16:25:52 → 16:26:50 (96 s of measured wall time), on
the Apple M3 Max development host described in `HOST.md`.

This is the **sparse-sampled** canonical dataset. It supersedes
`spsc-tail-latency-superseded-per-message-timestamp/`, which timestamped every
message and used a separate `ready_ticks[2 × Capacity]` side array. No number
from that dataset is reused here.

## Repository state

```
HEAD: 3515ea59dad85e5b4d124582d3f6b58e5b6a5571
```

### `git status --short` at collection time

The full listing is recorded in `HOST.md`. Its shape at run time was:

```
 M CMakeLists.txt
 M benchmark/spsc_tail_harness.h
 M benchmark/spsc_tail_latency_bench.cpp
 M docs/results/README.md
R  docs/results/spsc-tail-latency/…  ->  docs/results/spsc-tail-latency-superseded-per-message-timestamp/…
```

**The tree was dirty, and the recorded HEAD does not contain the Phase-4.1
benchmark.** HEAD `3515ea5` is the commit that published the previous Phase-4
work; the Phase-4.1 changes to `benchmark/spsc_tail_harness.h`,
`benchmark/spsc_tail_latency_bench.cpp`, `tests/spsc_tail_latency_tests.cpp` and
the scripts were uncommitted at collection time, and the archive move of the
superseded dataset was staged but not committed. This is why `HOST.md` records
the **digest of each source file as it was on disk at run time** — those digests,
not the commit id, are the authoritative record of what was compiled. A future
re-run is only comparable to this dataset if these digests match.

## Source hashes

The digests below are what was on disk at run time, and they are the
authoritative record of what was compiled — a re-run is comparable to this
dataset only if they match:

```
3cd807df336b8a499191f7354f9f4451d5f6d29e573fd315acef49b3c8d4a4f2  benchmark/spsc_tail_latency_bench.cpp
a96b2ac2eec4ad290bc3c5dd60ee37fcb40004e09c424bbabac553e5631fb0fd  benchmark/spsc_tail_harness.h
0fc7b1926d3a85aa2dc00c2c74ded670a99c12b527e48fb646c39d8391fd17c1  tests/spsc_tail_latency_tests.cpp
576e2df35972c2efdb44314ebcd7025a0d60e24e66a8db63a5c2d1f6fa9a97da  scripts/spsc-tail-latency.sh
6ae01d7afc1b85fb3f79d5694b425eed3021cb8b728cdc1f524eaf06f9886ce3  scripts/verify-spsc-tail-summary.py
ed4855223270e483128a4b8833df7d815b0b2b4bca9ffba1387b433918e8e96a  include/spsc_remote_cursor_ring_buffer.h
```

Note what is **not** in this list: `include/spsc_remote_cursor_ring_buffer.h` is
the frozen Phase-3 header and its digest is recorded to show it was
**unmodified** — Phase 4 changes no queue implementation, and Phase 4.1 changed
only instrumentation inside the benchmark. Its digest is **identical to the one
recorded in the superseded dataset**, which is the evidence for that claim.

### A comment-only pass was made after collection

Four of the six digests above still match the working tree. The two that do not
are the benchmark sources, which received a **documentation-only** edit after the
run — stale comments that still described the superseded per-message
instrumentation (a "countdown" sampler, and a claim that the clock was read
*unconditionally on every message*) were corrected to describe the sparse
sequence-keyed schedule:

```
334bc67cdb7bf0a1f0f6c6d6dcf9653ccee06a9bce58225d950f2a5d3d7de434  benchmark/spsc_tail_latency_bench.cpp   (now)
5fdbbf599bb1df9a6111a3e171fa40612b296d35bb609136867fe41e9e85ef99  benchmark/spsc_tail_harness.h           (now)
```

**This is checked, not asserted.** Reversing exactly those four edits
reconstructs both files byte-for-byte, and the reconstructed files hash to
`3cd807df…` and `a96b2ac2…` — the digests recorded above. That reproduces the
run-time sources from the current tree and therefore **proves no code that
executes in the timed path was changed**: the pre-edit files differed from the
recorded ones in those four text spans and nowhere else.

### A second comment-only pass removed a stale description of the side array

One further documentation-only edit was made in `benchmark/spsc_tail_latency_bench.cpp`
after that. A comment block still described the **superseded** transport — a
stamp written to a "`Capacity`-entry array indexed by `s & (Capacity - 1)`" before
the push — long after the stamp had moved inside the message. It was replaced
with the current description, and the historical side-array reasoning was
redirected to the superseded dataset's `SUPERSEDED.md`, where it belongs. The
current digest is:

```
eed7deb5774aa15bea5da31b6dc6b590b22e7b0ec72de794f4138463f16e56d9  benchmark/spsc_tail_latency_bench.cpp   (now)
5fdbbf599bb1df9a6111a3e171fa40612b296d35bb609136867fe41e9e85ef99  benchmark/spsc_tail_harness.h           (now, unchanged by this pass)
```

Like the first pass, this one is **verifiable rather than asserted**: the diff
consists only of `//` comment lines — no statement, no declaration, no string
literal, no formatting of code — so it cannot alter what the timed path executes.
`benchmark/spsc_tail_harness.h` was not touched at all. The claim is reproducible
from any later tree with:

```sh
git diff -U0 <this-commit> -- benchmark/spsc_tail_latency_bench.cpp \
  | grep -E '^[+-]' | grep -vE '^(\+\+\+|---)' \
  | grep -vE '^[+-][[:space:]]*//'      # must print nothing
```

Because one of the **earlier** edits is inside a string literal (the `--help`
usage text), the **rebuilt binary is not byte-identical** to the one that produced
the dataset:

```
96ae9f35c1f4fff950b2b26a117c27ca93405dce205d49210af72292334393ef  build-p41/spsc_tail_latency_bench   (produced this dataset, 16:22:16)
ac303ca6620ee3a5a12a729b1f41c1772cb9cbbfe66755bcc1827139a7172238  build-p41/spsc_tail_latency_bench   (rebuilt after the documentation pass, 16:37:04)
```

The dataset itself is unaffected — no file under `raw/`, `summaries/`,
`calibration/` or `stderr/` was touched, and the digests and hashes above exist
precisely so this claim is auditable rather than trusted. After the rebuild the
full suite still passes (29/29 CTest).

`scripts/analyze-spsc-tail.py` is a post-hoc consumer of `summaries/*.csv` and
cannot affect the data; it is deliberately not pinned to the run. In Phase 4.1 it
is nevertheless invoked *by* the runner, immediately after the verification gate,
so the derived tables cannot be produced from an unverified dataset.

## Binary

```
96ae9f35c1f4fff950b2b26a117c27ca93405dce205d49210af72292334393ef  build-p41/spsc_tail_latency_bench  (115544 bytes)
```

**This hash was recorded after collection, not at run time** — the runner pins the
sources, not the executable, so it is weaker evidence than the digests above and
is labelled accordingly. The evidence that it is the producing binary:

- Built `2026-09-13 16:22:16`, i.e. **before** the earliest raw file
  (`2026-09-13 16:25:52`).
- All recorded sources are older than it (latest: `scripts/spsc-tail-latency.sh`
  at 16:24:42 — the runner script itself is read, not compiled), so the runner's
  fresh `cmake --build` had nothing to recompile or relink.
- **Nothing rebuilt it during the collection**, and the only later relink is the
  post-collection documentation pass documented under "Source hashes" above,
  which left the dataset files untouched and after which 29/29 CTest still pass.
  That relink is also why the current on-disk binary hashes to `ac303ca6…`
  rather than to the value above.
- It contains **no** AddressSanitizer / UndefinedBehaviorSanitizer /
  ThreadSanitizer symbols, so the canonical numbers did not come from a sanitized
  build. The sanitizer sweeps used separate build directories
  (`build-p41-asan`, `build-p41-tsan`).

The runner was invoked as `BUILDDIR=build-p41 scripts/spsc-tail-latency.sh`, so
the canonical build is the same Release directory used for the 29/29 CTest run
recorded in the Phase-4.1 validation, rather than the runner's default
`build-spsc-tail`.

A rebuild from the recorded sources with the recorded compiler and flags is
expected to reproduce this dataset's *behaviour* but **not** byte-identical
latencies; see "Stability" below.

## Exact commands

Every effective invocation, in the order it ran, is recorded verbatim in
`command.txt`, and `run_order.txt` is parsed back out of `command.txt` so the
record and the order cannot disagree. The collection was driven by:

```
scripts/spsc-tail-latency.sh
```

which configures and builds the Release tree with forced `-O3 -DNDEBUG`, runs the
36 processes, runs the raw→summary verifier and writes its output to
`invariants.txt`, then runs `scripts/analyze-spsc-tail.py` to derive the
cross-repetition tables.

## How to re-verify this dataset

```
scripts/verify-spsc-tail-summary.py docs/results/spsc-tail-latency
scripts/analyze-spsc-tail.py       docs/results/spsc-tail-latency
```

The first recomputes every percentile, the mean, the count, the checksum
relationships **and the sparse-instrumentation counters** from `raw/*.csv` and
compares them to `summaries/*.csv`; it must print
`ALL PHASE 4 RAW -> SUMMARY CHECKS PASSED` and a check count of **5,240,804**.
The second regenerates the four derived files and **refuses to run** unless
`invariants.txt` records that passing verification.

The queue-level correctness of the harness itself is covered by CTest
(`spsc_tail_latency_tests` plus the benchmark's smoke and rejection tests); the
canonical numbers come only from the non-sanitized Release build above.

### The instrumentation is provably sparse

Every one of the 180 repetitions records how many clock reads the instrumentation
actually took:

```
expected_samples == sample_count
                 == producer_sample_clock_reads
                 == consumer_sample_clock_reads  ==  9,696
stamp_contract_failures                         ==  0
```

That is 1,745,280 sampled clock reads per thread across the whole dataset,
against 1,800,000,000 if every message were stamped — **0.097 %**. A repetition
that took a per-message clock read would report ~10,000,000 here and fail, rather
than quietly producing the same 9,696 recorded latencies.

The claim can be re-derived from the published summaries without trusting the
benchmark — columns 6–10 are `expected_samples`, `sample_count`,
`producer_sample_clock_reads`, `consumer_sample_clock_reads` and
`stamp_contract_failures`:

```
cd docs/results/spsc-tail-latency
awk -F, '!/^#/ && $1!="session" {n++; if ($7!=$6 || $8!=$6 || $9!=$6 || $10!=0) bad++}
         END {printf "repetitions=%d  rows disagreeing with the sparse contract=%d\n", n, bad+0}' summaries/*.csv
```

which reports `repetitions=180  rows disagreeing with the sparse contract=0`.

## Stability

The superseded dataset's stability note said: *"Repeat the collection and the
per-cell P50 and P90 will move — in some cells by two orders of magnitude."*
**That is no longer true of this dataset, and the difference is the point of
Phase 4.1.** With per-message timestamping removed, P50 is constant at 125 ns in
all 120 repetitions of the six fast-band cells and moves by at most 1.11× in the
other three. What remains variable is the far tail: P99.9 and max still spread by
up to four orders of magnitude across a cell's 20 repetitions, because they are
single observations.

So: **P50 here is a reproducible property of the configuration; P99.9 and max are
not.** Do not compare either against a re-run as though a difference were a
treatment effect — there is no treatment in this matrix.

## What was NOT done

- **No frozen queue implementation was modified.** `include/` is untouched by
  this phase, and the digest above is identical to the superseded dataset's.
- **No Phase-2, Phase-3A or Phase-3B dataset or result file was read, modified or
  regenerated.** Their directories are byte-identical.
- **No treatment comparison was performed.** The matrix contains exactly one
  queue configuration; the cached variant, `MutexBoundedQueue` and the
  `same_line` layout are absent by design.
- **No new optimization was introduced.** Phase 4 is a measurement phase, and no
  further SPSC optimization phase is opened from it.
- **No number from the superseded dataset was carried over.** Every figure in
  `docs/SPSC_TAIL_LATENCY.md` is regenerated from these files.
- No number in this dataset is compared with a Phase-2/3A/3B `ns/msg` figure, and
  no latency value is attributed to a cache, coherence, scheduler, core-type or
  frequency cause.

## One run of this experiment was discarded

An earlier complete 36-process run — every gate passing, every verification check
green — was measured **concurrently with other work on the same host** and is
therefore not usable. It is retained, unedited and clearly labelled, at
`docs/results/spsc-tail-latency-CONTAMINATED-concurrent-load/`, with its derived
aggregate tables **deleted** so no quotable summary of it survives. That
directory is deliberately **not committed** — 106 MB of raw data for a run that
cannot be used — so a clone will contain neither it nor its `SUPERSEDED.md`; the
repository keeps the "Two hazards" section of `docs/SPSC_TAIL_LATENCY.md`
instead.

**On this dataset the post-hoc rate diagnostic did not fire.** No repetition
exceeded 5× its own cell's median `ns_per_message` (worst cell spread 3.01×), so
`invariants.txt` records no warning. Note what that does and does not establish:
it rules out a *sustained* slowdown of any repetition. It does not rule out a
short stall inside one, and one cell — 32 B / 65536 B — does contain three
consecutive repetitions in a single process with maxima of 1.85 ms, 3.62 ms and
20.4 ms. Those are reported, named and left in the dataset rather than censored,
because magnitude alone is not proof of invalidity; see Q6 and the H2 section of
`docs/SPSC_TAIL_LATENCY.md`.
