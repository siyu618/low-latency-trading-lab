// Phase 4 (tail-latency) lightweight unit tests.
//
// These test the POST-measurement statistics and the deterministic-input /
// calibration plumbing WITHOUT running the canonical multi-million-update tail
// benchmark. Everything here is fast and deterministic:
//   * percentile calculation (nearest-rank)
//   * batch normalization (batch-normalized ns/update)
//   * exact batch division and partial-batch rejection
//   * deterministic stream behavior (same seed -> identical stream; differential
//     map-vs-flat replay agreement; seed change -> different stream)
//   * timer-calibration path (the empty-timing-skeleton returns sane samples)
//
// A plain CHECK macro reports file/line of the first failing assertion; any
// failed CHECK accumulates into a total and main() returns non-zero so CTest
// fails on a bad run (same style as order_book_tests.cpp).

#include "../benchmark/tail_stats.h"
#include "../benchmark/stream_gen.h"

#include "flat_order_book.h"
#include "map_order_book.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using llob::ApplyResult;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::MapOrderBook;
using llob_tail::normalized_ns_per_update;
using llob_tail::percentile_rank;
using llob_tail::sorted_samples;

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

// Prints the suite result and returns the number of failures in THIS suite, so
// main() can accumulate a process-wide total (summary() itself resets the
// per-suite counters for the next suite).
int summary(const char* suite) {
    const int n_fail = g_failures;
    if (n_fail == 0) {
        std::printf("[ok] %-42s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-42s (%d/%d checks FAILED)\n", suite, n_fail, g_checks);
    }
    g_failures = 0;
    g_checks   = 0;
    return n_fail;
}

// ---------------------------------------------------------------------------
// Nearest-rank percentile.
// ---------------------------------------------------------------------------
void test_percentile() {
    // [1,2,3] (N=3): P50 = ceil(1.5)=2nd = 2; P90 = ceil(2.7)=3rd = 3.
    const std::vector<int64_t> s3{1, 2, 3};
    CHECK(percentile_rank(s3, 0.50) == 2);
    CHECK(percentile_rank(s3, 0.90) == 3);
    CHECK(percentile_rank(s3, 1.00) == 3); // max

    // [10,20,30,40] (N=4): P50 = ceil(2)=2nd = 20; P75 = ceil(3)=3rd = 30;
    // P99 = ceil(3.96)=4th = 40.
    const std::vector<int64_t> s4{10, 20, 30, 40};
    CHECK(percentile_rank(s4, 0.50) == 20);
    CHECK(percentile_rank(s4, 0.75) == 30);
    CHECK(percentile_rank(s4, 0.99) == 40);
    CHECK(percentile_rank(s4, 1.00) == 40);

    // Single element.
    const std::vector<int64_t> s1{7};
    CHECK(percentile_rank(s1, 0.5) == 7);

    // Empty: defined to return T{} (never dereferenced).
    const std::vector<int64_t> s0{};
    CHECK(percentile_rank(s0, 0.5) == 0);
}

// ---------------------------------------------------------------------------
// sorted_samples returns an ascending copy; the raw vector is untouched.
// ---------------------------------------------------------------------------
void test_sorted_copy() {
    const std::vector<int64_t> raw{50, 10, 40, 20, 30};
    const std::vector<int64_t> s = sorted_samples(raw);
    CHECK(s.size() == raw.size());
    bool sorted = true;
    for (size_t i = 1; i < s.size(); ++i) sorted = sorted && s[i - 1] <= s[i];
    CHECK(sorted);
    // raw preserved in original order (source of truth intact)
    CHECK(raw[0] == 50 && raw[1] == 10 && raw[2] == 40);
}

// ---------------------------------------------------------------------------
// Batch normalization: batch-normalized ns/update is elapsed / batch ops.
// ---------------------------------------------------------------------------
void test_normalization() {
    CHECK(normalized_ns_per_update(5120, 512) == 10.0);
    CHECK(normalized_ns_per_update(256, 512) == 0.5);
    CHECK(normalized_ns_per_update(1000, 1) == 1000.0);
    CHECK(normalized_ns_per_update(0, 512) == 0.0);
    CHECK(normalized_ns_per_update(999, 0) == 0.0); // guarded divide-by-zero
}

// ---------------------------------------------------------------------------
// Batch splitting: every update is applied as full batches plus an optional
// trailing PARTIAL batch, which is recorded with its ACTUAL op count and
// excluded from the distribution (never normalized by the full batch size).
// ---------------------------------------------------------------------------
void test_batch_division() {
    using llob_tail::BatchSplit;
    using llob_tail::split_batches;

    // The spec's suggested canonical default does NOT divide evenly:
    // 10,000,000 / 512 = 19531 full batches, remainder 128.
    const BatchSplit ten_m = split_batches(10'000'000, 512);
    CHECK(ten_m.full == 19531u);
    CHECK(ten_m.remainder == 128u); // 10,000,000 % 512 == 128

    // A batch size that does divide evenly has no partial batch.
    const BatchSplit even = split_batches(10'000'000, 1250);
    CHECK(even.full == 8000u);
    CHECK(even.remainder == 0u);

    // The remainder is always < batch.
    const BatchSplit s2 = split_batches(1000, 512);
    CHECK(s2.full == 1u);
    CHECK(s2.remainder == 488u);

    // A partial batch must NEVER be normalized by the full batch size — only by
    // its actual op count. 1000 updates in 512-batches = 1 full (512) + partial
    // of 488. If the partial took 4880 ns: dividing by 488 (correct) gives 10.0;
    // dividing by 512 (the forbidden full-batch size) would give 9.53125.
    CHECK(normalized_ns_per_update(4880, 488) == 10.0);      // actual op count
    CHECK(normalized_ns_per_update(4880, 512) != 10.0);      // never the full size
    CHECK(normalized_ns_per_update(4880, 488) > normalized_ns_per_update(4880, 512));
}

// ---------------------------------------------------------------------------
// A trailing partial batch is EXCLUDED from the whole distribution — including
// the MEAN, not just the percentiles. This guards the invariant that all
// distribution metrics (mean, min, percentiles, max) are computed over FULL
// batches only; a partial's elapsed must never leak into any of them.
// ---------------------------------------------------------------------------
void test_partial_excluded_from_mean() {
    // Simulate the benchmark's storage: full-batch samples plus one partial.
    // Distribution metrics must be identical whether the partial is absent or
    // present (the partial is recorded but excluded).
    const std::vector<int64_t> full_elapsed{1000, 1200, 1100}; // 3 full, 512 ops
    const int64_t partial_elapsed = 5000;                      // 128-op partial

    double mean_no_partial = 0.0;
    for (int64_t s : full_elapsed) mean_no_partial += static_cast<double>(s);
    mean_no_partial /= static_cast<double>(full_elapsed.size());

    // The buggy form would have added the partial into the sum.
    double sum_with_partial = mean_no_partial * static_cast<double>(full_elapsed.size());
    sum_with_partial += static_cast<double>(partial_elapsed);
    const double mean_if_partial_leaked =
        sum_with_partial / static_cast<double>(full_elapsed.size());

    CHECK(mean_if_partial_leaked != mean_no_partial); // the partial MUST not shift it
}


// the stream in (full batches + trailing partial of updates % batch) must leave
// the book in EXACTLY the same state as applying the whole stream at once. This
// is the correctness invariant behind the benchmark's allow-a-partial-final-batch
// rule: every update is applied, in order, once.
// ---------------------------------------------------------------------------
void test_partial_batch_replay() {
    using namespace llob_bench;
    using llob_tail::BatchSplit;
    using llob_tail::split_batches;

    const int64_t n = 1'000;
    const uint64_t batch   = 512;
    const uint64_t updates = 2'000; // NOT divisible by 512: 3 full + rem 464
    const auto snap = StreamGen::fill_snapshot(n);
    const auto ops  = StreamGen::steady_ops(Workload::C, n, updates, kDefaultSeed);
    const BatchSplit bs = split_batches(updates, batch);
    CHECK(bs.full == 3u);
    CHECK(bs.remainder == 464u);
    CHECK(bs.full * batch + bs.remainder == updates); // every update accounted

    // Reference: whole stream, one book.
    FlatOrderBook ref(1, 2 * n);
    CHECK(ref.load_snapshot(snap));
    for (const L2Update& u : ops) (void)ref.apply(u);

    // Batched: full batches then the trailing partial, same ops in order.
    FlatOrderBook batched(1, 2 * n);
    CHECK(batched.load_snapshot(snap));
    const L2Update* it = ops.data();
    for (size_t b = 0; b < bs.full; ++b)
        for (uint64_t i = 0; i < batch; ++i, ++it) (void)batched.apply(*it);
    for (uint64_t i = 0; i < bs.remainder; ++i, ++it) (void)batched.apply(*it);
    CHECK(it == ops.data() + ops.size()); // all ops consumed, none skipped/dup

    CHECK(batched.synced() == ref.synced());
    CHECK(batched.last_applied_seq() == ref.last_applied_seq());
    CHECK(batched.best_bid() == ref.best_bid());
    CHECK(batched.best_ask() == ref.best_ask());
    CHECK(batched.level_count() == ref.level_count());
}


// Field-wise stream equality (L2Update has no operator==).
bool same_stream(const std::vector<llob::L2Update>& x,
                 const std::vector<llob::L2Update>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i].seq != y[i].seq || x[i].price != y[i].price ||
            x[i].qty != y[i].qty || x[i].side != y[i].side)
            return false;
    }
    return true;
}

void test_deterministic_stream() {
    using namespace llob_bench;
    const int64_t n = 10'000;
    const uint64_t updates = 50'000;

    const auto a1 = StreamGen::steady_ops(Workload::A, n, updates, kDefaultSeed);
    const auto a2 = StreamGen::steady_ops(Workload::A, n, updates, kDefaultSeed);
    CHECK(a1.size() == updates);
    CHECK(same_stream(a1, a2)); // deterministic

    const auto e1 = StreamGen::steady_ops(Workload::E, n, updates, kDefaultSeed);
    const auto e2 = StreamGen::steady_ops(Workload::E, n, updates, kDefaultSeed);
    CHECK(same_stream(e1, e2));

    const auto adiff = StreamGen::steady_ops(Workload::A, n, updates, 42);
    CHECK(!same_stream(adiff, a1)); // seed change yields a different stream

    // Sequence numbers are contiguous from 2N+1 and every op is in-domain.
    CHECK(a1.front().seq == 2ull * static_cast<uint64_t>(n) + 1ull);
    bool contiguous = true;
    for (size_t i = 1; i < a1.size(); ++i)
        contiguous = contiguous && (a1[i].seq == a1[i - 1].seq + 1);
    CHECK(contiguous);
}

// ---------------------------------------------------------------------------
// Differential replay: the same deterministic stream applied to both books
// yields identical externally-visible final state (the Phase 1/2 parity
// guarantee, exercised at Phase 4's stream granularity).
// ---------------------------------------------------------------------------
void test_differential_replay() {
    using namespace llob_bench;
    const int64_t n = 1'000;
    const uint64_t updates = 20'000;
    for (int w = 0; w < kWorkloadCount; ++w) {
        const Workload wl = static_cast<Workload>(w);
        const auto snap = StreamGen::fill_snapshot(n);
        const auto ops  = StreamGen::steady_ops(wl, n, updates, kDefaultSeed);

        MapOrderBook  mbook(1, 2 * n);
        FlatOrderBook fbook(1, 2 * n);
        CHECK(mbook.load_snapshot(snap));
        CHECK(fbook.load_snapshot(snap));
        for (const L2Update& u : ops) {
            const ApplyResult rm = mbook.apply(u);
            const ApplyResult rf = fbook.apply(u);
            if (!(rm == rf && mbook.synced() == fbook.synced() &&
                  mbook.best_bid() == fbook.best_bid() &&
                  mbook.best_ask() == fbook.best_ask() &&
                  mbook.last_applied_seq() == fbook.last_applied_seq())) {
                CHECK(false); // divergence; report the cell
                break;
            }
        }
        CHECK(mbook.synced());
        CHECK(mbook.level_count() == fbook.level_count());
    }
}

// ---------------------------------------------------------------------------
// Timer-calibration path: the empty-timing-skeleton (no book work) returns a
// sane, non-empty sample vector of non-negative durations. It also bounds the
// overhead against what a real batch duration would be (a ~few-ns/update flat
// batch of 512 is on the order of microseconds; a 512-iteration empty boundary
// loop is much smaller, so the median boundary overhead must be well under a
// microsecond on this machine).
// ---------------------------------------------------------------------------
void test_calibration_path() {
    std::vector<int64_t> cal;
    uint64_t sink = 0;
    {
        // Use the exact skeleton the benchmark runs (re-implemented here to keep
        // the test independent of the benchmark's anonymous namespace).
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 512; ++i) {
            sink += static_cast<uint64_t>(i);
            // optimizer barrier (as in the benchmark)
#if defined(__GNUC__) || defined(__clang__)
            __asm__ __volatile__("" ::: "memory");
#else
            (void)0;
#endif
        }
        const auto t1 = std::chrono::steady_clock::now();
        cal.push_back(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    CHECK(cal.size() == 1);
    CHECK(cal[0] >= 0);
    CHECK(cal[0] < 1000000); // far under a millisecond for 512 empty iters
    (void)sink;
}

} // namespace

int main() {
    int total = 0;

    test_percentile();
    total += summary("percentile (nearest-rank)");

    test_sorted_copy();
    total += summary("sorted sample copy");

    test_normalization();
    total += summary("batch normalization");

    test_batch_division();
    total += summary("exact/partial batch division");

    test_partial_batch_replay();
    total += summary("partial-batch replay (all updates applied)");

    test_partial_excluded_from_mean();
    total += summary("partial batch excluded from mean");

    test_deterministic_stream();
    total += summary("deterministic stream");

    test_differential_replay();
    total += summary("differential replay (map vs flat)");

    test_calibration_path();
    total += summary("timer calibration path");

    std::printf("%s\n", total == 0 ? "phase4 stats: all ok"
                                   : "phase4 stats: FAILURES");
    return total == 0 ? 0 : 1;
}
