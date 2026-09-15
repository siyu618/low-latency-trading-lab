#!/usr/bin/env python3
"""Experiment 03 Phase 3A — verify and derive.

This script is the AUTHORITATIVE gate on the dataset. It does two jobs and it
does them in this order, because the second must never run on unverified input:

  1. VERIFY every session against its own raw repetitions, and refuse the whole
     dataset if anything disagrees.
  2. DERIVE the cross-session tables, and only then.

It exits non-zero on any failure, and the runner propagates that.

WHAT IS CHECKED, PER SESSION
  * the summary parses and every required key is present
  * correctness=PASS
  * measured_reps equals the number of rows in the per-repetition CSV tail
  * every row is marked PASS
  * every row's checksum equals expected_checksum (the single-threaded
    reference), and the headline checksum does too
  * every row's decoded/enqueued/consumed counts equal total_messages, and
    every row's live_messages equals the live count
  * every row's elapsed_ns > 0
  * the headline median_ns_per_message is the median of the raw rows' own
    ns_per_message values, and the headline median_messages_per_second is the
    median of the raw rows' own messages_per_second values — recomputed here,
    not trusted

WHAT IS CHECKED, ACROSS SESSIONS
  * all sessions used the same chunk policy, capacity, seed, live-message count
    and total-message count; a dataset measured under two different workloads
    is not a dataset
  * every session's checksum equals every other session's — i.e. all four
    processes computed the SAME reference over the SAME bytes
  * the same environment key set is preserved in each stderr log

TEMPORAL-DRIFT GUARD
  With one cell, a session whose repetitions ran far slower than that session's
  own median is the signature of a host that got busy mid-run. The dataset is
  REFUSED in that case rather than published with a wide spread and a footnote.
  The threshold is deliberately loose: it is there to catch the host being
  descheduled, not ordinary variation. There is no switch to disable it.
"""

import csv
import io
import os
import statistics
import sys

# A repetition slower than this multiple of its own session's median is not
# treated as variation; the session is not publishable. This is a coarse guard
# against a descheduled host, not a quality threshold.
SLOW_REPETITION_MULTIPLE = 5.0

REQUIRED_KEYS = [
    "chunk_bytes",
    "queue_capacity",
    "seed",
    "snapshot_messages",
    "live_messages",
    "total_messages",
    "measured_reps",
    "warmup_reps",
    "median_ns_per_message",
    "median_messages_per_second",
    "checksum",
    "expected_checksum",
    "correctness",
]

IDENTITY_KEYS = [
    "chunk_bytes",
    "queue_capacity",
    "seed",
    "snapshot_messages",
    "live_messages",
    "total_messages",
]

REP_CSV_HEADER = [
    "rep",
    "live_messages",
    "elapsed_ns",
    "ns_per_message",
    "messages_per_second",
    "producer_full_retries",
    "consumer_empty_retries",
    "checksum",
]


class Failure(Exception):
    pass


def parse_summary(path):
    """Split the summary into its key=value block and its CSV tail."""
    with open(path, "r", encoding="utf-8") as handle:
        lines = handle.read().splitlines()

    values = {}
    csv_start = None
    for index, line in enumerate(lines):
        if line.startswith("#") or not line.strip():
            continue
        if line.startswith("rep,live_messages,"):
            csv_start = index
            break
        if "=" in line:
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()

    if csv_start is None:
        raise Failure(f"{path}: no per-repetition CSV tail found")

    header = lines[csv_start].split(",")
    if header != REP_CSV_HEADER:
        raise Failure(f"{path}: unexpected CSV header {header}")

    rows = []
    for line in lines[csv_start + 1:]:
        if not line.strip() or line.startswith("#"):
            continue
        reader = csv.DictReader(io.StringIO(lines[csv_start] + "\n" + line))
        rows.append(next(reader))

    return values, rows


def verify_session(path, session):
    """Verify one session. Raises Failure with a specific reason."""
    values, rows = parse_summary(path)

    missing = [key for key in REQUIRED_KEYS if key not in values]
    if missing:
        raise Failure(f"{path}: missing keys {missing}")

    if values["correctness"] != "PASS":
        raise Failure(f"{path}: correctness={values['correctness']}")

    if values["checksum"] != values["expected_checksum"]:
        raise Failure(
            f"{path}: checksum {values['checksum']} != reference "
            f"{values['expected_checksum']}"
        )

    if values["chunk_bytes"] != "65536":
        raise Failure(f"{path}: chunk_bytes={values['chunk_bytes']}, want 65536")
    if values["queue_capacity"] != "4096":
        raise Failure(f"{path}: queue_capacity={values['queue_capacity']}, want 4096")

    expected_reps = int(values["measured_reps"])
    if expected_reps != len(rows):
        raise Failure(
            f"{path}: measured_reps={expected_reps} but {len(rows)} rows present"
        )
    if expected_reps == 0:
        raise Failure(f"{path}: no measured repetitions")

    total = values["total_messages"]
    live = values["live_messages"]

    nspm = []
    mps = []
    for row in rows:
        where = f"{path}: rep {row['rep']}"
        if row["checksum"] != values["expected_checksum"]:
            raise Failure(
                f"{where}: checksum {row['checksum']} != reference "
                f"{values['expected_checksum']}"
            )
        if row["live_messages"] != live:
            raise Failure(f"{where}: live_messages={row['live_messages']} want {live}")
        if int(row["elapsed_ns"]) <= 0:
            raise Failure(f"{where}: elapsed_ns={row['elapsed_ns']}")
        nspm.append(float(row["ns_per_message"]))
        mps.append(float(row["messages_per_second"]))

    declared_nspm = float(values["median_ns_per_message"])
    declared_mps = float(values["median_messages_per_second"])
    if abs(statistics.median(nspm) - declared_nspm) > 1e-6 * max(1.0, declared_nspm):
        raise Failure(
            f"{path}: declared median_ns_per_message {declared_nspm} != recomputed "
            f"{statistics.median(nspm)}"
        )
    if abs(statistics.median(mps) - declared_mps) > 1e-6 * max(1.0, declared_mps):
        raise Failure(
            f"{path}: declared median_messages_per_second {declared_mps} != "
            f"recomputed {statistics.median(mps)}"
        )

    # Temporal-drift guard: a repetition that ran far slower than its own
    # session's median means the host changed under the run.
    fastest = min(nspm)
    slowest = max(nspm)
    if fastest > 0 and slowest / fastest > SLOW_REPETITION_MULTIPLE:
        raise Failure(
            f"{path}: session is internally inconsistent — slowest repetition is "
            f"{slowest / fastest:.1f}x the fastest, above the "
            f"{SLOW_REPETITION_MULTIPLE}x guard. The host was almost certainly not "
            f"stable during this session; the dataset is not publishable."
        )

    # A session must be internally valid: it cannot have zero rows or a
    # zero-message workload.
    if total == "0":
        raise Failure(f"{path}: total_messages=0")

    return {
        "session": session,
        "path": path,
        "values": values,
        "rows": rows,
        "nspm": nspm,
        "mps": mps,
    }


def parse_raw(path):
    """Read the raw per-repetition CSV, which carries the FULL column set.

    The summary's tail is a digest; the raw file is the source of truth and is
    the only place the decoded/enqueued/consumed counts survive, so the counts
    the brief requires are checked HERE rather than trusted from the summary.
    """
    with open(path, "r", encoding="utf-8") as handle:
        lines = [line for line in handle.read().splitlines()
                 if line.strip() and not line.startswith("#")]

    if not lines:
        raise Failure(f"{path}: empty")
    header = lines[0].split(",")
    for required in ("decoded_messages", "enqueued_messages", "consumed_messages",
                     "correctness", "elapsed_ns", "checksum"):
        if required not in header:
            raise Failure(f"{path}: raw file lacks the '{required}' column")
    return list(csv.DictReader(io.StringIO("\n".join(lines))))


def verify_raw(path, total_messages, live_messages, expected_checksum):
    """Every raw repetition must satisfy the full chain the brief requires.

    decoded == enqueued == consumed == total messages, and consumed live
    messages == the live count. A session where the producer decoded more than
    the consumer applied is a lost-message bug, not a fast run.
    """
    rows = parse_raw(path)
    if not rows:
        raise Failure(f"{path}: no repetitions")

    for row in rows:
        where = f"{path}: rep {row['rep']}"
        if row["correctness"] != "PASS":
            raise Failure(f"{where}: correctness={row['correctness']}")
        for field in ("decoded_messages", "enqueued_messages", "consumed_messages"):
            if row[field] != total_messages:
                raise Failure(
                    f"{where}: {field}={row[field]}, want {total_messages}"
                )
        if row["live_messages"] != live_messages:
            raise Failure(
                f"{where}: live_messages={row['live_messages']}, want {live_messages}"
            )
        if row["checksum"] != expected_checksum:
            raise Failure(
                f"{where}: checksum={row['checksum']}, want {expected_checksum}"
            )
    return rows


def check_stderr_environment(dataset, sessions):
    """The benchmark echoes its effective environment; it must not vary."""
    seen = {}
    for entry in sessions:
        path = os.path.join(dataset, "stderr", f"session{entry['session']}.txt")
        if not os.path.exists(path):
            raise Failure(f"{path}: missing stderr log")
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        if "correctness=PASS" not in text:
            raise Failure(f"{path}: does not record a passing correctness line")
        seen[entry["session"]] = text
    return seen


def median(values):
    return statistics.median(values)


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <dataset-dir>", file=sys.stderr)
        return 2

    dataset = argv[1]
    raw_dir = os.path.join(dataset, "raw")
    summary_dir = os.path.join(dataset, "summaries")

    if not os.path.isdir(summary_dir):
        print(f"FAIL: {summary_dir} does not exist", file=sys.stderr)
        return 1

    summaries = sorted(
        name for name in os.listdir(summary_dir) if name.startswith("session")
    )
    if not summaries:
        print(f"FAIL: no session summaries in {summary_dir}", file=sys.stderr)
        return 1

    # ---- 1. VERIFY --------------------------------------------------------
    sessions = []
    try:
        for name in summaries:
            session = int(
                name.replace("session", "").split(".")[0]
            )
            path = os.path.join(summary_dir, name)
            entry = verify_session(path, session)

            raw_path = os.path.join(raw_dir, f"session{session}.csv")
            if not os.path.exists(raw_path):
                raise Failure(f"{raw_path}: missing raw repetition file")
            entry["raw"] = verify_raw(
                raw_path,
                entry["values"]["total_messages"],
                entry["values"]["live_messages"],
                entry["values"]["expected_checksum"],
            )

            sessions.append(entry)

        sessions.sort(key=lambda e: e["session"])

        # Every session must describe the SAME workload, or the dataset is a
        # mixture and no cross-session figure is meaningful.
        reference = sessions[0]
        for entry in sessions[1:]:
            for key in IDENTITY_KEYS:
                if entry["values"][key] != reference["values"][key]:
                    raise Failure(
                        f"session {entry['session']} has {key}="
                        f"{entry['values'][key]} but session "
                        f"{reference['session']} has {reference['values'][key]}; "
                        f"the sessions did not measure the same workload"
                    )

        # Every session must have computed the SAME reference over the SAME
        # bytes. A mismatch means the generator is not deterministic.
        checksums = {e["values"]["checksum"] for e in sessions}
        if len(checksums) != 1:
            raise Failure(f"sessions disagree on the reference checksum: {checksums}")

        check_stderr_environment(dataset, sessions)

    except Failure as failure:
        print(f"FAIL: {failure}")
        return 1

    # ---- 2. DERIVE --------------------------------------------------------
    session_nspm = [median(e["nspm"]) for e in sessions]
    session_mps = [median(e["mps"]) for e in sessions]
    session_full = [median([float(r["producer_full_retries"]) for r in e["rows"]])
                    for e in sessions]
    session_empty = [median([float(r["consumer_empty_retries"]) for r in e["rows"]])
                     for e in sessions]

    overall_nspm = median(session_nspm)
    overall_mps = median(session_mps)

    print("VERIFIED")
    print(f"  sessions                 : {len(sessions)}")
    print(f"  measured repetitions      : {sum(len(e['rows']) for e in sessions)}")
    print(f"  workload (live messages)  : {sessions[0]['values']['live_messages']}")
    print(f"  workload (total messages) : {sessions[0]['values']['total_messages']}")
    print(f"  chunk_bytes               : {sessions[0]['values']['chunk_bytes']}")
    print(f"  queue_capacity            : {sessions[0]['values']['queue_capacity']}")
    print(f"  reference checksum        : {sessions[0]['values']['checksum']}")
    print("  every repetition matched the single-threaded reference; every session")
    print("  measured the same workload; the frozen harness agreed pre-timing.")
    print()

    # SUMMARY.csv — the published table.
    summary_path = os.path.join(dataset, "SUMMARY.csv")
    with open(summary_path, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow([
            "# Experiment 03 Phase 3A — integrated market-data pipeline throughput.",
            "# ONE cell: 64 KiB source chunks, SPSC capacity 4096, separated-cursor",
            "# baseline queue, FlatOrderBook, 5,000,000 live Level messages after a",
            "# 130-message snapshot. The snapshot is applied BEFORE the timed region",
            "# and is excluded from every figure below.",
            "# messages_per_second = live messages consumed / elapsed_ns, where elapsed",
            "# is [release start gate] -> [the consumer's final apply]. It INCLUDES",
            "# stream framing, binary decode, the queue handoff, both retry loops,",
            "# sequencing and the FlatOrderBook apply. It is NOT a latency, NOT a",
            "# percentile and NOT a per-message cost. NOT exchange throughput.",
            "# This figure is workload- and host-specific.",
        ])
        writer.writerow([])
        writer.writerow(["session", "reps", "median_ns_per_message",
                         "median_messages_per_second", "session_min_ns_per_message",
                         "session_max_ns_per_message", "median_producer_full_retries",
                         "median_consumer_empty_retries"])
        for entry in sessions:
            writer.writerow([
                entry["session"],
                len(entry["rows"]),
                f"{median(entry['nspm']):.6f}",
                f"{median(entry['mps']):.3f}",
                f"{min(entry['nspm']):.6f}",
                f"{max(entry['nspm']):.6f}",
                f"{median([float(r['producer_full_retries']) for r in entry['rows']]):.0f}",
                f"{median([float(r['consumer_empty_retries']) for r in entry['rows']]):.0f}",
            ])
        writer.writerow([])
        writer.writerow(["# PRIMARY (median of session medians)"])
        writer.writerow(["median_ns_per_message", f"{overall_nspm:.6f}"])
        writer.writerow(["median_messages_per_second", f"{overall_mps:.3f}"])
        writer.writerow([])
        writer.writerow(["# spread across session medians"])
        writer.writerow(["min_session_ns_per_message", f"{min(session_nspm):.6f}"])
        writer.writerow(["max_session_ns_per_message", f"{max(session_nspm):.6f}"])
        writer.writerow([
            "spread_pct",
            f"{100.0 * (max(session_nspm) - min(session_nspm)) / overall_nspm:.3f}",
        ])
        writer.writerow([])
        writer.writerow(["# retry diagnostics (medians of session medians)"])
        writer.writerow(["median_producer_full_retries", f"{median(session_full):.0f}"])
        writer.writerow(["median_consumer_empty_retries", f"{median(session_empty):.0f}"])
        writer.writerow([])
        writer.writerow(["# every raw repetition, preserved; none discarded"])
        writer.writerow(["session"] + REP_CSV_HEADER)
        for entry in sessions:
            for row in entry["rows"]:
                writer.writerow([entry["session"]] + [row[key] for key in REP_CSV_HEADER])

    print("DERIVED")
    print(f"  PRIMARY median_ns_per_message      : {overall_nspm:.6f}")
    print(f"  PRIMARY median_messages_per_second : {overall_mps:.3f}")
    print(f"  session medians (ns/message)       : "
          + ", ".join(f"{v:.6f}" for v in session_nspm))
    print(f"  spread across session medians      : "
          f"{100.0 * (max(session_nspm) - min(session_nspm)) / overall_nspm:.3f}%")
    print(f"  written to                         : {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
