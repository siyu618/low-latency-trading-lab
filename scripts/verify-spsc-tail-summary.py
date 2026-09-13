#!/usr/bin/env python3
"""Experiment 02 Phase 4 — raw -> summary verifier.

Recomputes EVERY measured repetition's statistics from its RAW samples and checks
them against the summary the benchmark wrote, then checks whole-dataset
invariants that no single file can establish. Nothing here trusts the summary:
the raw per-sample rows are the source of truth, and every summary value is
treated as a claim that can fail.

What is checked
---------------
Per repetition, recomputed from raw and compared exactly to the summary row:
  count, min, mean, p50, p90, p99, p99.9, max   (all in ns)

Per repetition, structural:
  * raw header and summary header agree on
    session / message_bytes / capacity / messages / settling_prefix /
    sample_interval
  * the file name encodes its own cell and session
  * expected sample count == (messages - settling) // interval
  * observed sample count == expected — a short repetition is a FAILURE, never a
    smaller dataset
  * sample_index is 0..N-1 exactly, in order, with no gaps and no repeats
  * latency_ticks >= 0 and latency_ns == latency_ticks * ns_per_tick
  * status == PASS, correctness == PASS, sequence_ok == 1, delivered == messages
  * every raw row agrees with its own file header
  * max_ns <= elapsed_ns — a sample cannot outlive the repetition that produced it
  * ns_per_message == elapsed_ns / messages (within its own printed precision)
  * retry counters are non-negative and finite

Cross-file (the checks a single file cannot make):
  * the checksum is a deterministic fold of messages 1..N, so every repetition of
    every cell of one message size must produce the SAME checksum. A dropped,
    duplicated or reordered message changes it.
  * 4 sessions x 9 cells = 36 processes, each cell in all four sessions,
    raw and summary file sets identical with no orphans on either side
  * calibration files state that they are NOT a correction factor, and their
    declared sample count matches their rows

Percentiles use the SAME nearest-rank rule as the benchmark:
    index(p) = ceil(p * N) - 1, over ascending-sorted observed values.
They are recomputed on the RAW TICK values and converted afterwards, which is
exact because tick -> ns is multiplication by a positive integer constant.

Usage: scripts/verify-spsc-tail-summary.py <results-dir> [--allow-partial]
Exit: 0 = every check passed, 1 = at least one check failed.
"""

import csv
import math
import os
import re
import sys
from collections import defaultdict

FAILURES = []
WARNINGS = []
CHECKS = [0]


def check(cond, msg):
    CHECKS[0] += 1
    if not cond:
        FAILURES.append(msg)
    return cond


def warn(cond, msg):
    """A DIAGNOSTIC, not a verdict.

    Phase 4 studies latency and jitter, so a legitimately rare stall must not be
    silently censored for being extreme. Magnitude alone is not proof that a run
    is invalid. Warnings are reported prominently and count toward nothing.
    """
    CHECKS[0] += 1
    if not cond:
        WARNINGS.append(msg)
    return cond


def read_csv_with_header_comments(path):
    """Return (list_of_comment_lines, list_of_dict_rows).

    Comment lines start with '#'. The column header is the first non-comment
    line; the benchmark writes it immediately before the data.
    """
    with open(path, newline="") as f:
        lines = f.read().splitlines()

    comments, header, data_lines = [], None, []
    for line in lines:
        if line.startswith("#"):
            comments.append(line)
        elif header is None:
            header = line
        elif line.strip():
            data_lines.append(line)

    rows = list(csv.DictReader([header] + data_lines)) if header else []
    return comments, rows


def header_field(comments, key):
    """Pull `key=value` out of the '#' comment preamble, or None."""
    pattern = re.compile(r"(?:^#\s*|\s)" + re.escape(key) + r"=(\S+)")
    for line in comments:
        m = pattern.search(line)
        if m:
            return m.group(1)
    return None


def nearest_rank(sorted_values, q):
    """index(p) = ceil(p * N) - 1, over observed values only."""
    n = len(sorted_values)
    rank = math.ceil(q * n)
    return sorted_values[rank - 1]


def summarize(sorted_ticks):
    n = len(sorted_ticks)
    return {
        "count": n,
        "min": sorted_ticks[0],
        "max": sorted_ticks[-1],
        "mean": sum(sorted_ticks) / n,
        "p50": nearest_rank(sorted_ticks, 0.50),
        "p90": nearest_rank(sorted_ticks, 0.90),
        "p99": nearest_rank(sorted_ticks, 0.99),
        "p999": nearest_rank(sorted_ticks, 0.999),
    }


def as_int(row, key):
    try:
        return int(row[key])
    except (KeyError, ValueError, TypeError):
        return None


def main(argv):
    if len(argv) < 2:
        print("usage: verify-spsc-tail-summary.py <results-dir> [--allow-partial]")
        return 1
    root = argv[1]
    allow_partial = "--allow-partial" in argv[2:]

    raw_dir = os.path.join(root, "raw")
    sum_dir = os.path.join(root, "summaries")
    cal_dir = os.path.join(root, "calibration")

    for d in (raw_dir, sum_dir):
        if not os.path.isdir(d):
            print("FATAL: %s is not a directory" % d)
            return 1

    raw_basenames = {f[:-4] for f in os.listdir(raw_dir) if f.endswith(".csv")}
    sum_basenames = {f[:-4] for f in os.listdir(sum_dir) if f.endswith(".csv")}

    print("Phase 4 raw -> summary verification")
    print("  results dir  : %s" % root)
    print("  raw files    : %d" % len(raw_basenames))
    print("  summary files: %d" % len(sum_basenames))
    print()

    check(raw_basenames == sum_basenames,
          "raw/summary file sets differ: only-raw=%s only-summary=%s" % (
              sorted(raw_basenames - sum_basenames),
              sorted(sum_basenames - raw_basenames)))

    coverage = defaultdict(set)          # session -> {cell}
    checksums = defaultdict(set)         # message_bytes -> {checksum}
    npm_by_cell = defaultdict(list)      # (bytes, cap) -> [(file, rep, ns/msg)]
    total_samples = 0
    verified_repetitions = 0

    # ------------------------------------------------------------------ cells
    for basename in sorted(raw_basenames & sum_basenames):
        raw_path = os.path.join(raw_dir, basename + ".csv")
        sum_path = os.path.join(sum_dir, basename + ".csv")
        raw_comments, raw_rows = read_csv_with_header_comments(raw_path)
        sum_comments, sum_rows = read_csv_with_header_comments(sum_path)

        # ---- header agreement -------------------------------------------
        header_keys = ("session", "message_bytes", "capacity", "messages",
                       "settling_prefix", "sample_interval")
        fields = {}
        for key in header_keys:
            a = header_field(raw_comments, key)
            b = header_field(sum_comments, key)
            fields[key] = b
            check(a is not None and b is not None and a == b,
                  "%s: raw/summary header disagree on %s (%r vs %r)" % (
                      basename, key, a, b))

        if any(fields[k] is None for k in header_keys):
            continue

        session = int(fields["session"])
        message_bytes = int(fields["message_bytes"])
        capacity = int(fields["capacity"])
        messages = int(fields["messages"])
        settling = int(fields["settling_prefix"])
        interval = int(fields["sample_interval"])

        cell = "b%d_c%d" % (message_bytes, capacity)
        check(basename == "%s_s%d" % (cell, session),
              "%s: file name does not encode its own cell/session (%s, session "
              "%d)" % (basename, cell, session))
        coverage[session].add(cell)

        check(interval > 0 and interval % 2 == 1,
              "%s: sample_interval %d is not a positive odd number" % (
                  basename, interval))
        check(0 < settling < messages,
              "%s: settling_prefix %d is not inside (0, messages=%d)" % (
                  basename, settling, messages))

        expected_samples = (messages - settling) // interval
        check(expected_samples > 0,
              "%s: derived zero samples (messages=%d settling=%d interval=%d)"
              % (basename, messages, settling, interval))

        # ns_per_tick: the raw preamble states it as "latency_ns is ticks * N".
        per_tick = header_field(raw_comments, "ns_per_tick")
        if per_tick is None:
            m = re.search(r"ticks \* (\d+)", "\n".join(raw_comments))
            per_tick = m.group(1) if m else None
        if per_tick is None:
            # Silently assuming 1 would make every ns comparison vacuous.
            check(False, "%s: cannot determine ns_per_tick from the raw "
                         "preamble" % basename)
            continue
        per_tick = int(per_tick)
        check(per_tick > 0, "%s: ns_per_tick is not positive" % basename)

        # ---- raw rows, grouped by repetition -----------------------------
        by_rep = defaultdict(list)
        index_seq = defaultdict(list)
        parses_ok = True
        for row in raw_rows:
            rep = as_int(row, "repetition")
            idx = as_int(row, "sample_index")
            ticks = as_int(row, "latency_ticks")
            ns = as_int(row, "latency_ns")
            row_bytes = as_int(row, "message_bytes")
            row_cap = as_int(row, "capacity")
            row_session = as_int(row, "session")

            if None in (rep, idx, ticks, ns, row_bytes, row_cap, row_session):
                check(False, "%s: unparsable raw row %r" % (basename, row))
                parses_ok = False
                continue

            check(row_bytes == message_bytes and row_cap == capacity and
                  row_session == session,
                  "%s: raw row disagrees with its own header: %r" % (basename, row))
            check(ticks >= 0,
                  "%s rep %d idx %d: negative latency_ticks (%d)" % (
                      basename, rep, idx, ticks))
            check(ns == ticks * per_tick,
                  "%s rep %d idx %d: latency_ns %d != latency_ticks %d * %d" % (
                      basename, rep, idx, ns, ticks, per_tick))
            by_rep[rep].append(ticks)
            index_seq[rep].append(idx)

        if not parses_ok:
            continue

        for rep in sorted(by_rep):
            idxs = index_seq[rep]
            total_samples += len(idxs)
            check(idxs == list(range(len(idxs))),
                  "%s rep %d: sample_index is not 0..N-1 in order (first "
                  "mismatch at position %r)" % (
                      basename, rep,
                      next((i for i, v in enumerate(idxs) if i != v), None)))

        # ---- summary rows -------------------------------------------------
        summary_by_rep = {}
        for row in sum_rows:
            rep = as_int(row, "repetition")
            if rep is None:
                check(False, "%s: unparsable summary row %r" % (basename, row))
                continue
            check(rep not in summary_by_rep,
                  "%s: duplicate summary row for repetition %d" % (basename, rep))
            summary_by_rep[rep] = row

        check(set(summary_by_rep) == set(by_rep),
              "%s: raw repetitions %s != summary repetitions %s" % (
                  basename, sorted(by_rep), sorted(summary_by_rep)))

        for rep in sorted(set(summary_by_rep) & set(by_rep)):
            row = summary_by_rep[rep]
            ticks = sorted(by_rep[rep])
            got = summarize(ticks)
            verified_repetitions += 1

            check(row.get("status") == "PASS",
                  "%s rep %d: status is %r" % (basename, rep, row.get("status")))
            check(row.get("correctness") == "PASS",
                  "%s rep %d: correctness is %r" % (
                      basename, rep, row.get("correctness")))
            check(as_int(row, "sequence_ok") == 1,
                  "%s rep %d: sequence_ok is %r" % (
                      basename, rep, row.get("sequence_ok")))
            check(as_int(row, "delivered") == messages,
                  "%s rep %d: delivered %r != messages %d" % (
                      basename, rep, row.get("delivered"), messages))
            check(as_int(row, "expected_samples") == expected_samples,
                  "%s rep %d: expected_samples %r != derived %d" % (
                      basename, rep, row.get("expected_samples"),
                      expected_samples))
            check(as_int(row, "sample_count") == got["count"],
                  "%s rep %d: sample_count %r != raw rows %d" % (
                      basename, rep, row.get("sample_count"), got["count"]))
            check(got["count"] == expected_samples,
                  "%s rep %d: raw sample count %d != derived %d (a short "
                  "repetition is a FAILURE, not a smaller dataset)" % (
                      basename, rep, got["count"], expected_samples))

            # ------------------------------- sparse instrumentation (§4, §8)
            #
            # THE PHASE 4.1 GATE. These counters are taken at the call site in
            # the benchmark, so they report what the instrumentation ACTUALLY
            # DID. They must each equal the derived sample count: one clock read
            # per sampled message, not one per message.
            #
            # A harness that timestamped every message would put `messages`
            # (~10,000,000) in these columns while still recording ~9,696
            # latencies — the same visible output from a completely different
            # measurement. This check is what makes that impossible to ship.
            for col in ("producer_sample_clock_reads",
                        "consumer_sample_clock_reads"):
                have = as_int(row, col)
                check(have == expected_samples,
                      "%s rep %d: %s is %r, expected %d. The timestamp "
                      "instrumentation is NOT sparse — a clock read was taken "
                      "for messages that were never sampled." % (
                          basename, rep, col, row.get(col),
                          expected_samples))
            check(as_int(row, "stamp_contract_failures") == 0,
                  "%s rep %d: %r messages disagreed with the sample schedule "
                  "(a producer/consumer schedule disagreement is a CORRECTNESS "
                  "failure)" % (basename, rep, row.get("stamp_contract_failures")))

            for col, key in (("min_ns", "min"), ("p50_ns", "p50"),
                             ("p90_ns", "p90"), ("p99_ns", "p99"),
                             ("p999_ns", "p999"), ("max_ns", "max")):
                want = got[key] * per_tick
                have = as_int(row, col)
                check(have == want,
                      "%s rep %d: %s summary=%r recomputed=%d" % (
                          basename, rep, col, row.get(col), want))

            want_mean = got["mean"] * per_tick
            have_mean = row.get("mean_ns")
            try:
                mean_ok = abs(float(have_mean) - want_mean) <= 1e-3
            except (TypeError, ValueError):
                mean_ok = False
            check(mean_ok,
                  "%s rep %d: mean_ns summary=%r recomputed=%.4f" % (
                      basename, rep, have_mean, want_mean))

            # ---- internal consistency of the measured repetition ---------
            elapsed = as_int(row, "elapsed_ns")
            check(elapsed is not None and elapsed > 0,
                  "%s rep %d: elapsed_ns %r is not positive" % (
                      basename, rep, row.get("elapsed_ns")))
            if elapsed:
                check(got["max"] * per_tick <= elapsed,
                      "%s rep %d: max_ns %d exceeds the repetition's own "
                      "elapsed_ns %d — a sample cannot outlive the repetition "
                      "that produced it" % (
                          basename, rep, got["max"] * per_tick, elapsed))
                npm = row.get("ns_per_message")
                try:
                    want_npm = elapsed / messages
                    check(abs(float(npm) - want_npm) <= max(1e-6, want_npm * 1e-5),
                          "%s rep %d: ns_per_message %r != elapsed_ns/messages "
                          "%.6f" % (basename, rep, npm, want_npm))
                except (TypeError, ValueError):
                    check(False, "%s rep %d: ns_per_message %r is not a number" % (
                        basename, rep, npm))

            for col in ("producer_full_retries", "consumer_empty_retries"):
                v = as_int(row, col)
                check(v is not None and v >= 0,
                      "%s rep %d: %s is %r" % (basename, rep, col, row.get(col)))

            npm = as_int(row, "ns_per_message")
            try:
                npm_val = float(row["ns_per_message"])
            except (KeyError, TypeError, ValueError):
                npm_val = None
            if npm_val is None:
                check(False, "%s rep %d: ns_per_message %r is not a number" % (
                    basename, rep, row.get("ns_per_message")))
            else:
                npm_by_cell[(message_bytes, capacity)].append(
                    (basename, rep, npm_val))

            cs = as_int(row, "checksum")
            if cs is None:
                check(False, "%s rep %d: checksum %r is not an integer" % (
                    basename, rep, row.get("checksum")))
            else:
                checksums[message_bytes].add(cs)

    # ------------------------------------------------------------- checksums
    # The checksum folds messages 1..N in sequence, so it is a pure function of
    # the message shape and N — identical for every repetition of every cell of
    # one size. A divergence means a message was dropped, duplicated or
    # reordered, or a payload was built differently.
    for message_bytes in sorted(checksums):
        found = checksums[message_bytes]
        check(len(found) == 1,
              "%d-byte cells produced %d distinct checksums across repetitions "
              "(%s) — the fold is deterministic, so this is a correctness "
              "failure, not noise" % (message_bytes, len(found),
                                      sorted(found)[:6]))
        check(found != {0}, "%d-byte cells produced an all-zero checksum" %
              message_bytes)

    # ------------------------------------------- host-interference DIAGNOSTIC
    #
    # THIS IS A WARNING, NOT A VERDICT. It was an automatic dataset failure
    # before Phase 4.1 and that was wrong for this phase.
    #
    # Phase 4 studies LATENCY AND JITTER, so a rare, extreme repetition is part
    # of the phenomenon being measured, not automatically an artifact. A run
    # whose tail was legitimately produced by a scheduler stall would have been
    # censored by a hard 5x rule — the most interesting observation in the
    # dataset deleted by a threshold. Magnitude alone is therefore NOT proof of
    # invalidity and must not invalidate a run.
    #
    # What DOES justify rejecting or archiving an entire run is INDEPENDENT
    # evidence of contamination: sanitizer benchmarks running concurrently, a
    # known competing workload, overlapping benchmark processes, or explicitly
    # observed host interference. None of that is visible in these files, which
    # is precisely why this check cannot make the call. The pre-flight host-load
    # metadata is kept for that purpose.
    #
    # So: report extreme repetitions prominently, name every one of them, and
    # leave the judgement to whoever has the independent evidence.
    #
    # ns_per_message is elapsed / messages delivered, an end-to-end rate that is
    # reproducible for a given cell (a few percent between clean repetitions)
    # precisely because it is not a latency. That reproducibility is what makes
    # it a usable detector — a repetition far above its own cell's median rate
    # did not run slower because of anything the queue did.
    NS_PER_MESSAGE_REPORT_FACTOR = 5.0
    for key in sorted(npm_by_cell, key=lambda k: (k[0], k[1])):
        entries = npm_by_cell[key]
        values = sorted(v for _, _, v in entries)
        med = values[len(values) // 2]
        check(med > 0, "%dB / %dB: median ns_per_message is %r" % (
            key[0], key[1], med))
        if med <= 0:
            continue
        outliers = [(f, r, v) for f, r, v in entries
                    if v > med * NS_PER_MESSAGE_REPORT_FACTOR]
        warn(not outliers,
             "%dB / %dB: %d of %d repetitions ran at more than %.0fx the cell's "
             "median ns_per_message (median %.2f ns, worst %.2f ns = %.1fx). "
             "CONTAMINATION CANDIDATE, not a failed gate: a run is only invalid "
             "if there is INDEPENDENT evidence of host interference (concurrent "
             "sanitizer or benchmark processes, a known competing workload, "
             "observed interference). If there is none, these repetitions are "
             "real tail observations and belong in the results. Worst: %s." % (
                 key[0], key[1], len(outliers), len(entries),
                 NS_PER_MESSAGE_REPORT_FACTOR, med,
                 max(v for _, _, v in outliers) if outliers else 0.0,
                 (max(v for _, _, v in outliers) / med) if outliers else 0.0,
                 ", ".join("%s rep %d at %.2f ns" % (f, r, v)
                           for f, r, v in sorted(outliers,
                                                 key=lambda t: -t[2])[:3])))

    # ---------------------------------------------------------- calibration
    if os.path.isdir(cal_dir):
        cal_files = sorted(f for f in os.listdir(cal_dir) if f.endswith(".csv"))
        for name in cal_files:
            comments, rows = read_csv_with_header_comments(
                os.path.join(cal_dir, name))
            check(len(rows) > 0, "calibration %s: no samples" % name)

            declared = header_field(comments, "samples")
            check(declared is not None and declared == str(len(rows)),
                  "calibration %s: header declares samples=%r but the file has "
                  "%d rows" % (name, declared, len(rows)))

            values = []
            for row in rows:
                v = as_int(row, "pair_cost_ns")
                if v is None:
                    check(False, "calibration %s: unparsable row %r" % (name, row))
                else:
                    values.append(v)
            if values:
                check(min(values) >= 0,
                      "calibration %s: negative pair cost %d" % (
                          name, min(values)))

            # The calibration must never be folded into a latency. This asserts
            # the file still says so, so a future edit that starts subtracting
            # it fails here.
            text = "\n".join(comments).lower()
            check("not subtracted" in text,
                  "calibration %s: does not state that it is not subtracted" % name)
            check("not a correction factor" in text or
                  "no correction factor" in text,
                  "calibration %s: does not state that it is not a correction "
                  "factor" % name)

    # ------------------------------------------------------- dataset coverage
    if allow_partial:
        print("  (partial mode: dataset-coverage checks skipped)")
    else:
        check(len(coverage) == 4,
              "expected 4 sessions, found %s" % sorted(coverage))
        for session in sorted(coverage):
            check(len(coverage[session]) == 9,
                  "session %d has %d cells, expected 9: %s" % (
                      session, len(coverage[session]), sorted(coverage[session])))
        check(len(raw_basenames) == 36,
              "expected 36 process files (4 sessions x 9 cells), found %d" % (
                  len(raw_basenames)))
        check(verified_repetitions > 0,
              "no measured repetitions were verified at all")

    # -------------------------------------------------------------- report
    print("  verified repetitions : %d" % verified_repetitions)
    print("  sampled latencies    : %d" % total_samples)
    print("  checks run           : %d" % CHECKS[0])
    print()

    # WARNINGS ARE REPORTED FIRST, AND THEY DO NOT FAIL THE RUN.
    #
    # They are diagnostics about the HOST, not about the queue, and Phase 4
    # measures jitter — so an extreme repetition is a result to look at, not a
    # gate to trip. A run is rejected only on independent evidence of
    # contamination, which these files cannot contain.
    if WARNINGS:
        print("WARNING: %d diagnostic(s) - these do NOT invalidate the "
              "dataset:" % len(WARNINGS))
        for msg in WARNINGS:
            print("  ! %s" % msg)
        print()

    if FAILURES:
        print("FAILED: %d check(s)" % len(FAILURES))
        for msg in FAILURES[:40]:
            print("  - %s" % msg)
        if len(FAILURES) > 40:
            print("  ... and %d more" % (len(FAILURES) - 40))
        return 1

    print("ALL PHASE 4 RAW -> SUMMARY CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
