#!/usr/bin/env bash
# Phase 4 post-run verification: prove that a run's summary.txt can be
# recomputed from ITS OWN raw_samples.csv.
#
# Recomputes, from the already-written raw CSV, the documented distribution
# metrics — sample count, mean, P50, P99, P99.9, max (plus min) — over FULL
# batches only, using the nearest-rank definition that orderbook_tail_bench
# documents, and requires summary.txt (from THE SAME invocation) to match within
# formatting tolerance (0.001 ns/update — the summary prints %.6f).
#
# It does NOT rerun the benchmark. If summary and raw came from two DIFFERENT
# runs, or a trailing partial batch leaked into the summary's distribution
# metrics, the recomputation disagrees and this script fails loudly (exit 1).
#
# Usage: verify-tail-summary.sh <raw_samples.csv> <summary.txt>
#
# Distribution rule mirrored here (must stay in lock-step with
# benchmark/tail_stats.h distribution_metrics()): the distribution is the rows
# whose batch_operations == batch_size; a trailing partial row (if present) is
# recorded in the raw CSV but excluded from every distribution metric. Mean is
# the sum of full-batch elapsed / N_full / batch_size; percentiles are
# nearest-rank: rank = ceil(q*N_full), the observed sample at that 1-based rank.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <raw_samples.csv> <summary.txt>" >&2
    exit 2
fi

raw="$1"
sum="$2"

[[ -f "$raw" ]] || { echo "verify-tail-summary: no raw samples file: $raw" >&2; exit 2; }
[[ -f "$sum" ]] || { echo "verify-tail-summary: no summary file: $sum" >&2; exit 2; }

# --- read the summary's claimed values -------------------------------------
get() { sed -n "s/^$1=//p" "$sum" | head -n1; }

bs="$(get batch_size)"
want_total="$(get total_samples)"
want_dist="$(get distribution_samples)"
want_partial_present="$(get partial_batch_present)"
want_partial_ops="$(get partial_batch_ops)"
want_mean="$(get mean_batch_normalized_ns_per_update)"
want_min="$(get min_batch_normalized_ns_per_update)"
want_p50="$(get p50_batch_normalized_ns_per_update)"
want_p99="$(get p99_batch_normalized_ns_per_update)"
want_p999="$(get p99_9_batch_normalized_ns_per_update)"
want_max="$(get max_batch_normalized_ns_per_update)"

if [[ -z "$bs" || -z "$want_dist" || -z "$want_total" ]]; then
    echo "verify-tail-summary: '$sum' is missing required keys (batch_size /" \
         "distribution_samples / total_samples); not a Phase 4 summary." >&2
    exit 2
fi

# --- extract the full-batch elapsed durations from the raw CSV --------------
full_elapsed="$(mktemp)"
full_sorted="$(mktemp)"
trap 'rm -f "$full_elapsed" "$full_sorted"' EXIT

# Pass 1 — row tallies. Data rows only (skip '#' comments and the
# column-header row). The distribution basis is rows with
# batch_operations == batch_size.
read -r data_total data_other data_other_ops < <(awk -F, -v bs="$bs" '
    /^#/       { next }
    $1 !~ /^[0-9]+$/ { next }   # column header "sample_index,..."
    { total++
      if ($2 == bs) { full++ }
      else          { other++; other_ops = $2 } }
    END { print total, other, (other ? other_ops : 0) }
' "$raw")

# Pass 2 — the elapsed ns of the FULL-batch rows (the distribution basis).
awk -F, -v bs="$bs" '
    /^#/       { next }
    $1 !~ /^[0-9]+$/ { next }
    $2 == bs { print $3 }
' "$raw" > "$full_elapsed"

n_full="$(wc -l < "$full_elapsed" | tr -d ' ')"

# --- recompute metrics from the full-batch basis ----------------------------
# Empty basis => nothing to verify (should not happen: updates >= batch_size).
if [[ "$n_full" -eq 0 ]]; then
    echo "verify-tail-summary: no full-batch rows (batch_operations == $bs)" \
         "found in '$raw'." >&2
    exit 2
fi

sort -n "$full_elapsed" > "$full_sorted"

metrics="$(awk -v n="$n_full" -v bs="$bs" '
    function ceil(x) { return int(x) + (x > int(x) ? 1 : 0) }
    { v[NR] = $1; s += $1 }
    END {
        printf "mean=%.6f\n", s / n / bs
        printf "min=%.6f\n",  v[1] / bs
        printf "p50=%.6f\n",  v[ceil(0.50 * n)] / bs
        printf "p99=%.6f\n",  v[ceil(0.99 * n)] / bs
        printf "p999=%.6f\n", v[ceil(0.999 * n)] / bs
        printf "max=%.6f\n",  v[n] / bs
    }
' "$full_sorted")"

re_mean="$(printf '%s\n' "$metrics" | sed -n 's/^mean=//p')"
re_min="$(printf '%s\n' "$metrics" | sed -n 's/^min=//p')"
re_p50="$(printf '%s\n' "$metrics" | sed -n 's/^p50=//p')"
re_p99="$(printf '%s\n' "$metrics" | sed -n 's/^p99=//p')"
re_p999="$(printf '%s\n' "$metrics" | sed -n 's/^p999=//p')"
re_max="$(printf '%s\n' "$metrics" | sed -n 's/^max=//p')"

# --- compare ----------------------------------------------------------------
fail=0

# Exact integer checks first: the row counts and the partial shape must agree.
if [[ "$data_total" != "$want_total" ]]; then
    echo "  FAIL  total_samples: raw rows=$data_total summary=$want_total" >&2
    fail=1
else
    printf '  ok    %-24s raw=%s summary=%s\n' 'total_samples' "$data_total" "$want_total"
fi

if [[ "$n_full" != "$want_dist" ]]; then
    echo "  FAIL  distribution_samples (full rows): raw=$n_full summary=$want_dist" >&2
    fail=1
else
    printf '  ok    %-24s raw=%s summary=%s\n' 'distribution_samples' "$n_full" "$want_dist"
fi

if [[ "$want_partial_present" == "1" ]]; then
    if [[ "$data_other" != "1" || "$data_other_ops" != "$want_partial_ops" ]]; then
        echo "  FAIL  partial row: raw has $data_other non-full row(s) (ops=$data_other_ops)," \
             "summary claims partial_batch_present=1 ops=$want_partial_ops" >&2
        fail=1
    else
        printf '  ok    %-24s raw=%s summary=%s\n' 'partial row' "1x$data_other_ops" "present ops=$want_partial_ops"
    fi
elif [[ "$data_other" != "0" ]]; then
    echo "  FAIL  summary says partial_batch_present=0 but the raw CSV has" \
         "$data_other non-full row(s)" >&2
    fail=1
fi

# Float checks: recomputed-from-raw vs summary, within 0.001 ns/update (the
# summary's %.6f formatting tolerance is far tighter; 0.001 is generous).
check() { # name want got
    local ok
    ok="$(awk -v w="$2" -v g="$3" 'BEGIN { d = (g > w) ? (g - w) : (w - g); print (d < 0.001) ? 1 : 0 }')"
    if [[ "$ok" == "1" ]]; then
        printf '  ok    %-24s raw=%-12s summary=%s\n' "$1" "$3" "$2"
    else
        printf '  FAIL  %-24s raw=%s summary=%s (recomputed from %s)\n' "$1" "$3" "$2" "$raw" >&2
        fail=1
    fi
}

check 'mean' "$want_mean" "$re_mean"
check 'min'  "$want_min"  "$re_min"
check 'p50'  "$want_p50"  "$re_p50"
check 'p99'  "$want_p99"  "$re_p99"
check 'p99.9' "$want_p999" "$re_p999"
check 'max'  "$want_max"  "$re_max"

if [[ "$fail" -ne 0 ]]; then
    echo "verify-tail-summary: FAILED — '$raw' and '$sum' do NOT agree." >&2
    exit 1
fi

echo "verify-tail-summary: PASS — raw samples reproduce the summary's distribution."
exit 0
