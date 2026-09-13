// ---------------------------------------------------------------------------
// Experiment 02 Phase 4 — SPSC tail-latency HARNESS LOGIC, shared between the
// tail benchmark (benchmark/spsc_tail_latency_bench.cpp) and its tests
// (tests/spsc_tail_latency_tests.cpp).
//
// Everything in this header is PURE and deterministic: no threads, no queue, no
// clock reads, no I/O. That is deliberate — the sampling schedule, the derived
// sample count, the tick->ns conversion, the payload validators and the
// nearest-rank summary are exactly the parts that must be checked by a test
// rather than trusted, and they cannot be checked if they live inside main().
//
// The clock-dependent and queue-dependent parts (threads, the measured interval,
// file output) stay in the benchmark itself.
// ---------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include "tail_stats.h" // read-only: the repo's ONE nearest-rank percentile

namespace lltl_tail {

// ---------------------------------------------------------------------------
// Constants — frozen harness parameters
// ---------------------------------------------------------------------------

// The HARDENED Phase-2 retry/yield policy, adopted UNCHANGED: yield only after
// this many CONSECUTIVE failed attempts, and reset the run on any success.
inline constexpr int kYieldAfterMisses = 1024;

inline constexpr std::uint64_t kDefaultMessages = 10'000'000;
inline constexpr int           kDefaultReps     = 5;
inline constexpr int           kDefaultWarmup   = 1;
inline constexpr std::uint64_t kDefaultSettling = 100'000;

// ODD by design, and NOT 1024. An odd interval is coprime with every
// power-of-two capacity, so the countdown sample visits every ring position
// instead of a fixed subset of them.
inline constexpr std::uint64_t kDefaultInterval = 1021;

inline constexpr std::uint64_t kDefaultCalibReps = 200'000;

// ---------------------------------------------------------------------------
// Tick -> nanosecond conversion
//
// Implemented as multiplication by an integer factor. `kNanosecondsPerTick` is
// only well defined when the clock's period divides into whole nanoseconds
// EXACTLY; callers static_assert that before using it, so a platform whose
// steady_clock period is not nanosecond-exact fails to build rather than
// silently rounding every sample.
// ---------------------------------------------------------------------------

// Exact integer factor converting one clock tick to nanoseconds, or 0 when no
// such integer factor exists.
constexpr std::int64_t ns_per_tick(std::int64_t period_num,
                                   std::int64_t period_den) noexcept {
    if (period_num <= 0 || period_den <= 0) return 0;
    const std::int64_t scale = period_num * 1'000'000'000LL;
    if (scale % period_den != 0) return 0; // not an exact whole-ns period
    return scale / period_den;
}

constexpr std::int64_t ticks_to_ns(std::int64_t ticks,
                                   std::int64_t per_tick) noexcept {
    return ticks * per_tick;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

// The DERIVED number of samples a repetition must produce. This is a
// requirement, not a prediction: a repetition that observes a different count
// has failed, and must not be reported as a smaller dataset.
//
// Messages [0, settling) are excluded. The countdown is reloaded to `interval`
// after each recorded sample, so it records on the interval-th, 2*interval-th,
// ... eligible message — hence floor(measured / interval) samples.
constexpr std::uint64_t expected_sample_count(std::uint64_t messages,
                                              std::uint64_t settling,
                                              std::uint64_t interval) noexcept {
    if (messages <= settling || interval == 0) return 0;
    return (messages - settling) / interval;
}

// Deterministic sparse sampler: a countdown, never `index % interval == 0`.
//
// The modulus form is rejected because an interval sharing a factor with a
// power-of-two capacity would sample the same ring positions on every pass,
// which correlates the sample with slot identity. A countdown over a coprime
// interval rotates through all positions.
class SamplingCountdown {
public:
    explicit SamplingCountdown(std::uint64_t interval) noexcept
        : interval_(interval), remaining_(interval), recorded_(0) {}

    // Called once per eligible message, in message order. Returns true when this
    // message's latency must be recorded.
    bool on_message() noexcept {
        if (interval_ == 0) return false;
        if (--remaining_ == 0) {
            remaining_ = interval_;
            ++recorded_;
            return true;
        }
        return false;
    }

    std::uint64_t recorded() const noexcept { return recorded_; }
    std::uint64_t interval() const noexcept { return interval_; }

private:
    std::uint64_t interval_;
    std::uint64_t remaining_;
    std::uint64_t recorded_;
};

// ---------------------------------------------------------------------------
// Message types — 16, 32 and 64 bytes.
//
// These are NOT the Phase-2/3 shapes (8/32/64) and Phase-4 absolute numbers must
// not be compared with Phase-2 or Phase-3 numbers.
//
// Every field is RE-DERIVED from the sequence number on receipt, so a torn,
// stale or partially published payload is DETECTABLE. A validator that cannot
// fail is not a correctness gate, which is why these are exercised by tests.
// ---------------------------------------------------------------------------

constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

// Fold each word through its OWN mixing round, in sequence.
//
// Deliberately NOT `mix64(a ^ b)`: XOR-combining words before mixing lets two
// distinct messages collide — for the 16-byte shape, `(seq=1, price=900001)` and
// `(seq=2, price=900002)` both reduce to `900000` — so the checksum would be
// blind to a real payload difference. A test pins this.
template <typename... Words>
constexpr std::uint64_t fold_words(std::uint64_t h, Words... words) noexcept {
    const std::uint64_t w[] = {static_cast<std::uint64_t>(words)...};
    for (std::uint64_t x : w) {
        h = mix64(h ^ (x + 0x9e3779b97f4a7c15ull));
    }
    return h;
}

constexpr std::uint64_t derive_price(std::uint64_t seq) noexcept {
    return 900'000ull + (seq % 100'000ull);
}
constexpr std::uint64_t derive_qty(std::uint64_t seq) noexcept {
    return 1ull + (seq % 1'000ull);
}
constexpr std::uint64_t derive_flags(std::uint64_t seq) noexcept {
    return mix64(seq) >> 59;
}

struct Msg16 {
    static constexpr std::size_t kBytes = 16;
    std::uint64_t seq                   = 0;
    std::uint64_t price                 = 0;

    static Msg16 make(std::uint64_t s) noexcept {
        Msg16 m;
        m.seq   = s;
        m.price = derive_price(s);
        return m;
    }
    bool valid_for(std::uint64_t s) const noexcept {
        return seq == s && price == derive_price(s);
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price);
    }
};

struct Msg32 {
    static constexpr std::size_t kBytes = 32;
    std::uint64_t seq                   = 0;
    std::uint64_t price                 = 0;
    std::uint64_t qty                   = 0;
    std::uint64_t checksum              = 0;

    static Msg32 make(std::uint64_t s) noexcept {
        Msg32 m;
        m.seq      = s;
        m.price    = derive_price(s);
        m.qty      = derive_qty(s);
        m.checksum = mix64(s ^ mix64(m.price ^ m.qty));
        return m;
    }
    bool valid_for(std::uint64_t s) const noexcept {
        return seq == s && price == derive_price(s) && qty == derive_qty(s) &&
               checksum == mix64(s ^ mix64(price ^ qty));
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, checksum);
    }
};

struct Msg64 {
    static constexpr std::size_t kBytes = 64;
    std::uint64_t seq                   = 0;
    std::uint64_t price                 = 0;
    std::uint64_t qty                   = 0;
    std::uint64_t flags                 = 0;
    std::uint64_t pad0                  = 0;
    std::uint64_t pad1                  = 0;
    std::uint64_t pad2                  = 0;
    std::uint64_t tail                  = 0;

    static Msg64 make(std::uint64_t s) noexcept {
        Msg64 m;
        m.seq   = s;
        m.price = derive_price(s);
        m.qty   = derive_qty(s);
        m.flags = derive_flags(s);
        m.pad0  = mix64(s ^ 0x1111ull);
        m.pad1  = mix64(s ^ 0x2222ull);
        m.pad2  = mix64(s ^ 0x3333ull);
        m.tail  = tail_for(m);
        return m;
    }
    bool valid_for(std::uint64_t s) const noexcept {
        return seq == s && price == derive_price(s) && qty == derive_qty(s) &&
               flags == derive_flags(s) && pad0 == mix64(s ^ 0x1111ull) &&
               pad1 == mix64(s ^ 0x2222ull) && pad2 == mix64(s ^ 0x3333ull) &&
               tail == tail_for(*this);
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, tail);
    }

private:
    static std::uint64_t tail_for(const Msg64& m) noexcept {
        return fold_words(0, m.seq, m.price, m.qty, m.flags, m.pad0, m.pad1,
                          m.pad2);
    }
};

#define LLTL_TAIL_ASSERT_MESSAGE_SIZE(T, N)                                    \
    static_assert(sizeof(T) == (N),                                            \
                  "Phase-4 message type must be exactly " #N " bytes");        \
    static_assert(std::is_trivially_copyable_v<T>,                             \
                  "Phase-4 payload must stay trivially copyable")

LLTL_TAIL_ASSERT_MESSAGE_SIZE(Msg16, 16);
LLTL_TAIL_ASSERT_MESSAGE_SIZE(Msg32, 32);
LLTL_TAIL_ASSERT_MESSAGE_SIZE(Msg64, 64);
#undef LLTL_TAIL_ASSERT_MESSAGE_SIZE

// ---------------------------------------------------------------------------
// Distribution summary — nearest rank, observed values only.
//
// index(p) = ceil(p * N) - 1 over an ASCENDING-SORTED vector of observed
// samples, via the repo's single definition in benchmark/tail_stats.h. Never
// interpolated, so every reported percentile is a latency that actually
// occurred. P99.99 is deliberately NOT computed: these sample counts do not
// support a headline at that depth.
//
// `raw` holds RAW ticks, unrounded. Because tick->ns is multiplication by a
// positive integer constant, converting a percentile of ticks gives exactly the
// percentile of the converted samples — so the conversion is applied to the
// result, never to the inputs.
// ---------------------------------------------------------------------------

struct Distribution {
    std::uint64_t count = 0;
    std::int64_t  min   = 0;
    std::int64_t  p50   = 0;
    std::int64_t  p90   = 0;
    std::int64_t  p99   = 0;
    std::int64_t  p999  = 0;
    std::int64_t  max   = 0;
    double        mean  = 0.0; // in ticks
};

inline Distribution summarize(const std::vector<std::int64_t>& raw) {
    Distribution d;
    if (raw.empty()) return d;
    const std::vector<std::int64_t> sorted = llob_tail::sorted_samples(raw);
    double sum = 0.0;
    for (std::int64_t v : sorted) sum += static_cast<double>(v);
    d.count = static_cast<std::uint64_t>(sorted.size());
    d.mean  = sum / static_cast<double>(sorted.size());
    d.min   = sorted.front();
    d.max   = sorted.back();
    d.p50   = llob_tail::percentile_rank(sorted, 0.50);
    d.p90   = llob_tail::percentile_rank(sorted, 0.90);
    d.p99   = llob_tail::percentile_rank(sorted, 0.99);
    d.p999  = llob_tail::percentile_rank(sorted, 0.999);
    return d;
}

// ---------------------------------------------------------------------------
// The invariants a repetition must satisfy to be reportable.
//
// Kept here, as one function, so the benchmark and its verifier cannot drift
// apart on what "PASS" means.
// ---------------------------------------------------------------------------

struct RepInvariants {
    std::uint64_t expected_delivered   = 0;
    std::uint64_t delivered            = 0;
    std::uint64_t expected_samples     = 0;
    std::uint64_t sample_count         = 0;
    std::uint64_t payload_mismatches   = 0;
    std::uint64_t timestamp_inversions = 0;
    bool          sequence_ok          = true;
    bool          cursor_ok            = true;
};

constexpr bool repetition_ok(const RepInvariants& v) noexcept {
    return v.cursor_ok && v.sequence_ok && v.delivered == v.expected_delivered &&
           v.sample_count == v.expected_samples && v.payload_mismatches == 0 &&
           v.timestamp_inversions == 0;
}

// A latency measured as (received - ready) can never be negative on a monotonic
// clock. A negative value means the two stamps did not come from one clock
// domain, or that a stamp was read before it was published — either way the
// repetition is invalid rather than "fast".
constexpr bool timestamp_ok(std::int64_t latency_ticks) noexcept {
    return latency_ticks >= 0;
}

} // namespace lltl_tail
