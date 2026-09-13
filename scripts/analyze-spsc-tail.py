#!/usr/bin/env python3
"""Experiment 02 Phase 4 — cross-repetition aggregation for the docs.

Reads the per-repetition summaries the benchmark wrote and produces the tables
the methodology doc cites. It adds no measurement: every number here is derived
from `summaries/*.csv`, and it refuses to run if the raw->summary verification
has not already passed.

THE ONE RULE THIS SCRIPT EXISTS TO ENFORCE
------------------------------------------
Distributions are NEVER pooled. `summaries/*.csv` holds one distribution per
measured repetition, and the benchmark's own header says so: "No repetition is
pooled into another." Concatenating 5 repetitions' samples into one 48,480-value
distribution would let a between-repetition regime shift masquerade as a within
repetition tail, which is exactly the artifact a tail study must not create.

So every cross-repetition figure here is an aggregate OF THE PER-REPETITION
STATISTICS — the median across the measured repetitions of that repetition-level
percentile. A "P99 of X ns" in the output means "the median repetition had a P99
of X ns". It never means "99% of all samples across all repetitions were under
X". Both are legitimate quantities; only the first is reported.

Outputs (into the results directory):
  CELL_TAIL.csv    machine-readable per-cell, per-session aggregates
  TAIL_MATRIX.md   the same as a readable table, plus cross-session spread
  TAIL_RATIOS.csv  per-repetition tail ratios (p99/p50, p999/p50, max/p50)

Usage: scripts/analyze-spsc-tail.py <results-dir>
Exit: 0 = tables written, 1 = refused (verification missing or data malformed).
"""

import csv
import os
import statistics
import sys
from collections import defaultdict

# The benchmark prints every percentile as an exact integer of nanoseconds and
# the mean as a float, because the mean is the only one that is not an observed
# sample. Keep them apart rather than rounding the mean into an integer.
INT_METRICS = ("p50_ns", "p90_ns", "p99_ns", "p999_ns", "max_ns")
FLOAT_METRICS = ("mean_ns",)
METRICS = INT_METRICS + FLOAT_METRICS


def read_rows(path):
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    comments, header, data = [], None, []
    for line in lines:
        if line.startswith("#"):
            comments.append(line)
        elif header is None:
            header = line
        elif line.strip():
            data.append(line)
    return comments, list(csv.DictReader([header] + data)) if header else []


def main(argv):
    if len(argv) != 2:
        print("usage: analyze-spsc-tail.py <results-dir>")
        return 1
    root = argv[1]
    sum_dir = os.path.join(root, "summaries")
    if not os.path.isdir(sum_dir):
        print("FATAL: %s is not a directory" % sum_dir)
        return 1

    # Refuse to produce doc tables from a dataset that has not been verified
    # raw-by-raw. A derived table is exactly the kind of artifact that gets
    # quoted without its provenance, so it must not exist for bad data.
    verify_log = os.path.join(root, "invariants.txt")
    if not os.path.isfile(verify_log):
        print("FATAL: %s is missing. Run the raw->summary verification first "
              "(scripts/verify-spsc-tail-summary.py) and save its output there. "
              "Refusing to derive tables from unverified data." % verify_log)
        return 1
    with open(verify_log) as f:
        text = f.read()
    if "ALL PHASE 4 RAW -> SUMMARY CHECKS PASSED" not in text:
        print("FATAL: %s does not record a passing verification. Refusing."
              % verify_log)
        return 1

    # cell -> session -> metric -> [per-repetition values]
    cells = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    ratios = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    npm = defaultdict(lambda: defaultdict(list))
    sessions_seen = defaultdict(set)

    files = sorted(f for f in os.listdir(sum_dir) if f.endswith(".csv"))
    if not files:
        print("FATAL: no summaries in %s" % sum_dir)
        return 1

    for name in files:
        _, rows = read_rows(os.path.join(sum_dir, name))
        for row in rows:
            if row.get("status") != "PASS" or row.get("correctness") != "PASS":
                print("FATAL: %s has a non-PASS row (repetition %s). Refusing to "
                      "summarize a dataset containing a failed repetition."
                      % (name, row.get("repetition")))
                return 1
            try:
                cell = "b%s_c%s" % (row["message_bytes"], row["capacity"])
                session = int(row["session"])
                rep = int(row["repetition"])
                vals = {m: int(row[m]) for m in INT_METRICS}
                vals.update({m: float(row[m]) for m in FLOAT_METRICS})
                msg_ns = float(row["ns_per_message"])
            except (KeyError, ValueError) as exc:
                print("FATAL: %s row is malformed (%s): %r" % (name, exc, row))
                return 1

            sessions_seen[cell].add(session)
            npm[cell][session].append(msg_ns)
            for m in METRICS:
                cells[cell][session][m].append(vals[m])

            p50 = vals["p50_ns"]
            if p50 <= 0:
                print("FATAL: %s rep %d has a non-positive p50_ns; tail ratios "
                      "are undefined." % (name, rep))
                return 1
            ratios[cell][session]["p99_over_p50"].append(vals["p99_ns"] / p50)
            ratios[cell][session]["p999_over_p50"].append(vals["p999_ns"] / p50)
            ratios[cell][session]["max_over_p50"].append(vals["max_ns"] / p50)

    ordered_cells = sorted(
        cells, key=lambda c: (int(c.split("_")[0][1:]), int(c.split("_")[1][1:])))

    # ------------------------------------------------------------- CELL_TAIL.csv
    with open(os.path.join(root, "CELL_TAIL.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["# Experiment 02 Phase 4 — DERIVED cross-repetition aggregate.",
                    ])
        w.writerow(["# Each value is the MEDIAN across the measured repetitions "
                    "of that repetition-level statistic."])
        w.writerow(["# NO distribution is pooled. See scripts/analyze-spsc-tail.py."])
        w.writerow(["cell", "session", "reps", "p50_ns", "p90_ns", "p99_ns",
                    "p999_ns", "max_ns", "mean_ns", "ns_per_message"])
        for cell in ordered_cells:
            for session in sorted(cells[cell]):
                rec = cells[cell][session]
                w.writerow(
                    [cell, session, len(rec["p50_ns"])] +
                    [int(statistics.median(rec[m])) for m in INT_METRICS] +
                    ["%.3f" % statistics.median(rec[m]) for m in FLOAT_METRICS] +
                    ["%.6f" % statistics.median(npm[cell][session])])

    # ----------------------------------------------------------- TAIL_RATIOS.csv
    with open(os.path.join(root, "TAIL_RATIOS.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["# Experiment 02 Phase 4 — DERIVED per-repetition tail "
                    "ratios (median across reps)."])
        w.writerow(["cell", "session", "p99_over_p50", "p999_over_p50",
                    "max_over_p50"])
        for cell in ordered_cells:
            for session in sorted(ratios[cell]):
                r = ratios[cell][session]
                w.writerow([cell, session] +
                           ["%.4f" % statistics.median(r[k]) for k in
                            ("p99_over_p50", "p999_over_p50", "max_over_p50")])

    # ------------------------------------------------------------ TAIL_MATRIX.md
    lines = []
    lines.append("# Experiment 02 Phase 4 — per-cell latency matrix")
    lines.append("")
    lines.append("**DERIVED** from `summaries/*.csv` by "
                 "`scripts/analyze-spsc-tail.py`. Not a measurement.")
    lines.append("")
    lines.append("Every figure is the **median across the 5 measured repetitions "
                 "of that repetition-level statistic**, then taken over the four "
                 "sessions' medians where a single per-cell number is shown.")
    lines.append("**No distribution is pooled across repetitions.** A \"P99\" "
                 "here is the P99 a typical repetition exhibited, not the pooled "
                 "99th percentile of every sample in the cell.")
    lines.append("")
    lines.append("## P50 / P99, per cell and session (ns)")
    lines.append("")
    lines.append("Each session cell is `P50 / P99`. The two spread columns are "
                 "max/min across the four session medians, kept separate because "
                 "P50 and P99 do not move together: a cell can have a stable tail "
                 "and a median that moves by 7x.")
    lines.append("")
    lines.append("| cell | s1 | s2 | s3 | s4 | P50 spread | P99 spread |")
    lines.append("|---|---|---|---|---|---|---|")
    for cell in ordered_cells:
        p50 = [int(statistics.median(cells[cell][s]["p50_ns"]))
               for s in sorted(cells[cell])]
        p99 = [int(statistics.median(cells[cell][s]["p99_ns"]))
               for s in sorted(cells[cell])]
        p50_spread = (max(p50) / min(p50)) if min(p50) > 0 else float("nan")
        p99_spread = (max(p99) / min(p99)) if min(p99) > 0 else float("nan")
        lines.append("| %s | %s | %.2fx | %.2fx |" % (
            cell.replace("b", "").replace("_c", " B / ") + " B",
            " | ".join("%d / %d" % (a, b) for a, b in zip(p50, p99)),
            p50_spread, p99_spread))
    lines.append("")
    lines.append("## Tail ratios (median across repetitions)")
    lines.append("")
    lines.append("| cell | session | P99/P50 | P99.9/P50 | max/P50 |")
    lines.append("|---|---|---|---|---|")
    for cell in ordered_cells:
        for session in sorted(ratios[cell]):
            r = ratios[cell][session]
            lines.append("| %s | %d | %.2f | %.2f | %.2f |" % (
                cell, session,
                statistics.median(r["p99_over_p50"]),
                statistics.median(r["p999_over_p50"]),
                statistics.median(r["max_over_p50"])))
    lines.append("")

    with open(os.path.join(root, "TAIL_MATRIX.md"), "w") as f:
        f.write("\n".join(lines))

    print("wrote CELL_TAIL.csv, TAIL_RATIOS.csv, TAIL_MATRIX.md into %s" % root)
    print("cells=%d cell-sessions=%d summary_files=%d" % (
        len(ordered_cells),
        sum(len(v) for v in sessions_seen.values()),
        len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
