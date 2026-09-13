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

THE TWO LEVELS, AND WHICH ONE IS CANONICAL
------------------------------------------
The design is NESTED: a cell has 4 sessions, and a session has 5 measured
repetitions. That gives two different ways to state a per-cell number, and they
are NOT mathematically identical:

  PRIMARY (session-blocked), the originally intended hierarchy:
      repetition -> median of that session's 5 repetitions
                 -> median of the 4 session medians

  SECONDARY (pooled repetitions), a diagnostic only:
      repetition -> median across all 20 repetition-level statistics

The session-blocked figure is PRIMARY because it matches how the data was
collected: the 5 repetitions inside one session share a process, a thread
placement and a moment in time, so they are not 20 exchangeable observations.
Collapsing each session first keeps a session that behaved differently from
dominating the cell's summary.

For an ODD number of sessions the two coincide; for 4 they generally do not.
A median of medians is not a median of all values: it weights each session
equally rather than each repetition equally, so with 5 repetitions per session
it is a weighted median of the 20 values, not the ordinary one. Both are
reported, always labelled. Neither is ever presented as the other.

For every cell and percentile the PRIMARY table also carries the MIN and MAX
session median, so the spread across sessions is visible next to the location.

Outputs (into the results directory):
  CELL_TAIL.csv             LEVEL-1 aggregates: one row per (cell, session).
                            A row is a session, NOT a cell summary.
  CELL_SESSION_BLOCKED.csv  PRIMARY per-cell summary + min/max session median,
                            with the secondary all-20 figure beside it
  TAIL_MATRIX.md            the readable form of the same, PRIMARY first
  TAIL_RATIOS.csv           LEVEL-1 tail ratios, one row per (cell, session)

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
        w.writerow(["# Experiment 02 Phase 4 — DERIVED LEVEL-1 aggregate: one row "
                    "per (cell, session)."])
        w.writerow(["# Each value is the MEDIAN across that session's 5 measured "
                    "repetitions of that repetition-level statistic."])
        w.writerow(["# A ROW HERE IS A SESSION, NOT A CELL SUMMARY. This table has "
                    "no per-cell row."])
        w.writerow(["# The per-cell PRIMARY figure is the session-blocked median in "
                    "CELL_SESSION_BLOCKED.csv —"])
        w.writerow(["# repetition -> median of 5 -> median of the 4 session medians. "
                    "Quote that one, not a row of this."])
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
        w.writerow(["# Experiment 02 Phase 4 — DERIVED LEVEL-1 tail ratios: one row "
                    "per (cell, session)."])
        w.writerow(["# Each ratio is computed per repetition, then medianed across "
                    "that session's 5 measured repetitions."])
        w.writerow(["# NOT COMPARABLE ACROSS CELLS whose medians sit in different "
                    "bands: the ratio is driven by the"])
        w.writerow(["# denominator (how low P50 sits), not by the tail. Published for "
                    "completeness, never for ranking."])
        w.writerow(["# Cell-level PRIMARY percentiles are in "
                    "CELL_SESSION_BLOCKED.csv."])
        w.writerow(["cell", "session", "p99_over_p50", "p999_over_p50",
                    "max_over_p50"])
        for cell in ordered_cells:
            for session in sorted(ratios[cell]):
                r = ratios[cell][session]
                w.writerow([cell, session] +
                           ["%.4f" % statistics.median(r[k]) for k in
                            ("p99_over_p50", "p999_over_p50", "max_over_p50")])

    # ------------------------------------------------- CELL_SESSION_BLOCKED.csv
    #
    # The PRIMARY per-cell summary. For every cell and every metric it carries
    # the session-blocked median, the min and max session median (the spread the
    # blocked median sits inside), and the secondary all-repetitions median.
    blocked_rows = {}
    for cell in ordered_cells:
        per_session = {s: {m: statistics.median(cells[cell][s][m])
                           for m in METRICS}
                       for s in sorted(cells[cell])}
        for m in METRICS:
            s_vals = [per_session[s][m] for s in sorted(per_session)]
            all_vals = [v for s in sorted(cells[cell])
                        for v in cells[cell][s][m]]
            blocked_rows[(cell, m)] = {
                "n_sessions": len(s_vals),
                "n_reps": len(all_vals),
                "blocked": statistics.median(s_vals),
                "min_session": min(s_vals),
                "max_session": max(s_vals),
                "session_spread": (max(s_vals) / min(s_vals)
                                   if min(s_vals) > 0 else float("nan")),
                "secondary_all20": statistics.median(all_vals),
                "all20_spread": (max(all_vals) / min(all_vals)
                                 if min(all_vals) > 0 else float("nan")),
            }

    with open(os.path.join(root, "CELL_SESSION_BLOCKED.csv"), "w",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["# Experiment 02 Phase 4 — DERIVED, PRIMARY per-cell summary."])
        w.writerow(["# blocked_median = median of the 4 SESSION medians "
                    "(repetition -> median of 5 -> median of 4). THIS IS THE "
                    "PRIMARY FIGURE."])
        w.writerow(["# min_session_median / max_session_median bound the spread "
                    "the blocked median sits inside."])
        w.writerow(["# secondary_all20_median = median across all 20 "
                    "repetition-level statistics. DIAGNOSTIC ONLY: it weights "
                    "repetitions equally where the blocked median weights "
                    "sessions equally, so the two are NOT the same quantity and "
                    "must never be presented as one."])
        w.writerow(["# NO distribution is pooled. See scripts/analyze-spsc-tail.py."])
        w.writerow(["cell", "metric", "n_sessions", "n_reps", "blocked_median",
                    "min_session_median", "max_session_median", "session_spread",
                    "secondary_all20_median", "all20_spread"])
        for cell in ordered_cells:
            for m in INT_METRICS:
                b = blocked_rows[(cell, m)]
                w.writerow([cell, m, b["n_sessions"], b["n_reps"],
                            int(b["blocked"]), int(b["min_session"]),
                            int(b["max_session"]), "%.4f" % b["session_spread"],
                            int(b["secondary_all20"]),
                            "%.4f" % b["all20_spread"]])
            b = blocked_rows[(cell, "mean_ns")]
            w.writerow([cell, "mean_ns", b["n_sessions"], b["n_reps"],
                        "%.3f" % b["blocked"], "%.3f" % b["min_session"],
                        "%.3f" % b["max_session"], "%.4f" % b["session_spread"],
                        "%.3f" % b["secondary_all20"],
                        "%.4f" % b["all20_spread"]])

    # ------------------------------------------------------------ TAIL_MATRIX.md
    def pretty(cell):
        return cell.replace("b", "", 1).replace("_c", " B / ") + " B"

    LABEL = {"p50_ns": "P50", "p90_ns": "P90", "p99_ns": "P99",
             "p999_ns": "P99.9", "max_ns": "max"}

    lines = []
    lines.append("# Experiment 02 Phase 4 — per-cell latency matrix")
    lines.append("")
    lines.append("**DERIVED** from `summaries/*.csv` by "
                 "`scripts/analyze-spsc-tail.py`. Not a measurement.")
    lines.append("")
    lines.append("**No distribution is pooled across repetitions.** A \"P99\" "
                 "here is the P99 a typical repetition exhibited, not the pooled "
                 "99th percentile of every sample in the cell.")
    lines.append("")
    lines.append("## How to read this file: two levels, one of them canonical")
    lines.append("")
    lines.append("The design is **nested** — each cell has 4 sessions, each "
                 "session has 5 measured repetitions — so \"the cell's P99\" can "
                 "be defined two ways, and they give different numbers:")
    lines.append("")
    lines.append("* **PRIMARY — session-blocked.** repetition → median of that "
                 "session's 5 repetitions → **median of the 4 session medians**. "
                 "This is the figure to quote.")
    lines.append("* **SECONDARY — all repetitions.** repetition → median across "
                 "all 20 repetition-level statistics. A **diagnostic only**.")
    lines.append("")
    lines.append("These are **not mathematically identical.** A median of medians "
                 "is not a median of all values: the blocked median weights each "
                 "*session* equally, the all-20 median weights each *repetition* "
                 "equally — with 5 repetitions per session the blocked figure is "
                 "a weighted median of the same 20 numbers. For an odd number of "
                 "sessions the two coincide; for 4 they generally do not. Where "
                 "they differ, **the PRIMARY column is the cell's value** and the "
                 "SECONDARY column is context, never a competing headline.")
    lines.append("")
    lines.append("The session-blocked level is primary because of how the data "
                 "was collected: the 5 repetitions inside one session share a "
                 "process, a thread placement and a moment in time, so the 20 "
                 "repetitions of a cell are not 20 exchangeable observations. "
                 "Collapsing each session first stops a single session that "
                 "behaved differently from dominating the cell's summary.")
    lines.append("")
    lines.append("## PRIMARY — session-blocked, per cell (ns)")
    lines.append("")
    lines.append("`min` / `max` are the **minimum and maximum session median** for "
                 "that metric — the spread the blocked median sits inside, and "
                 "the first thing to look at before quoting any single number. "
                 "`spread` is `max/min` across the four session medians.")
    lines.append("")
    lines.append("| cell | metric | blocked | min session | max session | spread |")
    lines.append("|---|---|---|---|---|---|")
    for cell in ordered_cells:
        for m in INT_METRICS:
            b = blocked_rows[(cell, m)]
            lines.append("| %s | %s | %d | %d | %d | %.2fx |" % (
                pretty(cell), LABEL[m],
                int(b["blocked"]), int(b["min_session"]),
                int(b["max_session"]), b["session_spread"]))
    lines.append("")
    lines.append("## SECONDARY (diagnostic) — all-20 median beside the blocked one")
    lines.append("")
    lines.append("Shown **only** so the size of the aggregation choice is visible. "
                 "It is not an alternative headline. `delta` is the all-20 median "
                 "as a percentage of the blocked median.")
    lines.append("")
    lines.append("| cell | metric | blocked (PRIMARY) | all-20 (secondary) | delta |")
    lines.append("|---|---|---|---|---|")
    for cell in ordered_cells:
        for m in INT_METRICS:
            b = blocked_rows[(cell, m)]
            delta = (100.0 * (b["secondary_all20"] - b["blocked"]) / b["blocked"]
                     if b["blocked"] else float("nan"))
            lines.append("| %s | %s | %d | %d | %+.1f%% |" % (
                pretty(cell), LABEL[m],
                int(b["blocked"]), int(b["secondary_all20"]), delta))
    lines.append("")
    lines.append("## P50 / P99, per cell and session (ns)")
    lines.append("")
    lines.append("Each session cell is `P50 / P99`, a median across that session's "
                 "5 repetitions. The two spread columns are max/min across the "
                 "four session medians, kept separate because P50 and P99 do not "
                 "move together: a cell can have a stable tail and a median that "
                 "moves by several-fold.")
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
            pretty(cell),
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

    print("wrote CELL_TAIL.csv, CELL_SESSION_BLOCKED.csv, TAIL_RATIOS.csv, "
          "TAIL_MATRIX.md into %s" % root)
    print("cells=%d cell-sessions=%d summary_files=%d" % (
        len(ordered_cells),
        sum(len(v) for v in sessions_seen.values()),
        len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
