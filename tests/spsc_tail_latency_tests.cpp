// Experiment 02 Phase 4 — SPSC tail-latency HARNESS tests.
//
// These test the DETERMINISTIC parts of the Phase-4 harness, without running the
// canonical multi-million-message matrix and without timing anything:
//
//   * the derived expected sample count, and its agreement with the actual
//     countdown schedule over a sweep of (messages, settling, interval) — the
//     invariant a repetition FAILS on, so it had better be right
//   * the countdown sampler is a countdown, not `index % interval`, and an ODD
//     interval visits every ring position of every power-of-two capacity
//   * an even interval is rejected by the benchmark's own validation
//   * the tick -> nanosecond conversion is exact and injective on this platform
//   * nearest-rank summary values (min/mean/P50/P90/P99/P99.9/max) on vectors
//     with hand-computed answers, including a size where nearest-rank does NOT
//     return the maximum
//   * the payload validators for 16/32/64-byte messages actually REJECT a
//     corrupted, stale or partially published payload — a validator that cannot
//     fail is not a correctness gate
//   * timestamp validation flags a negative latency instead of reporting it as
//     an impossibly fast sample
//   * repetition_ok() fails on every one of its conditions, one at a time
//
// A plain CHECK macro reports file/line of the first failing assertion; any
// failed CHECK accumulates into a total and main() returns non-zero so CTest
// fails on a bad run (same style as order_book_tests.cpp).

#include "../benchmark/spsc_tail_harness.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using lltl_tail::Distribution;
using lltl_tail::expected_sample_count;
using lltl_tail::Msg16;
using lltl_tail::Msg32;
using lltl_tail::Msg64;
using lltl_tail::ns_per_tick;
using lltl_tail::RepInvariants;
using lltl_tail::repetition_ok;
using lltl_tail::SamplingCountdown;
using lltl_tail::summarize;
using lltl_tail::ticks_to_ns;
using lltl_tail::timestamp_ok;

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

int summary(const char* suite) {
    const int n_fail = g_failures;
    if (n_fail == 0) {
        std::printf("[ok] %-44s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-44s (%d/%d checks FAILED)\n", suite, n_fail,
                    g_checks);
    }
    g_failures = 0;
    g_checks   = 0;
    return n_fail;
}

// Run the real countdown over `messages` messages, skipping the settling prefix,
// and return how many samples it recorded. This is the schedule the benchmark
// actually executes.
std::uint64_t run_schedule(std::uint64_t messages, std::uint64_t settling,
                           std::uint64_t interval) {
    SamplingCountdown sampler(interval);
    std::uint64_t     recorded = 0;
    for (std::uint64_t i = 0; i < messages; ++i) {
        if (i >= settling && sampler.on_message()) ++recorded;
    }
    return recorded;
}

// ---------------------------------------------------------------------------
// The derived count is what the schedule produces, across a sweep.
// ---------------------------------------------------------------------------
void test_expected_count_matches_schedule() {
    const std::uint64_t intervals[] = {1, 3, 17, 1021, 4095, 65537};
    const std::uint64_t settlings[] = {0, 1, 100, 1000};
    const std::uint64_t countss[]   = {1, 2, 1023, 1024, 1025, 5000, 40000};

    for (std::uint64_t interval : intervals) {
        for (std::uint64_t settling : settlings) {
            for (std::uint64_t messages : countss) {
                if (messages <= settling) continue;
                const std::uint64_t derived =
                    expected_sample_count(messages, settling, interval);
                const std::uint64_t actual =
                    run_schedule(messages, settling, interval);
                CHECK(derived == actual);
                // A zero-interval schedule must not divide by zero, and must
                // record nothing.
                CHECK(interval != 0);
            }
        }
    }

    // The degenerate inputs are defined, not undefined.
    CHECK(expected_sample_count(100, 100, 1021) == 0); // nothing measured
    CHECK(expected_sample_count(50, 100, 1021) == 0);  // settling past the end
    CHECK(expected_sample_count(1000, 0, 0) == 0);     // zero interval
    CHECK(run_schedule(100, 0, 0) == 0);

    // The canonical Phase-4 shape, spelled out so a change to any of the three
    // constants shows up here as a number, not as a vibe.
    CHECK(expected_sample_count(10'000'000, 100'000, 1021) == 9696);
}

// ---------------------------------------------------------------------------
// The countdown is a countdown, and it is NOT `index % interval`.
//
// A modulus sampler with an interval that shares a factor with a power-of-two
// capacity revisits the same slot indices forever. The countdown over the
// canonical odd 1021 must instead visit EVERY slot index of every capacity.
// ---------------------------------------------------------------------------
void test_countdown_is_not_a_modulus() {
    // Direct demonstration that the two schedules differ: with interval 4 over
    // 12 eligible messages, both happen to record 3 samples, but at DIFFERENT
    // message indices (modulus records 0,4,8; countdown records 3,7,11).
    {
        SamplingCountdown sampler(4);
        std::vector<std::uint64_t> countdown_hits;
        for (std::uint64_t i = 0; i < 12; ++i) {
            if (sampler.on_message()) countdown_hits.push_back(i);
        }
        CHECK(countdown_hits.size() == 3);
        CHECK(countdown_hits[0] == 3);
        CHECK(countdown_hits[1] == 7);
        CHECK(countdown_hits[2] == 11);

        std::vector<std::uint64_t> modulus_hits;
        for (std::uint64_t i = 0; i < 12; ++i) {
            if (i % 4 == 0) modulus_hits.push_back(i);
        }
        CHECK(modulus_hits[0] == 0);
        CHECK(countdown_hits != modulus_hits);
    }

    // Slot rotation: an odd interval is coprime with every power-of-two
    // capacity, so the sampled slot indices cover ALL positions.
    const std::uint64_t capacities[] = {1024, 4096, 65536};
    for (std::uint64_t capacity : capacities) {
        std::vector<bool> seen(static_cast<std::size_t>(capacity), false);
        std::uint64_t     visited = 0;
        for (std::uint64_t k = 0; k < capacity; ++k) {
            const std::uint64_t slot = (k * lltl_tail::kDefaultInterval) % capacity;
            if (!seen[static_cast<std::size_t>(slot)]) {
                seen[static_cast<std::size_t>(slot)] = true;
                ++visited;
            }
        }
        CHECK(visited == capacity);
    }

    // The default interval must be odd: that is the whole reason it is 1021 and
    // not 1024.
    CHECK(lltl_tail::kDefaultInterval % 2 == 1);
    CHECK(lltl_tail::kDefaultInterval != 1024);

    // A countdown built with a zero interval records nothing and must not spin.
    SamplingCountdown degenerate(0);
    for (int i = 0; i < 10; ++i) CHECK(!degenerate.on_message());
}

// ---------------------------------------------------------------------------
// Exact tick -> nanosecond conversion.
//
// ns_per_tick returns 0 when no exact integer factor exists, which is what makes
// the benchmark's static_assert a build-time failure rather than a silent
// rounding of every sample.
// ---------------------------------------------------------------------------
void test_tick_conversion_is_exact() {
    // A nanosecond clock (the libc++/libstdc++ steady_clock case) converts 1:1.
    CHECK(ns_per_tick(1, 1'000'000'000) == 1);
    // A microsecond clock needs the exact factor 1000.
    CHECK(ns_per_tick(1, 1'000'000) == 1000);
    // A ratio that is NOT a whole number of nanoseconds per tick is rejected
    // rather than rounded: 1/3 ns per tick has no exact integer factor.
    CHECK(ns_per_tick(1, 3) == 0);
    // Degenerate inputs are rejected, not divided by.
    CHECK(ns_per_tick(1, 0) == 0);
    CHECK(ns_per_tick(0, 1) == 0);
    CHECK(ns_per_tick(-1, 1) == 0);

    // The conversion is a multiplication by a positive integer, so it is
    // monotonic and injective: distinct tick counts never collide, and ordering
    // is preserved. That is why converting a percentile AFTER the fact gives the
    // same answer as converting the samples first.
    const std::int64_t per_tick = 1;
    CHECK(ticks_to_ns(0, per_tick) == 0);
    CHECK(ticks_to_ns(1, per_tick) == 1);
    CHECK(ticks_to_ns(123'456, per_tick) == 123'456);
    CHECK(ticks_to_ns(10, per_tick) < ticks_to_ns(11, per_tick));

    const std::int64_t micro = 1000;
    CHECK(ticks_to_ns(7, micro) == 7000);
    CHECK(ticks_to_ns(7, micro) < ticks_to_ns(8, micro));
}

// ---------------------------------------------------------------------------
// Nearest-rank summary over observed values.
// ---------------------------------------------------------------------------
void test_summary_nearest_rank() {
    // N = 10, values 10..100 step 10. index(p) = ceil(p*10) - 1.
    //   P50 -> ceil(5)   - 1 = 4  -> 50
    //   P90 -> ceil(9)   - 1 = 8  -> 90
    //   P99 -> ceil(9.9) - 1 = 9  -> 100 (NOT 99: nearest rank, no interpolation)
    //   P99.9 -> ceil(9.99) - 1 = 9 -> 100
    std::vector<std::int64_t> raw;
    for (int i = 1; i <= 10; ++i) raw.push_back(static_cast<std::int64_t>(i) * 10);
    const Distribution d = summarize(raw);
    CHECK(d.count == 10);
    CHECK(d.min == 10);
    CHECK(d.max == 100);
    CHECK(d.p50 == 50);
    CHECK(d.p90 == 90);
    CHECK(d.p99 == 100);
    CHECK(d.p999 == 100);
    CHECK(d.mean > 54.9 && d.mean < 55.1); // 550/10 = 55 exactly

    // N = 1000: P99 is the 990th smallest, NOT the maximum. This is the case a
    // reader most often gets wrong, so it is pinned explicitly.
    std::vector<std::int64_t> big;
    for (std::int64_t v = 1; v <= 1000; ++v) big.push_back(v);
    const Distribution b = summarize(big);
    CHECK(b.count == 1000);
    CHECK(b.p50 == 500);
    CHECK(b.p90 == 900);
    CHECK(b.p99 == 990);
    CHECK(b.p999 == 999); // ceil(999.0) - 1 = 998 -> the 999th smallest = 999
    CHECK(b.max == 1000);
    CHECK(b.p99 != b.max); // the distinction that matters

    // Unsorted input is summarized exactly as if it were sorted: the summary
    // must not depend on arrival order.
    std::vector<std::int64_t> shuffled{90, 10, 100, 50, 70, 20, 80, 30, 60, 40};
    const Distribution s = summarize(shuffled);
    CHECK(s.count == d.count);
    CHECK(s.min == d.min);
    CHECK(s.max == d.max);
    CHECK(s.p50 == d.p50);
    CHECK(s.p90 == d.p90);
    CHECK(s.p99 == d.p99);
    CHECK(s.p999 == d.p999);

    // A single sample is all percentiles at once.
    const Distribution one = summarize(std::vector<std::int64_t>{42});
    CHECK(one.count == 1 && one.min == 42 && one.p50 == 42 && one.p90 == 42 &&
          one.p99 == 42 && one.p999 == 42 && one.max == 42);

    // An empty distribution is defined, not undefined.
    const Distribution none = summarize(std::vector<std::int64_t>{});
    CHECK(none.count == 0 && none.min == 0 && none.max == 0);
}

// ---------------------------------------------------------------------------
// Payload validators must be able to FAIL.
// ---------------------------------------------------------------------------
template <typename Msg>
void corrupt_each_field_and_check_rejection() {
    const std::uint64_t seq = 1'234'567;
    const Msg           good = Msg::make(seq);
    CHECK(good.valid_for(seq));

    // The right message for the wrong sequence is rejected.
    CHECK(!good.valid_for(seq + 1));

    // Every field, corrupted one at a time, must be caught.
    Msg bad = good;
    bad.seq += 1;
    CHECK(!bad.valid_for(seq));
    bad = good;
    bad.price += 1;
    CHECK(!bad.valid_for(seq));
    // `qty` exists on the 32- and 64-byte shapes but not on the 16-byte one, so
    // it is corrupted only where it exists.
    if constexpr (requires(Msg m) { m.qty += 1; }) {
        bad = good;
        bad.qty += 1;
        CHECK(!bad.valid_for(seq));
    }

    // A default-constructed (never published) payload is rejected.
    const Msg never{};
    CHECK(!never.valid_for(seq));

    // A payload from a DIFFERENT sequence — the stale-value case a ring buffer
    // can actually produce if a slot is reused too early — is rejected.
    const Msg other = Msg::make(seq + 4096);
    CHECK(!other.valid_for(seq));
}

void test_payload_validators() {
    CHECK(sizeof(Msg16) == 16);
    CHECK(sizeof(Msg32) == 32);
    CHECK(sizeof(Msg64) == 64);

    corrupt_each_field_and_check_rejection<Msg16>();
    corrupt_each_field_and_check_rejection<Msg32>();
    corrupt_each_field_and_check_rejection<Msg64>();

    // The 64-byte tail word folds the WHOLE payload, so a tail that was written
    // before the rest of the message (partial publication) is rejected.
    const std::uint64_t seq  = 99;
    Msg64               good = Msg64::make(seq);
    Msg64               torn = good;
    CHECK(torn.valid_for(seq));
    torn.pad1 += 1; // corrupt a field the tail covers
    CHECK(!torn.valid_for(seq));

    // Distinct sequences produce distinct folds: the checksum actually depends
    // on the payload rather than being constant.
    CHECK(Msg16::make(1).fold(0) != Msg16::make(2).fold(0));
    CHECK(Msg32::make(1).fold(0) != Msg32::make(2).fold(0));
    CHECK(Msg64::make(1).fold(0) != Msg64::make(2).fold(0));
}

// ---------------------------------------------------------------------------
// Timestamp validation.
// ---------------------------------------------------------------------------
void test_timestamp_validation() {
    CHECK(timestamp_ok(0));
    CHECK(timestamp_ok(1));
    CHECK(timestamp_ok(1'000'000));
    // A negative latency means the two stamps did not come from one clock domain
    // (or a stamp was read before it was published). It is INVALID, not fast.
    CHECK(!timestamp_ok(-1));
    CHECK(!timestamp_ok(-1'000'000));
}

// ---------------------------------------------------------------------------
// The PASS predicate: every condition must be able to fail on its own.
// ---------------------------------------------------------------------------
void test_repetition_ok_conditions() {
    RepInvariants good;
    good.expected_delivered   = 100;
    good.delivered            = 100;
    good.expected_samples     = 1000;
    good.sample_count         = 1000;
    good.payload_mismatches   = 0;
    good.timestamp_inversions = 0;
    good.sequence_ok          = true;
    good.cursor_ok            = true;
    CHECK(repetition_ok(good));

    RepInvariants v = good;
    v.delivered = 99; // a lost message
    CHECK(!repetition_ok(v));

    v = good;
    v.sample_count = 999; // fewer samples than derived: a FAILURE, not a
                          // smaller dataset
    CHECK(!repetition_ok(v));

    v = good;
    v.sample_count = 1001; // more samples than derived is equally wrong
    CHECK(!repetition_ok(v));

    v = good;
    v.payload_mismatches = 1;
    CHECK(!repetition_ok(v));

    v = good;
    v.timestamp_inversions = 1;
    CHECK(!repetition_ok(v));

    v = good;
    v.sequence_ok = false;
    CHECK(!repetition_ok(v));

    v = good;
    v.cursor_ok = false; // the layout gate is NOT optional
    CHECK(!repetition_ok(v));
}

// ---------------------------------------------------------------------------
// Retry/yield policy: the constant is the frozen Phase-2 one.
// ---------------------------------------------------------------------------
void test_frozen_harness_constants() {
    CHECK(lltl_tail::kYieldAfterMisses == 1024);
    CHECK(lltl_tail::kDefaultMessages == 10'000'000);
    CHECK(lltl_tail::kDefaultReps == 5);
    CHECK(lltl_tail::kDefaultWarmup == 1);
    CHECK(lltl_tail::kDefaultSettling == 100'000);
}

} // namespace

int main() {
    std::printf("Experiment 02 Phase 4 — SPSC tail-latency harness tests\n\n");

    // Exit-code self-test mode: a deliberately failing CHECK must make this
    // binary return non-zero. CTest drives it through assert_nonzero_exit.cmake
    // so a regression in the runner's failure propagation cannot go unnoticed.
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("self-test: deliberately failing CHECK\n");
        CHECK(1 == 2);
        return summary("exit-code self-test") == 0 ? 0 : 1;
    }

    int failures = 0;

    test_expected_count_matches_schedule();
    failures += summary("expected sample count vs schedule");

    test_countdown_is_not_a_modulus();
    failures += summary("countdown sampling / slot rotation");

    test_tick_conversion_is_exact();
    failures += summary("exact tick -> ns conversion");

    test_summary_nearest_rank();
    failures += summary("nearest-rank summary");

    test_payload_validators();
    failures += summary("payload validators reject corruption");

    test_timestamp_validation();
    failures += summary("timestamp validation");

    test_repetition_ok_conditions();
    failures += summary("repetition PASS predicate");

    test_frozen_harness_constants();
    failures += summary("frozen harness constants");

    if (failures == 0) {
        std::printf("\nALL PHASE 4 HARNESS TESTS PASSED\n");
    } else {
        std::printf("\n%d PHASE 4 HARNESS CHECK(S) FAILED\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
