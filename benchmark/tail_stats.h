// Phase 4 tail-latency statistics — shared between the tail benchmark
// (order_book_tail_bench.cpp) and its lightweight unit tests
// (tests/phase4_stats_tests.cpp).
//
// Everything here is POST-measurement analysis over already-collected raw
// integer batch durations. Percentiles are computed on the OBSERVED distribution
// (never interpolated) by the precise rule below; raw values are never rounded
// before these functions run.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace llob_tail {

// ---------------------------------------------------------------------------
// Nearest-rank percentile over an ascending-sorted vector of observed samples.
//
// For a fraction q in (0, 1]: rank = ceil(q * N); the result is the observed
// sample at that 1-based rank (index rank-1), i.e. the smallest sample that is
// >= the fraction q of the population. P50 of N samples is the ceil(N/2)-th
// smallest; P100 (q = 1) is the maximum. Reproducible and precise: it returns a
// value a batch actually took and never interpolates to a latency no sample had.
// ---------------------------------------------------------------------------
template <typename T>
T percentile_rank(const std::vector<T>& sorted, double q) {
    if (sorted.empty()) return T{};
    const size_t rank = static_cast<size_t>(
        std::ceil(q * static_cast<double>(sorted.size())));
    return sorted[rank - 1];
}

// Return an ascending-sorted copy of the raw samples (raw source stays intact).
inline std::vector<int64_t> sorted_samples(const std::vector<int64_t>& raw) {
    std::vector<int64_t> v = raw;
    std::sort(v.begin(), v.end());
    return v;
}

// One batch duration (integer ns) normalized by the number of updates in the
// batch. This is a batch-average ns/update, NOT a directly measured single-update
// latency — the docs make that distinction explicit.
inline double normalized_ns_per_update(int64_t batch_elapsed_ns,
                                       uint64_t batch_operations) {
    if (batch_operations == 0) return 0.0;
    return static_cast<double>(batch_elapsed_ns) /
           static_cast<double>(batch_operations);
}

// ---------------------------------------------------------------------------
// Split a total update count into (number of FULL batches, remainder ops).
// updates / batch full batches each apply exactly `batch` ops. A nonzero
// remainder (always < batch) is a final PARTIAL batch: it is timed and recorded
// with its ACTUAL op count (`remainder`), and it is EXCLUDED from the percentile
// distribution (a short batch would carry disproportionate timer-boundary noise
// into the tail). A partial batch must NEVER be normalized by the full batch
// size — only by its actual op count.
// ---------------------------------------------------------------------------
struct BatchSplit {
    size_t   full;      // number of full batches (the distribution basis)
    uint64_t remainder; // ops in the optional trailing partial batch, < batch
};

inline BatchSplit split_batches(uint64_t updates, uint64_t batch) {
    return BatchSplit{static_cast<size_t>(updates / batch), updates % batch};
}

// ---------------------------------------------------------------------------
// One timed batch, shared by the benchmark and the unit tests.
// `ops` is the number of updates applied in the batch: the full batch_size for
// a FULL batch, or the ACTUAL trailing remainder (updates % batch) for a final
// PARTIAL batch. `elapsed_ns` is the raw integer steady_clock duration (source
// of truth; never rounded before analysis).
// ---------------------------------------------------------------------------
struct BatchSample {
    uint64_t ops;
    int64_t  elapsed_ns;
};

// ---------------------------------------------------------------------------
// Distribution metrics over FULL batches only, batch-normalized to ns/update.
// A trailing partial batch (if any) is recorded in the raw CSV but is EXCLUDED
// from EVERY one of these metrics — mean, min, percentiles, and max are all
// computed over the first `full_batches` samples only. This is the single
// production rule the benchmark and the regression tests share: if the rule
// ever regresses (e.g. a partial sample leaks into the basis), the tests that
// call distribution_metrics() / full_batch_elapsed() fail.
// ---------------------------------------------------------------------------
struct DistributionMetrics {
    size_t distribution_samples = 0; // number of FULL batches in the basis
    double mean_ns_per_update   = 0.0;
    double min_ns_per_update    = 0.0;
    double p50_ns_per_update    = 0.0;
    double p90_ns_per_update    = 0.0;
    double p99_ns_per_update    = 0.0;
    double p99_9_ns_per_update  = 0.0; // P99.9
    double max_ns_per_update    = 0.0;
};

// Elapsed durations of the first `full_batches` samples ONLY (the full batches).
// The caller appends full batches first and the optional partial LAST, so this
// is exactly "the full-batch part of the run". It never copies a partial sample
// even if `samples` is longer than `full_batches` (defensive: it copies at most
// full_batches entries).
inline std::vector<int64_t> full_batch_elapsed(
    const std::vector<BatchSample>& samples, size_t full_batches) {
    const size_t n = full_batches < samples.size() ? full_batches : samples.size();
    std::vector<int64_t> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) v.push_back(samples[i].elapsed_ns);
    return v;
}

// Distribution metrics over the FULL-batch basis only (see the struct comment).
// batch_size is the normalization factor (ns per update = elapsed / batch_size).
// Returns all-zero metrics when there is no full batch (nothing to report).
inline DistributionMetrics distribution_metrics(
    const std::vector<BatchSample>& samples, size_t full_batches,
    uint64_t batch_size) {
    DistributionMetrics m;
    m.distribution_samples = full_batches < samples.size() ? full_batches
                                                           : samples.size();
    const std::vector<int64_t> full = full_batch_elapsed(samples, full_batches);
    if (full.empty()) return m; // no full batch -> no distribution
    const double scale = 1.0 / static_cast<double>(batch_size);
    const std::vector<int64_t> srt = sorted_samples(full);
    double sum = 0.0;
    for (int64_t e : full) sum += static_cast<double>(e);
    m.mean_ns_per_update = sum / static_cast<double>(full.size()) * scale;
    m.min_ns_per_update  = static_cast<double>(srt.front()) * scale;
    m.p50_ns_per_update  = static_cast<double>(percentile_rank(srt, 0.50)) * scale;
    m.p90_ns_per_update  = static_cast<double>(percentile_rank(srt, 0.90)) * scale;
    m.p99_ns_per_update  = static_cast<double>(percentile_rank(srt, 0.99)) * scale;
    m.p99_9_ns_per_update = static_cast<double>(percentile_rank(srt, 0.999)) * scale;
    m.max_ns_per_update  = static_cast<double>(srt.back()) * scale;
    return m;
}

} // namespace llob_tail
