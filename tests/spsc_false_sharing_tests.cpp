// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3A correctness tests.
//
// Phase 3A changes exactly one thing about the frozen Phase-1 SPSC ring buffer:
// the cache-line placement of its two cursors. Its correctness burden is
// therefore two-fold, and both halves are exercised here.
//
// (1) SEMANTIC EQUIVALENCE. Both Phase-3A variants must pass the SAME Phase-1
//     correctness protocol as the frozen SpscRingBuffer. Every single-thread
//     semantic test, every concurrent test, and the reference differential
//     against MutexBoundedQueue is run TWICE here — once for the same-line
//     control and once for the separated control — from one templated body, so
//     the two variants cannot drift apart in what is asserted about them.
//
// (2) LAYOUT EVIDENCE. The experiment's conclusion depends on the two variants
//     really being same-line and separated AT RUNTIME, on the object that ran.
//     These tests measure the actual cursor addresses, convert them to line
//     indices under the host's REPORTED line size, and require the placement to
//     match the claim. Two extra checks matter for honesty:
//
//       * a NEGATIVE CONTROL on the guard: a reported line larger than the
//         compile-time assumption must be REJECTED rather than accepted, so
//         the "separated" control cannot silently become same-line;
//       * ROBUSTNESS at smaller line sizes: because the same-line block keeps
//         both cursors within 16 bytes and the separated blocks are a full
//         assumption apart, both invariants also hold if the host's real line
//         is 64, 32 or 16 bytes. That is what makes the 128-byte compile-time
//         value a conservative choice rather than a lucky one.
//
// (3) EQUAL FOOTPRINT (Phase 3A.1). Cursor placement must be the ONLY thing
//     that differs. If the two cursor policies were different sizes, the payload
//     array declared after them would start at a different offset in each
//     variant — a second changed variable that shifts the object's internal
//     layout, and hence how the payload maps to cache lines/sets. So these tests
//     require, for every T/Capacity the experiment uses: identical cursor-policy
//     size and alignment, identical queue object size, and an identical payload
//     offset from the object base. The equality being tested is of the relative
//     offset; these tests measure no cache-set indexing and claim none.
//     The header asserts this at compile time as well; it is tested here so that
//     the requirement is visible where the experiment's claims are checked.
//
//     Plus a variant-vs-variant differential: identical operation sequences fed
//     to both layouts must produce identical return values and identical
//     payload values, which is the "only cursor layout differs" claim stated as
//     something a test can fail on.
//
// The suite doubles as an exit-code self-test (LLDB_SELFTEST_FAIL), matching the
// Phase-1 suite.
//
// A plain CHECK macro reports the file/line of the first failing assertion;
// failures accumulate into a total and main() returns non-zero on any failure,
// so CTest genuinely fails on a bad run. No external test framework is used.

#include "cache_line.h"
#include "mutex_bounded_queue.h"
#include "spsc_cursor_layout_ring_buffer.h"
#include "spsc_ring_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using lltl::CursorLayout;
using lltl::CursorLayoutRingBuffer;
using lltl::MutexBoundedQueue;
using lltl::SeparatedCursorBlocks;
using lltl::SameLineCursorBlock;
using lltl::SpscSeparatedCursorRingBuffer;
using lltl::SpscSameLineRingBuffer;
using lltl::SpscRingBuffer;

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
// main() can accumulate a process-wide total (summary() resets the per-suite
// counters for the next suite).
int summary(const char* suite) {
    const int n_fail = g_failures;
    if (n_fail == 0) {
        std::printf("[ok] %-52s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-52s (%d/%d checks FAILED)\n", suite, n_fail,
                    g_checks);
    }
    g_failures = 0;
    g_checks   = 0;
    return n_fail;
}

// Runs a templated test body once per Phase-3A layout and reports each as its
// own named suite. Both variants are always exercised together, so a test can
// never be silently applied to only one of the two controls.
#define RUN_BOTH(name, fn)                                     \
    do {                                                       \
        fn<SameLineCursorBlock>();                             \
        total += summary("same_line: " name);                  \
        fn<SeparatedCursorBlocks>();                           \
        total += summary("separated: " name);                  \
    } while (0)

// ---------------------------------------------------------------------------
// Payload types (shared with the Phase-1 suite's shapes).
// ---------------------------------------------------------------------------

// Structured, non-trivial payload: default-constructible, copy/move assignable.
struct Message {
    std::uint64_t seq = 0;
    std::string   text;

    Message() = default;
    Message(std::uint64_t s, std::string t) : seq(s), text(std::move(t)) {}
};

// Proves WHICH element operation each API call performs, so the overloads are
// exercised rather than assumed. Single-threaded => plain counters are fine.
struct CopyMoveProbe {
    std::uint64_t v = 0;
    static int    copies;
    static int    moves;

    CopyMoveProbe() = default;
    explicit CopyMoveProbe(std::uint64_t x) : v(x) {}
    CopyMoveProbe(const CopyMoveProbe& o) : v(o.v) { ++copies; }
    CopyMoveProbe(CopyMoveProbe&& o) noexcept : v(o.v) { ++moves; }
    CopyMoveProbe& operator=(const CopyMoveProbe& o) {
        v = o.v;
        ++copies;
        return *this;
    }
    CopyMoveProbe& operator=(CopyMoveProbe&& o) noexcept {
        v = o.v;
        ++moves;
        return *this;
    }
};
int CopyMoveProbe::copies = 0;
int CopyMoveProbe::moves   = 0;

// Multi-field fixed-size message whose every field is a deterministic function
// of seq, bound together by a checksum: a torn / partially published payload is
// caught even if one field alone happened to look plausible.
struct MarketMessage {
    std::uint64_t seq      = 0;
    std::uint64_t price    = 0;
    std::uint64_t qty      = 0;
    std::uint64_t checksum = 0;
};

constexpr std::uint64_t field_price(std::uint64_t seq) {
    return 900'000ull + (seq % 100'000ull);
}
constexpr std::uint64_t field_qty(std::uint64_t seq) {
    return 1ull + (seq % 1'000ull);
}
constexpr std::uint64_t field_checksum(std::uint64_t seq, std::uint64_t price,
                                       std::uint64_t qty) {
    std::uint64_t h = seq * 0x9E3779B97F4A7C15ull;
    h ^= price + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= qty + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

MarketMessage make_market_message(std::uint64_t seq) {
    MarketMessage m;
    m.seq      = seq;
    m.price    = field_price(seq);
    m.qty      = field_qty(seq);
    m.checksum = field_checksum(m.seq, m.price, m.qty);
    return m;
}

// Test-side waiting helper (the QUEUE never spins; this is only how a test
// thread waits for the other side). Busy-retries, then yields.
struct SpinWaiter {
    int misses = 0;

    void pause() {
        if (++misses >= 1024) {
            misses = 0;
            std::this_thread::yield();
        }
    }
};

// ---------------------------------------------------------------------------
// Single-thread semantic tests, run against BOTH layouts.
// ---------------------------------------------------------------------------
template <typename Policy>
void t_new_queue_empty() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    CHECK(q.empty());
    CHECK(q.capacity() == 8);
}

template <typename Policy>
void t_pop_empty_returns_false() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    int v = 123;
    CHECK(!q.try_pop(v));
    CHECK(v == 123); // out must be untouched on failure
    CHECK(q.empty());
}

template <typename Policy>
void t_push_pop_one() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    CHECK(q.try_push(42));
    CHECK(!q.empty());
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 42);
    CHECK(q.empty());
}

template <typename Policy>
void t_fifo_order() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    for (int i = 1; i <= 5; ++i) {
        CHECK(q.try_push(i));
    }
    for (int i = 1; i <= 5; ++i) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == i);
    }
    CHECK(q.empty());
}

template <typename Policy>
void t_fill_exactly_capacity_then_full_reject() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_push(i));
    }
    CHECK(!q.empty());
    CHECK(q.try_push(100) == false); // full: head - tail == Capacity
    int v = -1;
    CHECK(q.try_pop(v));
    CHECK(v == 0);
}

template <typename Policy>
void t_pop_after_full_drains_in_order() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_push(i));
    }
    for (int i = 0; i < 8; ++i) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == i);
    }
    CHECK(q.empty());
    int v = -1;
    CHECK(!q.try_pop(v)); // drained
    CHECK(q.empty());
}

// Wrap the PHYSICAL ring many times: each slot is overwritten and re-read
// repeatedly while the monotonic counters grow far past Capacity.
template <typename Policy>
void t_wrap_around_repeatedly() {
    CursorLayoutRingBuffer<std::uint64_t, 8, Policy> q;
    constexpr int kCycles = 100;
    std::uint64_t next    = 0;
    for (int c = 0; c < kCycles; ++c) {
        for (int i = 0; i < 8; ++i) {
            CHECK(q.try_push(next + static_cast<std::uint64_t>(i)));
        }
        for (int i = 0; i < 8; ++i) {
            std::uint64_t v = 0;
            CHECK(q.try_pop(v));
            CHECK(v == next);
            ++next;
        }
    }
    CHECK(next == static_cast<std::uint64_t>(kCycles * 8));
    CHECK(q.empty());
}

// Fill/drain cycles at varying occupancy, with a std::deque as source of truth.
template <typename Policy>
void t_interleaved_fill_drain_cycles() {
    CursorLayoutRingBuffer<int, 8, Policy> q;
    std::deque<int>                        model;
    std::uint64_t rng      = 0x9E3779B97F4A7C15ull; // fixed seed
    constexpr std::uint64_t kMix = 0xBF58476D1CE4E5B9ull;
    const auto next_rand = [&rng]() {
        rng += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = rng;
        z = (z ^ (z >> 30)) * kMix;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };

    int next_value = 0;
    for (int step = 0; step < 2000; ++step) {
        const std::size_t want = static_cast<std::size_t>(next_rand() & 7u);
        if (model.size() < 8 && next_rand() % 2u == 0u) { // push
            for (std::size_t i = 0; i <= want && model.size() < 8; ++i) {
                CHECK(q.try_push(next_value));
                model.push_back(next_value);
                ++next_value;
            }
        } else if (!model.empty()) { // pop
            const std::size_t n =
                want + 1 > model.size() ? model.size() : want + 1;
            for (std::size_t i = 0; i < n; ++i) {
                int v = -1;
                CHECK(q.try_pop(v));
                CHECK(v == model.front());
                model.pop_front();
            }
        }
    }
    while (!model.empty()) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == model.front());
        model.pop_front();
    }
    CHECK(q.empty());
}

template <typename Policy>
void t_payload_sequence_values_preserved() {
    CursorLayoutRingBuffer<std::uint64_t, 16, Policy> q;
    constexpr std::uint64_t kBase = 1'000'000'000ull;
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(q.try_push(kBase + static_cast<std::uint64_t>(i)));
    }
    for (std::size_t i = 0; i < 16; ++i) {
        std::uint64_t v = 0;
        CHECK(q.try_pop(v));
        CHECK(v == kBase + static_cast<std::uint64_t>(i));
    }
}

template <typename Policy>
void t_structured_message_copy() {
    CursorLayoutRingBuffer<Message, 4, Policy> q;
    Message src(7, "bid update");
    CHECK(q.try_push(src));        // const& overload -> must COPY
    src.text = "mutated after push"; // must not affect the enqueued copy
    Message out;
    CHECK(q.try_pop(out));
    CHECK(out.seq == 7);
    CHECK(out.text == "bid update");
}

template <typename Policy>
void t_structured_message_move() {
    CursorLayoutRingBuffer<Message, 4, Policy> q;
    Message tmp(9, "ask update");
    CHECK(q.try_push(std::move(tmp))); // && overload -> must MOVE
    Message out;
    CHECK(q.try_pop(out));
    CHECK(out.seq == 9);
    CHECK(out.text == "ask update");
}

template <typename Policy>
void t_copy_vs_move_overloads() {
    CopyMoveProbe::copies = 0;
    CopyMoveProbe::moves  = 0;
    {
        CursorLayoutRingBuffer<CopyMoveProbe, 4, Policy> q;
        CopyMoveProbe                                    a(1);
        CHECK(q.try_push(a));                // const& -> copy into the slot
        CHECK(q.try_push(CopyMoveProbe(2))); // &&     -> move into the slot
        CopyMoveProbe out1;
        CopyMoveProbe out2;
        CHECK(q.try_pop(out1)); // move out of the slot
        CHECK(q.try_pop(out2));
        CHECK(out1.v == 1);
        CHECK(out2.v == 2);
    }
    CHECK(CopyMoveProbe::moves >= 2);  // 2nd push + both pops must have moved
    CHECK(CopyMoveProbe::copies == 1); // exactly the const& push copied
}

// ---------------------------------------------------------------------------
// Concurrent tests, run against BOTH layouts.
// ---------------------------------------------------------------------------
template <typename Policy>
void t_concurrent_producer_consumer() {
    constexpr std::size_t   kCapacity = 1024;
    constexpr std::uint64_t kMessages = 1ull << 20;

    CursorLayoutRingBuffer<std::uint64_t, kCapacity, Policy> q;
    std::atomic<bool>          start{false};
    std::atomic<bool>          consumer_ok{true};
    std::atomic<std::uint64_t> consumer_count{0};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::uint64_t i = 0;
        while (i < kMessages) {
            if (q.try_push(i)) {
                ++i;
            } else {
                std::this_thread::yield(); // full; wait for the consumer
            }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::uint64_t expected = 0;
        std::uint64_t v        = 0;
        while (expected < kMessages) {
            if (q.try_pop(v)) {
                if (v != expected) {
                    consumer_ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
                consumer_count.store(expected, std::memory_order_relaxed);
            } else {
                std::this_thread::yield(); // empty; wait for the producer
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    CHECK(consumer_count.load(std::memory_order_relaxed) == kMessages);
    CHECK(consumer_ok.load(std::memory_order_relaxed)); // exact order, no dup/loss
}

// Tiny ring (Capacity = 2): the producer must overwrite the SAME physical slots
// over and over while the consumer reads them. The sharpest slot-reuse edge.
template <typename Policy>
void t_concurrent_rapid_slot_reuse() {
    constexpr std::size_t   kCapacity = 2;
    constexpr std::uint64_t kMessages = 400'000;

    CursorLayoutRingBuffer<std::uint64_t, kCapacity, Policy> q;
    std::atomic<bool>          start{false};
    std::atomic<bool>          order_ok{true};
    std::atomic<std::uint64_t> consumed{0};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SpinWaiter    wait;
        std::uint64_t i = 0;
        while (i < kMessages) {
            if (q.try_push(i)) {
                ++i;
            } else {
                wait.pause();
            }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SpinWaiter    wait;
        std::uint64_t expected = 0;
        std::uint64_t v        = 0;
        while (expected < kMessages) {
            if (q.try_pop(v)) {
                if (v != expected) {
                    order_ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
                consumed.store(expected, std::memory_order_relaxed);
            } else {
                wait.pause();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    CHECK(consumed.load(std::memory_order_relaxed) == kMessages);
    CHECK(order_ok.load(std::memory_order_relaxed));
}

// Every field of a multi-field message validated independently, plus a checksum
// binding them: proves the release/acquire publication edge protects the WHOLE
// payload, not merely the sequence cursor.
template <typename Policy>
void t_concurrent_multi_field_payload() {
    constexpr std::size_t   kCapacity = 4;
    constexpr std::uint64_t kMessages = 200'000;

    CursorLayoutRingBuffer<MarketMessage, kCapacity, Policy> q;
    std::atomic<bool>          start{false};
    std::atomic<bool>          fields_ok{true};
    std::atomic<std::uint64_t> consumed{0};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SpinWaiter    wait;
        std::uint64_t i = 0;
        while (i < kMessages) {
            if (q.try_push(make_market_message(i))) {
                ++i;
            } else {
                wait.pause();
            }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SpinWaiter    wait;
        std::uint64_t expected = 0;
        MarketMessage m;
        while (expected < kMessages) {
            if (q.try_pop(m)) {
                const bool ok =
                    (m.seq == expected) && (m.price == field_price(m.seq)) &&
                    (m.qty == field_qty(m.seq)) &&
                    (m.checksum == field_checksum(m.seq, m.price, m.qty));
                if (!ok) {
                    fields_ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
                consumed.store(expected, std::memory_order_relaxed);
            } else {
                wait.pause();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    CHECK(consumed.load(std::memory_order_relaxed) == kMessages);
    CHECK(fields_ok.load(std::memory_order_relaxed));
}

// FIFO semantics pinned to MutexBoundedQueue and a std::deque model at every
// step, exactly as in the Phase-1 suite (now for the Phase-3A type).
template <typename Policy>
void t_reference_differential() {
    constexpr std::size_t kCapacity = 64;
    MutexBoundedQueue<std::uint64_t, kCapacity>               mq;
    CursorLayoutRingBuffer<std::uint64_t, kCapacity, Policy>  sq;
    std::deque<std::uint64_t>                                 model;

    std::uint64_t rng  = 0xD1B54A32D192ED03ull; // fixed seed
    std::uint64_t next = 0;

    constexpr int kOps = 100'000;
    for (int op = 0; op < kOps; ++op) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull; // LCG
        const std::uint64_t roll = rng >> 33;

        const bool must_push = model.empty();
        const bool must_pop  = !must_push && model.size() == kCapacity;
        const bool do_push   = must_push || (!must_pop && (roll % 3u) < 2u);

        if (do_push) {
            const bool m_ok = mq.try_push(next);
            const bool s_ok = sq.try_push(next);
            CHECK(m_ok);
            CHECK(m_ok == s_ok);
            model.push_back(next);
            ++next;
        } else {
            std::uint64_t m_val = 0;
            std::uint64_t s_val = 0;
            const bool    m_ok  = mq.try_pop(m_val);
            const bool    s_ok  = sq.try_pop(s_val);
            CHECK(m_ok);
            CHECK(m_ok == s_ok);
            CHECK(m_val == s_val);
            CHECK(m_val == model.front());
            model.pop_front();
        }
        CHECK(mq.empty() == sq.empty());
        CHECK(mq.empty() == model.empty());
    }

    while (!model.empty()) {
        std::uint64_t m_val = 0;
        std::uint64_t s_val = 0;
        CHECK(mq.try_pop(m_val));
        CHECK(sq.try_pop(s_val));
        CHECK(m_val == s_val);
        CHECK(m_val == model.front());
        model.pop_front();
    }
    CHECK(mq.empty());
    CHECK(sq.empty());
}

// ---------------------------------------------------------------------------
// Variant-vs-variant differential: the "only cursor layout differs" claim,
// stated as something a test can fail on. An identical deterministic operation
// sequence is fed to both layouts; every return value and every payload value
// must agree, and both must agree with the deque model.
// ---------------------------------------------------------------------------
void test_variants_agree_under_identical_operation_sequence() {
    constexpr std::size_t kCapacity = 64;
    SpscSameLineRingBuffer<std::uint64_t, kCapacity>       a;
    SpscSeparatedCursorRingBuffer<std::uint64_t, kCapacity> b;
    std::deque<std::uint64_t>                               model;

    std::uint64_t rng  = 0x0123456789ABCDEFull; // fixed seed
    std::uint64_t next = 0;

    constexpr int kOps = 200'000;
    for (int op = 0; op < kOps; ++op) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        const std::uint64_t roll = rng >> 33;

        const bool must_push = model.empty();
        const bool must_pop  = !must_push && model.size() == kCapacity;
        const bool do_push   = must_push || (!must_pop && (roll % 3u) < 2u);

        if (do_push) {
            const bool a_ok = a.try_push(next);
            const bool b_ok = b.try_push(next);
            CHECK(a_ok);
            CHECK(a_ok == b_ok);
            model.push_back(next);
            ++next;
        } else {
            std::uint64_t a_val = 0;
            std::uint64_t b_val = 0;
            const bool    a_ok  = a.try_pop(a_val);
            const bool    b_ok  = b.try_pop(b_val);
            CHECK(a_ok);
            CHECK(a_ok == b_ok);
            CHECK(a_val == b_val);
            CHECK(a_val == model.front());
            model.pop_front();
        }
        CHECK(a.empty() == b.empty());
        CHECK(a.empty() == model.empty());
    }

    // Drain whatever the final ops left behind; both layouts and the model must
    // stay in lock-step to the end.
    while (!model.empty()) {
        std::uint64_t a_val = 0;
        std::uint64_t b_val = 0;
        CHECK(a.try_pop(a_val));
        CHECK(b.try_pop(b_val));
        CHECK(a_val == b_val);
        CHECK(a_val == model.front());
        model.pop_front();
    }
    CHECK(a.empty());
    CHECK(b.empty());
}

// ---------------------------------------------------------------------------
// Layout evidence tests. These are Phase-3A-specific and are the reason the
// experiment can attribute anything to cache-line placement at all.
// ---------------------------------------------------------------------------

// The compile-time assumption must be at least as large as the line the host
// actually reports. A host with a LARGER line would invalidate the separated
// control, and the experiment must fail rather than publish.
void test_host_line_size_is_supported() {
    const std::size_t reported = lltl::reported_cache_line_size();
    std::printf("  host reports cache-line size = %zu bytes; "
                "compile-time assumption = %zu bytes\n",
                reported, lltl::kAssumedCacheLineSize);
    CHECK(reported > 0); // 0 means "could not query" — never treated as a default
    CHECK(lltl::line_size_supported(reported));
    CHECK(reported <= lltl::kAssumedCacheLineSize);
}

// The same-line control must MEASURE as same-line on a real object.
void test_same_line_control_is_same_line_at_runtime() {
    auto q = std::make_unique<SpscSameLineRingBuffer<std::uint64_t, 4096>>();
    const auto r = q->cursor_layout_report(lltl::reported_cache_line_size());
    CHECK(r.layout == CursorLayout::SameLine);
    CHECK(r.head_address != r.tail_address); // distinct objects...
    CHECK(r.cursors_same_line);              // ...sharing one line
    CHECK(r.head_line_index == r.tail_line_index);
    CHECK(r.same_line_invariant_holds);
    CHECK(r.payload_disjoint);
    CHECK(r.ok());
}

// The separated control must MEASURE as separated on a real object.
void test_separated_control_is_separated_at_runtime() {
    auto q = std::make_unique<SpscSeparatedCursorRingBuffer<std::uint64_t, 4096>>();
    const auto r = q->cursor_layout_report(lltl::reported_cache_line_size());
    CHECK(r.layout == CursorLayout::Separated);
    CHECK(!r.cursors_same_line); // distinct objects in distinct lines
    CHECK(r.head_line_index != r.tail_line_index);
    CHECK(r.same_line_invariant_holds);
    CHECK(r.payload_disjoint);
    CHECK(r.ok());
}

// Both controls must keep holding if the host's real line is SMALLER than the
// 128-byte assumption. This is what makes 128 a conservative value: the cursors
// of the same-line block are 8 bytes apart, and the separated blocks are a full
// assumption apart, so 64/32/16-byte hosts still get the intended controls.
void test_invariants_hold_under_smaller_line_sizes() {
    auto same = std::make_unique<SpscSameLineRingBuffer<std::uint64_t, 1024>>();
    auto sep  = std::make_unique<SpscSeparatedCursorRingBuffer<std::uint64_t, 1024>>();

    const std::size_t smaller[] = {64, 32, 16};
    for (const std::size_t line : smaller) {
        const auto rs = same->cursor_layout_report(line);
        CHECK(rs.cursors_same_line);
        CHECK(rs.same_line_invariant_holds);
        CHECK(rs.payload_disjoint);

        const auto rp = sep->cursor_layout_report(line);
        CHECK(!rp.cursors_same_line);
        CHECK(rp.same_line_invariant_holds);
        CHECK(rp.payload_disjoint);
    }
}

// NEGATIVE CONTROL on the guard itself: a host reporting a line LARGER than the
// assumption must be rejected. Without this, the separated control could quietly
// become a same-line control on such a host and the experiment would report the
// opposite of the truth.
void test_layout_guard_rejects_unsupported_line_size() {
    constexpr std::size_t kTooLarge = 2 * lltl::kAssumedCacheLineSize;
    CHECK(!lltl::line_size_supported(kTooLarge));
    CHECK(!lltl::line_size_supported(0)); // "unknown" is also not supported

    auto sep = std::make_unique<SpscSeparatedCursorRingBuffer<std::uint64_t, 1024>>();
    const auto r = sep->cursor_layout_report(kTooLarge);
    CHECK(!r.line_size_supported);
    CHECK(!r.ok()); // must never be publishable under an invalid assumption
}

// The payload is not a second variable: both variants must present payload
// storage of the same size, starting on an interference boundary, and never
// sharing a line with a cursor.
void test_payload_layout_is_equivalent_across_variants() {
    const std::size_t line = lltl::reported_cache_line_size();
    CHECK(line > 0);

    auto same = std::make_unique<SpscSameLineRingBuffer<std::uint64_t, 1024>>();
    auto sep  = std::make_unique<SpscSeparatedCursorRingBuffer<std::uint64_t, 1024>>();

    const auto rs = same->cursor_layout_report(line);
    const auto rp = sep->cursor_layout_report(line);

    CHECK(rs.payload_end - rs.payload_begin == rp.payload_end - rp.payload_begin);
    CHECK(rs.payload_begin % line == 0); // starts on an interference boundary
    CHECK(rp.payload_begin % line == 0);
    CHECK(rs.payload_disjoint);
    CHECK(rp.payload_disjoint);
}

// ---------------------------------------------------------------------------
// Phase 3A.1 — equal footprint. The payload offset must not move between the
// two variants, or the experiment changes cursor placement AND payload layout at
// once. This is a build-time invariant in the header; it is asserted here too so
// that a reader checking the experiment's controls finds it, and it is checked
// again on live objects below.
// ---------------------------------------------------------------------------
void test_cursor_policy_footprints_are_equal() {
    CHECK(sizeof(SameLineCursorBlock) == sizeof(SeparatedCursorBlocks));
    CHECK(alignof(SameLineCursorBlock) == alignof(SeparatedCursorBlocks));
    CHECK(sizeof(SameLineCursorBlock) == 2 * lltl::kAssumedCacheLineSize);
    CHECK(sizeof(SeparatedCursorBlocks) == 2 * lltl::kAssumedCacheLineSize);

    // The treatment, stated as offsets rather than as prose: same-line keeps
    // BOTH cursors in the first assumed block; separated puts tail in the
    // second; head is at the same offset in both.
    CHECK(offsetof(SameLineCursorBlock, head) == 0);
    CHECK(offsetof(SameLineCursorBlock, tail) < lltl::kAssumedCacheLineSize);
    CHECK(offsetof(SeparatedCursorBlocks, head) == 0);
    CHECK(offsetof(SeparatedCursorBlocks, tail) >= lltl::kAssumedCacheLineSize);

    // The same-line block's padding is inert storage, not a place a cursor
    // could be moved into without the assertion above firing.
    CHECK(offsetof(SameLineCursorBlock, reserved) >=
          lltl::kAssumedCacheLineSize);
}

// Runtime equal-footprint check for one T/Capacity: the objects are the same
// size and the payload begins at the same offset from the object base, so the
// only thing that changed between the variants is what the cursor policy
// contributes.
template <typename T, std::size_t Capacity>
void check_footprint_agreement(const char* label) {
    const std::size_t line = lltl::reported_cache_line_size();

    auto same = std::make_unique<SpscSameLineRingBuffer<T, Capacity>>();
    auto sep  = std::make_unique<SpscSeparatedCursorRingBuffer<T, Capacity>>();

    CHECK(sizeof(SpscSameLineRingBuffer<T, Capacity>) ==
          sizeof(SpscSeparatedCursorRingBuffer<T, Capacity>));

    const auto rs = same->cursor_layout_report(line);
    const auto rp = sep->cursor_layout_report(line);

    CHECK(rs.object_size == rp.object_size);
    CHECK(rs.payload_offset_from_object_base ==
          rp.payload_offset_from_object_base);
    CHECK(lltl::footprints_agree(rs, rp));

    // The reported values must describe the object that was actually measured,
    // not a recomputed constant.
    CHECK(rs.object_address == reinterpret_cast<std::uintptr_t>(same.get()));
    CHECK(rp.object_address == reinterpret_cast<std::uintptr_t>(sep.get()));
    CHECK(rs.object_size == sizeof(*same));
    CHECK(rp.object_size == sizeof(*sep));
    CHECK(rs.payload_offset_from_object_base ==
          rs.payload_begin - rs.object_address);
    CHECK(rs.payload_offset_from_object_base >= sizeof(SameLineCursorBlock));
    CHECK(rs.payload_offset_from_object_base < rs.object_size);

    std::printf("  footprint %-28s same_line: size=%zu payload_offset=%zu | "
                "separated: size=%zu payload_offset=%zu\n",
                label, rs.object_size, rs.payload_offset_from_object_base,
                rp.object_size, rp.payload_offset_from_object_base);
}

// Trivially copyable payloads with the benchmark's exact sizes, so the shapes
// the canonical matrix runs are covered here too (the benchmark's own
// cross-variant gate is the runtime guard; this is the test-side statement).
struct Msg8Bytes {
    std::uint64_t seq = 0;
};
struct Msg64Bytes {
    std::uint64_t w[8] = {};
};
static_assert(sizeof(Msg8Bytes) == 8);
static_assert(sizeof(Msg64Bytes) == 64);

// Every (T, Capacity) shape the Phase-3A benchmark actually runs, plus the small
// shapes the tests use. A single shape with a mismatched payload offset would be
// enough to make that cell's result uninterpretable.
void test_object_footprint_and_payload_offset_are_equal() {
    check_footprint_agreement<Msg8Bytes, 1024>("8B/1024");
    check_footprint_agreement<Msg8Bytes, 4096>("8B/4096");
    check_footprint_agreement<Msg8Bytes, 65536>("8B/65536");
    check_footprint_agreement<MarketMessage, 1024>("32B/1024");
    check_footprint_agreement<MarketMessage, 4096>("32B/4096");
    check_footprint_agreement<MarketMessage, 65536>("32B/65536");
    check_footprint_agreement<Msg64Bytes, 1024>("64B/1024");
    check_footprint_agreement<Msg64Bytes, 4096>("64B/4096");
    check_footprint_agreement<Msg64Bytes, 65536>("64B/65536");
    check_footprint_agreement<std::uint64_t, 8>("u64/8");
    check_footprint_agreement<Message, 8>("Message/8");
}

// The frozen Phase-1 type is still the natural/unpadded baseline and is NOT one
// of the two Phase-3A controls. This asserts only that it is intact and usable;
// Phase 3A makes no causal claim from it.
void test_frozen_natural_baseline_untouched() {
    SpscRingBuffer<std::uint64_t, 1024> q;
    CHECK(q.empty());
    CHECK(q.try_push(1ull));
    std::uint64_t v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 1ull);
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// Exit-code self-test. A failed CHECK must produce a non-zero exit status.
// ---------------------------------------------------------------------------
void self_test_exit_code() {
    std::printf("  (self-test) intentionally failing one CHECK...\n");
    CHECK(1 == 2); // must trip
}

} // namespace

int main() {
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("exit-code self-test: expecting non-zero exit\n");
        self_test_exit_code();
        const int n = summary("self-test (expected FAIL)");
        return n == 0 ? 0 : 1;
    }

    int total = 0;

    // --- Phase-3A layout evidence (runs first: if the controls are not what
    //     they claim, nothing below is evidence about cache lines) -----------
    test_host_line_size_is_supported();
    total += summary("layout: host line size supported by assumption");

    test_same_line_control_is_same_line_at_runtime();
    total += summary("layout: same-line control IS same-line at runtime");

    test_separated_control_is_separated_at_runtime();
    total += summary("layout: separated control IS separated at runtime");

    test_invariants_hold_under_smaller_line_sizes();
    total += summary("layout: invariants hold for smaller real lines");

    test_layout_guard_rejects_unsupported_line_size();
    total += summary("layout: guard rejects unsupported line size");

    test_payload_layout_is_equivalent_across_variants();
    total += summary("layout: payload equivalent across variants");

    test_cursor_policy_footprints_are_equal();
    total += summary("layout: cursor policy footprints equal");

    test_object_footprint_and_payload_offset_are_equal();
    total += summary("layout: object size / payload offset equal per T+cap");

    test_variants_agree_under_identical_operation_sequence();
    total += summary("differential: variants agree on identical ops");

    test_frozen_natural_baseline_untouched();
    total += summary("frozen Phase-1 natural baseline still usable");

    // --- Phase-1 correctness protocol, both layouts ------------------------
    RUN_BOTH("new queue is empty", t_new_queue_empty);
    RUN_BOTH("pop empty returns false", t_pop_empty_returns_false);
    RUN_BOTH("push one / pop one", t_push_pop_one);
    RUN_BOTH("FIFO ordering", t_fifo_order);
    RUN_BOTH("fill exactly Capacity, push-when-full rejected",
             t_fill_exactly_capacity_then_full_reject);
    RUN_BOTH("pop after full drains in order", t_pop_after_full_drains_in_order);
    RUN_BOTH("physical wrap-around (many fill/drain cycles)",
             t_wrap_around_repeatedly);
    RUN_BOTH("interleaved fill/drain vs deque model",
             t_interleaved_fill_drain_cycles);
    RUN_BOTH("payload: sequence values preserved",
             t_payload_sequence_values_preserved);
    RUN_BOTH("payload: structured message copied", t_structured_message_copy);
    RUN_BOTH("payload: structured message moved", t_structured_message_move);
    RUN_BOTH("payload: copy vs move overloads exercised",
             t_copy_vs_move_overloads);
    RUN_BOTH("concurrent: large deterministic sequence",
             t_concurrent_producer_consumer);
    RUN_BOTH("concurrent: rapid slot reuse (Capacity = 2)",
             t_concurrent_rapid_slot_reuse);
    RUN_BOTH("concurrent: multi-field payload publication",
             t_concurrent_multi_field_payload);
    RUN_BOTH("reference differential (mutex vs variant vs model)",
             t_reference_differential);

    std::printf("%s\n", total == 0 ? "spsc false sharing: all ok"
                                   : "spsc false sharing: FAILURES");
    return total == 0 ? 0 : 1;
}
