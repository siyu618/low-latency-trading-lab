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

} // namespace llob_tail
