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
// power-of-two capacity, so the sample schedule visits every ring position
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
// Messages [0, settling) are excluded. The schedule records on the
// interval-th, 2*interval-th, ... eligible message, so the count is
// floor(measured / interval).
constexpr std::uint64_t expected_sample_count(std::uint64_t messages,
                                              std::uint64_t settling,
                                              std::uint64_t interval) noexcept {
    if (messages <= settling || interval == 0) return 0;
    return (messages - settling) / interval;
}

// The value an UNSAMPLED message carries in its `ready_ticks` field.
//
// A sampled message carries a raw steady-clock tick count instead. The two are
// distinguished by an exact equality test, never by a magnitude comparison, so
// there is no threshold to tune and no "close enough" case.
//
// This is the ONE place the sentinel is defined. Both threads compare against the
// same constant, and a message whose field disagrees with the schedule its
// sequence number implies is a correctness failure rather than a silent
// mis-measurement — see `stamp_contract_ok` below.
inline constexpr std::int64_t kUnsampledSentinel = 0;

// Deterministic sparse sampler, keyed on the message's SEQUENCE NUMBER rather
// than on a call count.
//
// Sequence-keyed rather than call-keyed so that the producer and the consumer can
// each evaluate the SAME schedule independently: the producer asks about the
// sequence it is about to push, the consumer about the sequence it has just
// popped. A call-counting sampler could not do this — it would be correct only
// while both sides happened to call it the same number of times in the same
// order, which is exactly the kind of silent coupling this design avoids.
//
// NEVER `index % interval == 0`. A modulus over an interval sharing a factor with
// a power-of-two capacity samples the same ring positions on every pass,
// correlating the sample with slot identity. An ODD interval is coprime with
// every power-of-two capacity, so the schedule rotates through all positions.
//
// The first sampled sequence is `settling + interval - 1`, i.e. the
// interval-th eligible message when counting from 0 at `settling`. This is
// identical to what a countdown reloaded to `interval` selects, and a test pins
// that equivalence.
class SampleSchedule {
public:
    static constexpr std::uint64_t kNever = ~std::uint64_t{0};

    constexpr SampleSchedule(std::uint64_t settling,
                             std::uint64_t interval) noexcept
        : interval_(interval),
          next_(interval == 0 ? kNever : settling + interval - 1) {}

    // True when `seq` is the next scheduled sample. Const: asking does not
    // advance the schedule. Callers precede `advance()` with this test.
    constexpr bool is_sample(std::uint64_t seq) const noexcept {
        return seq == next_;
    }

    // Move past the sample just taken. Call ONLY after `is_sample(seq)` returned
    // true for the sequence being recorded, so the two sides cannot drift.
    constexpr void advance() noexcept { next_ += interval_; }

    constexpr std::uint64_t next_sample_seq() const noexcept { return next_; }
    constexpr std::uint64_t interval() const noexcept { return interval_; }

private:
    std::uint64_t interval_;
    std::uint64_t next_;
};

// The instrumented message contract, checked on receipt.
//
// A sampled sequence MUST carry a real producer stamp; an unsampled sequence
// MUST carry the sentinel. Both sides derive `sampled` from the same schedule,
// so this test IS the producer/consumer schedule-agreement check: if the two
// sides ever disagreed about a sequence, the message's own field would
// contradict the receiver's expectation and fail here.
constexpr bool stamp_contract_ok(std::int64_t ready_ticks,
                                 bool sampled) noexcept {
    return sampled ? ready_ticks != kUnsampledSentinel
                   : ready_ticks == kUnsampledSentinel;
}

// ---------------------------------------------------------------------------
// Message types — 16, 32 and 64 bytes, each carrying its own producer stamp.
//
// These are NOT the Phase-2/3 shapes (8/32/64) and Phase-4 absolute numbers must
// not be compared with Phase-2 or Phase-3 numbers.
//
// THE STAMP TRAVELS INSIDE THE MESSAGE. `ready_ticks` is a field of every payload
// and reaches the consumer through the same SPSC path as the rest of the payload
// — producer -> SPSC message -> consumer. There is deliberately NO side array,
// map or shared metadata structure holding stamps. A side array is a second,
// independently addressed memory working set whose footprint scales with
// capacity, which contaminates the capacity comparison this phase exists to
// make; it also forces a write and a read per message. The superseded dataset
// did use one, and its `SUPERSEDED.md` records why that was wrong.
//
// Every DETERMINISTIC field is re-derived from the sequence number on receipt, so
// a torn, stale or partially published payload is DETECTABLE. A validator that
// cannot fail is not a correctness gate, which is why these are exercised by
// tests.
//
// `ready_ticks` is NOT deterministic and is deliberately excluded from
// `valid_for()` and from `fold()`. Including it would make the cross-repetition
// checksum comparison meaningless, because two repetitions of the same
// configuration legitimately carry different timestamps. Its contract is checked
// against the sample schedule instead — see `stamp_contract_ok`.
//
// Sizes are exact and are asserted below. Note what the 16-byte shape can and
// cannot validate: with only two words, one of them the stamp, Msg16 has room for
// `seq` alone as a derived field. Its gate is the sequence check plus the
// sentinel contract, which is weaker than Msg32 and Msg64 — those keep three and
// seven derived fields respectively.
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
    std::int64_t  ready_ticks           = kUnsampledSentinel;

    // Builds the DETERMINISTIC payload only. `ready_ticks` stays at the sentinel
    // until the producer stamps a sampled message, so an unstamped message is
    // well formed by construction rather than by remembering to clear a field.
    static Msg16 make(std::uint64_t s) noexcept {
        Msg16 m;
        m.seq = s;
        return m;
    }
    bool valid_for(std::uint64_t s) const noexcept { return seq == s; }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq);
    }
};

struct Msg32 {
    static constexpr std::size_t kBytes = 32;
    std::uint64_t seq                   = 0;
    std::uint64_t price                 = 0;
    std::uint64_t qty                   = 0;
    std::int64_t  ready_ticks           = kUnsampledSentinel;

    static Msg32 make(std::uint64_t s) noexcept {
        Msg32 m;
        m.seq   = s;
        m.price = derive_price(s);
        m.qty   = derive_qty(s);
        return m;
    }
    bool valid_for(std::uint64_t s) const noexcept {
        return seq == s && price == derive_price(s) && qty == derive_qty(s);
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty);
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
    std::int64_t  ready_ticks           = kUnsampledSentinel;

    static Msg64 make(std::uint64_t s) noexcept {
        Msg64 m;
        m.seq   = s;
        m.price = derive_price(s);
        m.qty   = derive_qty(s);
        m.flags = derive_flags(s);
        m.pad0  = mix64(s ^ 0x1111ull);
        m.pad1  = mix64(s ^ 0x2222ull);
        m.pad2  = mix64(s ^ 0x3333ull);
        return m;
    }
    bool valid_for(std::uint64_t s) const noexcept {
        return seq == s && price == derive_price(s) && qty == derive_qty(s) &&
               flags == derive_flags(s) && pad0 == mix64(s ^ 0x1111ull) &&
               pad1 == mix64(s ^ 0x2222ull) && pad2 == mix64(s ^ 0x3333ull);
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, flags, pad0, pad1, pad2);
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
    std::uint64_t expected_delivered          = 0;
    std::uint64_t delivered                   = 0;
    std::uint64_t expected_samples            = 0;
    std::uint64_t sample_count                = 0;
    // The proof that instrumentation is SPARSE rather than an assurance that it
    // is: these are counted at the call site, so a repetition that took a clock
    // read per message reports roughly 10,000,000 here and FAILS the gate below,
    // instead of quietly producing the same 9,696 recorded latencies.
    std::uint64_t producer_sample_clock_reads = 0;
    std::uint64_t consumer_sample_clock_reads = 0;
    std::uint64_t payload_mismatches          = 0;
    std::uint64_t stamp_contract_failures     = 0;
    std::uint64_t timestamp_inversions        = 0;
    bool          sequence_ok                 = true;
    bool          cursor_ok                   = true;
};

constexpr bool repetition_ok(const RepInvariants& v) noexcept {
    return v.cursor_ok && v.sequence_ok && v.delivered == v.expected_delivered &&
           v.sample_count == v.expected_samples &&
           v.producer_sample_clock_reads == v.expected_samples &&
           v.consumer_sample_clock_reads == v.expected_samples &&
           v.payload_mismatches == 0 && v.stamp_contract_failures == 0 &&
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
