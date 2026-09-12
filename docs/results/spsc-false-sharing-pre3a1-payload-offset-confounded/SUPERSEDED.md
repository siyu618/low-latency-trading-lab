# SUPERSEDED — pre-Phase-3A.1 dataset (payload-offset confounded)

**Do not cite this dataset for causal cursor-placement claims.** It is retained
as real, self-validating evidence and as the reason Phase 3A.1 exists.

## What this dataset is

The Experiment 02 Phase 3A run measured 2026-09-12 on the Apple M3 Max, before
the Phase-3A.1 controlled-variable hardening: 4 balanced AB/BA sessions, 72
processes, 360 measured repetitions, 10M messages per repetition.

## What remains true about it

- **All measurements are real.** Every raw per-repetition CSV in `raw/` is the
  unedited output of the benchmark binary that produced it.
- **Correctness passed.** All 360 measured repetitions report
  `correctness=PASS` with a stable per-cell checksum and an exact FIFO,
  no-loss, no-duplicate transfer of all 10,000,000 messages.
- **Cursor-layout verification passed.** All 360 measured repetitions carried a
  passing runtime layout verification under the host's reported 128-byte cache
  line: `same_line` rows measured `head`/`tail` in one line, `separated` rows
  measured them in different lines, and no cursor block overlapped payload
  storage. See `LAYOUT_VERIFICATION.md`.
- **The balanced AB/BA order held,** 2 same-line-first and 2 separated-first per
  cell, verified from `command.txt` into `run_order.txt`.

## Why it is superseded

The two Phase-3A controls did not have the same cursor-policy footprint:

| variant | cursor policy size | payload offset from object base |
|---|---|---|
| `same_line` | 128 bytes | **128** |
| `separated` | 256 bytes | **256** |

Because `slots_` is declared after `cursors_`, the payload array began at a
different *relative offset within the object* in the two variants — 128 bytes
apart. That is a systematic object-layout/address-mapping shift on top of the
cursor move. It is stated as a layout shift, not as a known hardware cache set:
this experiment recorded object addresses and cursor placement, did not measure
the hardware's cache-set indexing function, and the two variants ran in separate
processes with independently allocated objects.
The run therefore changed **two** things at once:

1. cursor cache-line placement — the intended variable; and
2. payload/object relative layout — an uncontrolled second variable.

A difference measured here cannot be attributed to cursor placement alone. That
is a real controlled-variable flaw regardless of how large the effect looked:
the harness contains full/empty retry feedback, an occasional `yield`, and OS
scheduling, so a small low-level difference can be amplified into a larger
end-to-end throughput difference. This dataset contains **no hardware-counter
evidence** that would separate the two variables or bound their contributions.

## What was done about it

Phase 3A.1 equalizes the two cursor policies to the same 256-byte footprint —
`same_line` keeps **both** cursors in the first cache line and reserves an
untouched second line purely to match the footprint — so the payload offset is
identical across variants. The hardened canonical dataset then replaces this one
at `docs/results/spsc-false-sharing/`.

This directory may be compared against the hardened dataset as a **secondary
methodology observation** only. Primary Phase-3A conclusions must use the
equal-footprint dataset.

## Editing statement

**No raw numeric result has been edited.** Nothing in `raw/`, `summaries/`,
`stderr/`, `summary.csv`, `paired_summary.csv`, `MATRIX.md`, `SESSIONS.md`,
`PAIRED_COMPARISON.md`, `LAYOUT_VERIFICATION.md`, `invariants.txt`,
`command.txt`, `run_order.txt`, `HOST.md` or `RESULTS_METADATA.md` was modified
when this directory was archived — it was moved as a whole. The only file added
is this `SUPERSEDED.md`.

**The pre-3A.1 analysis conclusions are likewise superseded.** The direction
table and the false-sharing attribution discussion that were written against
this dataset have been rewritten against the equal-footprint dataset.
