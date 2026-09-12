// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3B correctness tests.
//
// Phase 3B changes exactly one thing about the Phase-3A separated SPSC ring
// buffer: how OFTEN each thread performs the acquire load of the opposite
// thread's cursor. Its correctness burden is therefore four-fold, and all four
// halves are exercised here.
//
// (1) SEMANTIC EQUIVALENCE. Both Phase-3B variants must pass the SAME Phase-1
//     correctness protocol as the frozen SpscRingBuffer and the Phase-3A
//     separated control. Every single-thread semantic test, every concurrent
//     test, and the reference differential against MutexBoundedQueue is run
//     TWICE — once per remote-cursor mode — from one templated body, so the two
//     variants cannot drift apart in what is asserted about them.
//
// (2) LAYOUT EVIDENCE. Phase 3B must not disturb the layout Phase 3A
//     established. Cursor placement stays SEPARATED and is re-verified at
//     runtime on a real object for BOTH modes; the two variants instantiate the
//     same cursor policy, so their object size and payload offset are identical
//     by construction and are checked on live objects anyway. Phase 3B adds one
//     new layout claim of its own — the cached remote cursors must sit on their
//     OWNER's interference block and never on the remote cursor's block — and
//     that claim is checked here, including a NEGATIVE CONTROL proving the check
//     can fail.
//
// (3) REFRESH MECHANISM. The point of the experiment is that the cached variant
//     performs materially fewer remote cursor loads. That is a property of the
//     code, so it is tested as one: exact counts where the operation sequence is
//     deterministic (single-threaded), and provable bounds rather than measured
//     ratios everywhere. A test that only asserted "the cached variant was
//     faster" would be a benchmark, not a correctness test.
//
// (4) COUNTER WRAP. Phase 3B adds comparisons against a THREAD-OWNED cached
//     value, so the modular arithmetic that makes the Phase-1/3A full/empty
//     tests exact has to keep working for the cached tests too. The counters are
//     monotonic unsigned; the wrap boundary cannot be reached by executing 2^64
//     operations, so the queue is SEEDED one short of the boundary and the tests
//     then push and pop ACROSS it through the ordinary API. The modular
//     inequality the cached check relies on is additionally tested directly, as
//     arithmetic, over values straddling the boundary.
//
// The suite doubles as an exit-code self-test (LLDB_SELFTEST_FAIL), matching the
// Phase-1 and Phase-3A suites.
//
// A plain CHECK macro reports the file/line of the first failing assertion;
// failures accumulate into a total and main() returns non-zero on any failure,
// so CTest genuinely fails on a bad run. No external test framework is used.

#include "cache_line.h"
#include "mutex_bounded_queue.h"
#include "spsc_cursor_layout_ring_buffer.h"
#include "spsc_remote_cursor_ring_buffer.h"
#include "spsc_ring_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using lltl::CursorLayout;
using lltl::CursorLayoutRingBuffer;
using lltl::MutexBoundedQueue;
using lltl::RemoteCursorMode;
using lltl::RemoteCursorRingBuffer;
using lltl::SeparatedCursorBlocks;
using lltl::SpscSeparatedBaselineInstrumented;
using lltl::SpscSeparatedBaselineRingBuffer;
using lltl::SpscSeparatedCachedCursorRingBuffer;
using lltl::SpscSeparatedCachedInstrumented;
using lltl::SpscSeparatedCursorRingBuffer;
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
        std::printf("[ok] %-56s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-56s (%d/%d checks FAILED)\n", suite, n_fail,
                    g_checks);
    }
    g_failures = 0;
    g_checks   = 0;
    return n_fail;
}

// The Phase-3B aliases for a given mode, so one templated body can be run once
// per mode without the body naming either variant.
template <RemoteCursorMode Mode, typename T, std::size_t Capacity>
using QueueFor = RemoteCursorRingBuffer<T, Capacity, Mode, false>;

template <RemoteCursorMode Mode, typename T, std::size_t Capacity>
using InstrumentedQueueFor = RemoteCursorRingBuffer<T, Capacity, Mode, true>;

// Runs a templated test body once per remote-cursor mode and reports each as its
// own named suite. Both variants are always exercised together, so a test can
// never be silently applied to only one of the two controls.
#define RUN_BOTH_MODES(name, fn)                               \
    do {                                                       \
        fn<RemoteCursorMode::Direct>();                        \
        total += summary("baseline: " name);                   \
        fn<RemoteCursorMode::Cached>();                        \
        total += summary("cached:   " name);                   \
    } while (0)

// ---------------------------------------------------------------------------
// Payload types (the Phase-1 / Phase-3A shapes, unchanged).
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

// Seeding a queue at a chosen counter value, spelled once.
template <RemoteCursorMode Mode, typename T, std::size_t Capacity>
QueueFor<Mode, T, Capacity> make_seeded_queue(std::size_t counter) {
    using Q = QueueFor<Mode, T, Capacity>;
    return Q(typename Q::SeedEmptyAtCounterForTest{counter});
}

// ---------------------------------------------------------------------------
// Single-thread semantic tests, run against BOTH modes.
// ---------------------------------------------------------------------------
template <RemoteCursorMode Mode>
void t_new_queue_empty() {
    QueueFor<Mode, int, 8> q;
    CHECK(q.empty());
    CHECK(q.capacity() == 8);
}

template <RemoteCursorMode Mode>
void t_pop_empty_returns_false() {
    QueueFor<Mode, int, 8> q;
    int v = 123;
    CHECK(!q.try_pop(v));
    CHECK(v == 123); // out must be untouched on failure
    CHECK(q.empty());
}

template <RemoteCursorMode Mode>
void t_push_pop_one() {
    QueueFor<Mode, int, 8> q;
    CHECK(q.try_push(42));
    CHECK(!q.empty());
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 42);
    CHECK(q.empty());
}

template <RemoteCursorMode Mode>
void t_fifo_order() {
    QueueFor<Mode, int, 8> q;
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

template <RemoteCursorMode Mode>
void t_fill_exactly_capacity_then_full_reject() {
    QueueFor<Mode, int, 8> q;
    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_push(i));
    }
    CHECK(!q.empty());
    CHECK(q.try_push(100) == false); // full: head - tail == Capacity
    int v = -1;
    CHECK(q.try_pop(v));
    CHECK(v == 0);
}

// Repeating the rejected push must keep rejecting: a cached value that is
// refreshed on the failure path must not let a full queue drift into accepting.
template <RemoteCursorMode Mode>
void t_repeated_push_when_full_never_succeeds() {
    QueueFor<Mode, int, 8> q;
    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_push(i));
    }
    for (int attempt = 0; attempt < 50; ++attempt) {
        CHECK(!q.try_push(1000 + attempt));
    }
    int v = -1;
    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_pop(v));
        CHECK(v == i);
    }
    CHECK(q.empty());
    CHECK(!q.try_pop(v));
}

template <RemoteCursorMode Mode>
void t_pop_after_full_drains_in_order() {
    QueueFor<Mode, int, 8> q;
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

// Repeating the rejected pop must keep rejecting: the consumer's cached copy of
// head must not become a reason to read a slot that was never published.
template <RemoteCursorMode Mode>
void t_repeated_pop_when_empty_never_succeeds() {
    QueueFor<Mode, int, 8> q;
    int v = -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        CHECK(!q.try_pop(v));
        CHECK(v == -1);
    }
    CHECK(q.try_push(7));
    CHECK(q.try_pop(v));
    CHECK(v == 7);
    CHECK(q.empty());
    for (int attempt = 0; attempt < 50; ++attempt) {
        CHECK(!q.try_pop(v));
    }
}

// Wrap the PHYSICAL ring many times: each slot is overwritten and re-read
// repeatedly while the monotonic counters grow far past Capacity. Every cycle
// alternates between full and empty, which forces both cached values to go stale
// and be refreshed, and forces both full/empty tests to be exercised.
template <RemoteCursorMode Mode>
void t_wrap_around_repeatedly() {
    QueueFor<Mode, std::uint64_t, 8> q;
    constexpr int kCycles = 100;
    std::uint64_t next    = 0;
    for (int c = 0; c < kCycles; ++c) {
        for (int i = 0; i < 8; ++i) {
            CHECK(q.try_push(next + static_cast<std::uint64_t>(i)));
        }
        CHECK(!q.try_push(0xDEADull)); // full, every cycle
        for (int i = 0; i < 8; ++i) {
            std::uint64_t v = 0;
            CHECK(q.try_pop(v));
            CHECK(v == next);
            ++next;
        }
        std::uint64_t v = 0;
        CHECK(!q.try_pop(v)); // empty, every cycle
    }
    CHECK(next == static_cast<std::uint64_t>(kCycles * 8));
    CHECK(q.empty());
}

// Partially filled so the ring wraps at a non-zero, VARYING occupancy: the
// cached values then go stale at irregular points rather than only at the
// full/empty extremes. A deque is the source of truth, so a cycle whose pop
// count exceeds the current occupancy drains the queue and stops there rather
// than reading a slot that was never published.
template <RemoteCursorMode Mode>
void t_wrap_around_at_partial_occupancy() {
    QueueFor<Mode, int, 8> q;
    std::deque<int>       model;
    int                   produced = 0;

    for (int cycle = 0; cycle < 300; ++cycle) {
        const int push_n = (cycle % 3) + 1;
        for (int i = 0; i < push_n; ++i) {
            CHECK(q.try_push(produced));
            model.push_back(produced);
            ++produced;
        }
        const int pop_n = (cycle % 4) + 1;
        for (int i = 0; i < pop_n; ++i) {
            if (model.empty()) {
                int v = -1;
                CHECK(!q.try_pop(v)); // empty: must refuse, not fabricate
                break;
            }
            int v = -1;
            CHECK(q.try_pop(v));
            CHECK(v == model.front());
            model.pop_front();
        }
        CHECK(q.empty() == model.empty());
    }

    while (!model.empty()) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == model.front());
        model.pop_front();
    }
    CHECK(q.empty());
    int v = -1;
    CHECK(!q.try_pop(v));
}

// Fill/drain cycles at varying occupancy, with a std::deque as source of truth.
template <RemoteCursorMode Mode>
void t_interleaved_fill_drain_cycles() {
    QueueFor<Mode, int, 8> q;
    std::deque<int>       model;
    std::uint64_t         rng    = 0x9E3779B97F4A7C15ull; // fixed seed
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

template <RemoteCursorMode Mode>
void t_payload_sequence_values_preserved() {
    QueueFor<Mode, std::uint64_t, 16> q;
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

template <RemoteCursorMode Mode>
void t_structured_message_copy() {
    QueueFor<Mode, Message, 4> q;
    Message src(7, "bid update");
    CHECK(q.try_push(src));         // const& overload -> must COPY
    src.text = "mutated after push"; // must not affect the enqueued copy
    Message out;
    CHECK(q.try_pop(out));
    CHECK(out.seq == 7);
    CHECK(out.text == "bid update");
}

template <RemoteCursorMode Mode>
void t_structured_message_move() {
    QueueFor<Mode, Message, 4> q;
    Message tmp(9, "ask update");
    CHECK(q.try_push(std::move(tmp))); // && overload -> must MOVE
    Message out;
    CHECK(q.try_pop(out));
    CHECK(out.seq == 9);
    CHECK(out.text == "ask update");
}

template <RemoteCursorMode Mode>
void t_copy_vs_move_overloads() {
    CopyMoveProbe::copies = 0;
    CopyMoveProbe::moves  = 0;
    {
        QueueFor<Mode, CopyMoveProbe, 4> q;
        CopyMoveProbe                    a(1);
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
// Concurrent tests, run against BOTH modes.
// ---------------------------------------------------------------------------
template <RemoteCursorMode Mode>
void t_concurrent_producer_consumer() {
    constexpr std::size_t   kCapacity = 1024;
    constexpr std::uint64_t kMessages = 1ull << 20;

    QueueFor<Mode, std::uint64_t, kCapacity> q;
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
// over and over while the consumer reads them. The sharpest slot-reuse edge, and
// the sharpest test of the cached producer's "may be full" refresh.
template <RemoteCursorMode Mode>
void t_concurrent_rapid_slot_reuse() {
    constexpr std::size_t   kCapacity = 2;
    constexpr std::uint64_t kMessages = 400'000;

    QueueFor<Mode, std::uint64_t, kCapacity> q;
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
// payload. Phase 3B matters here because the consumer's acquire load of head is
// now performed LESS OFTEN — the payload it guards must still be fully visible
// when the cached value says data is available.
template <RemoteCursorMode Mode>
void t_concurrent_multi_field_payload() {
    constexpr std::size_t   kCapacity = 4;
    constexpr std::uint64_t kMessages = 200'000;

    QueueFor<Mode, MarketMessage, kCapacity> q;
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
// step, exactly as in the Phase-1 and Phase-3A suites (now for the Phase-3B
// type, in both modes).
template <RemoteCursorMode Mode>
void t_reference_differential() {
    constexpr std::size_t kCapacity = 64;
    MutexBoundedQueue<std::uint64_t, kCapacity> mq;
    QueueFor<Mode, std::uint64_t, kCapacity>    sq;
    std::deque<std::uint64_t>                   model;

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
// Variant-vs-variant differential. The "only remote-load frequency differs"
// claim, stated as something a test can fail on. An identical deterministic
// operation sequence is fed to the Phase-3B baseline, the Phase-3B cached
// variant, the Phase-3A SEPARATED control and a deque model; every return value
// and every payload value must agree.
//
// Including the Phase-3A separated control is deliberate: it is the algorithm
// the Phase-3B baseline claims to be, and this is where that claim is checked.
// ---------------------------------------------------------------------------
void test_all_variants_agree_under_identical_operation_sequence() {
    constexpr std::size_t kCapacity = 64;
    SpscSeparatedBaselineRingBuffer<std::uint64_t, kCapacity>     baseline;
    SpscSeparatedCachedCursorRingBuffer<std::uint64_t, kCapacity> cached;
    SpscSeparatedCursorRingBuffer<std::uint64_t, kCapacity>       phase3a;
    std::deque<std::uint64_t>                                     model;

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
            const bool a_ok = baseline.try_push(next);
            const bool b_ok = cached.try_push(next);
            const bool c_ok = phase3a.try_push(next);
            CHECK(a_ok);
            CHECK(a_ok == b_ok);
            CHECK(a_ok == c_ok);
            model.push_back(next);
            ++next;
        } else {
            std::uint64_t a_val = 0;
            std::uint64_t b_val = 0;
            std::uint64_t c_val = 0;
            const bool    a_ok  = baseline.try_pop(a_val);
            const bool    b_ok  = cached.try_pop(b_val);
            const bool    c_ok  = phase3a.try_pop(c_val);
            CHECK(a_ok);
            CHECK(a_ok == b_ok);
            CHECK(a_ok == c_ok);
            CHECK(a_val == b_val);
            CHECK(a_val == c_val);
            CHECK(a_val == model.front());
            model.pop_front();
        }
        CHECK(baseline.empty() == cached.empty());
        CHECK(baseline.empty() == phase3a.empty());
        CHECK(baseline.empty() == model.empty());
    }

    while (!model.empty()) {
        std::uint64_t a_val = 0;
        std::uint64_t b_val = 0;
        std::uint64_t c_val = 0;
        CHECK(baseline.try_pop(a_val));
        CHECK(cached.try_pop(b_val));
        CHECK(phase3a.try_pop(c_val));
        CHECK(a_val == b_val);
        CHECK(a_val == c_val);
        CHECK(a_val == model.front());
        model.pop_front();
    }
    CHECK(baseline.empty());
    CHECK(cached.empty());
    CHECK(phase3a.empty());
}

// ---------------------------------------------------------------------------
// Layout evidence. Phase 3B must leave the Phase-3A separated control exactly
// where it was, and must not add a new cross-thread sharing relationship.
// ---------------------------------------------------------------------------

// Both modes must MEASURE as separated at runtime, on a real object.
template <RemoteCursorMode Mode>
void t_both_modes_are_separated_at_runtime() {
    auto q = std::make_unique<QueueFor<Mode, std::uint64_t, 4096>>();
    const auto r = q->cursor_layout_report(lltl::reported_cache_line_size());

    CHECK(r.layout() == CursorLayout::Separated);
    CHECK(!r.cursor.cursors_same_line);
    CHECK(r.head_line_index() != r.tail_line_index());
    CHECK(r.cursor.same_line_invariant_holds);
    CHECK(r.cursor.payload_disjoint);
    CHECK(r.ok());
}

// The cached remote cursors must sit on their OWNER's interference block, and
// must NOT share a block with the remote cursor. This is the Phase-3B-specific
// layout claim: it is what makes "no new cross-thread sharing was introduced"
// something the experiment verified rather than something it asserted.
void test_cached_state_placement_is_verified() {
    auto q = std::make_unique<SpscSeparatedCachedCursorRingBuffer<std::uint64_t, 4096>>();
    const std::size_t line = lltl::reported_cache_line_size();
    const auto        r    = q->cursor_layout_report(line);

    CHECK(r.cached_tail_address != 0);
    CHECK(r.cached_head_address != 0);
    CHECK(r.cached_tail_address != r.cached_head_address);

    // cached_tail with head (producer's own line), cached_head with tail.
    CHECK(r.cached_tail_colocated_with_owner);
    CHECK(r.cached_head_colocated_with_owner);
    CHECK(r.cached_tail_line_index == r.head_line_index());
    CHECK(r.cached_head_line_index == r.tail_line_index());

    // ...and therefore NOT with the remote cursor.
    CHECK(r.cached_state_disjoint_from_remote_cursor);
    CHECK(r.cached_tail_line_index != r.tail_line_index());
    CHECK(r.cached_head_line_index != r.head_line_index());

    CHECK(r.ok_cached());
    CHECK(r.ok());

    std::printf("  cached placement: head line %zu + cached_tail line %zu | "
                "tail line %zu + cached_head line %zu\n",
                r.head_line_index(), r.cached_tail_line_index,
                r.tail_line_index(), r.cached_head_line_index);
}

// NEGATIVE CONTROL on the Phase-3B layout check itself. If the check could not
// fail, "cached placement PASS" would be evidence of nothing. The verdict is a
// pure function of the four reported addresses, so it can be driven with
// addresses that deliberately violate each clause and must reject them.
void test_cached_placement_check_can_fail() {
    const std::size_t line = lltl::reported_cache_line_size();
    CHECK(line > 0);

    auto q = std::make_unique<SpscSeparatedCachedCursorRingBuffer<std::uint64_t, 1024>>();
    const auto good = q->cursor_layout_report(line);
    CHECK(good.ok_cached()); // the real object passes

    // Recompute the verdict from four addresses, exactly as the report does, so
    // a doctored report can be fed to the same predicate.
    const auto verdict = [line](std::uintptr_t cached_tail_addr,
                                std::uintptr_t head_addr,
                                std::uintptr_t cached_head_addr,
                                std::uintptr_t tail_addr) {
        return lltl::same_cache_line(cached_tail_addr, head_addr, line) &&
               lltl::same_cache_line(cached_head_addr, tail_addr, line) &&
               !lltl::same_cache_line(cached_tail_addr, tail_addr, line) &&
               !lltl::same_cache_line(cached_head_addr, head_addr, line);
    };

    // As reported: passes.
    CHECK(verdict(good.cached_tail_address, good.head_address(),
                  good.cached_head_address, good.tail_address()));

    // The violation Phase 3B is specifically guarding against: a cached value
    // sitting on the REMOTE thread's cursor line, which would be a new
    // cross-thread sharing relationship. Both directions must be rejected.
    CHECK(!verdict(good.tail_address(), good.head_address(),
                   good.cached_head_address, good.tail_address()));
    CHECK(!verdict(good.cached_tail_address, good.head_address(),
                   good.head_address(), good.tail_address()));

    // And a cached value that is on NEITHER cursor's line — it drifted off its
    // owner's block — must be rejected too, so "colocated with owner" is a real
    // requirement rather than a restatement of the disjointness one.
    CHECK(!verdict(good.payload_begin(), good.head_address(),
                   good.cached_head_address, good.tail_address()));

    // Sanity: the two cursor lines really are distinct and the cached values
    // really are colocated, so "same cache line" is a discriminating test here
    // rather than one that is always true or always false.
    CHECK(!lltl::same_cache_line(good.head_address(), good.tail_address(), line));
    CHECK(lltl::same_cache_line(good.head_address(), good.cached_tail_address,
                                line));
    CHECK(!lltl::same_cache_line(good.payload_begin(), good.head_address(),
                                 line));
}

// Both modes instantiate the SAME cursor policy, so object size and payload
// offset are identical by construction. Checked on live objects anyway, because
// the canonical runner re-checks it in the raw data and a construction-level
// identity that was never executed is not evidence.
void test_both_modes_have_identical_footprint() {
    const std::size_t line = lltl::reported_cache_line_size();

    auto a = std::make_unique<SpscSeparatedBaselineRingBuffer<std::uint64_t, 1024>>();
    auto b = std::make_unique<SpscSeparatedCachedCursorRingBuffer<std::uint64_t, 1024>>();

    CHECK(sizeof(*a) == sizeof(*b));

    const auto ra = a->cursor_layout_report(line);
    const auto rb = b->cursor_layout_report(line);

    CHECK(ra.object_size() == rb.object_size());
    CHECK(ra.payload_offset_from_object_base() ==
          rb.payload_offset_from_object_base());
    CHECK(lltl::remote_footprints_agree(ra, rb));
}

// Phase 3B reuses the Phase-3A cursor policy footprint, so the payload keeps the
// offset Phase 3A verified. This is what makes Phase 3B a different TREATMENT
// rather than a different object layout.
template <typename T, std::size_t Capacity>
void check_phase3b_footprint_matches_phase3a(const char* label) {
    auto b = std::make_unique<SpscSeparatedBaselineRingBuffer<T, Capacity>>();
    auto a = std::make_unique<SpscSeparatedCursorRingBuffer<T, Capacity>>();

    CHECK(sizeof(*b) == sizeof(*a));

    const std::size_t line = lltl::reported_cache_line_size();
    const auto        rb   = b->cursor_layout_report(line);
    const auto        ra   = a->cursor_layout_report(line);

    CHECK(rb.object_size() == ra.object_size);
    CHECK(rb.payload_offset_from_object_base() ==
          ra.payload_offset_from_object_base);
    CHECK(lltl::footprints_agree(rb.cursor, ra));

    // Payload storage is the same size and starts on an interference boundary.
    CHECK(rb.cursor.payload_end - rb.cursor.payload_begin ==
          ra.payload_end - ra.payload_begin);
    CHECK(rb.cursor.payload_begin % line == 0);

    std::printf("  footprint %-20s phase3b size=%zu payload_offset=%zu | "
                "phase3a size=%zu payload_offset=%zu\n",
                label, rb.object_size(), rb.payload_offset_from_object_base(),
                ra.object_size, ra.payload_offset_from_object_base);
}

void test_footprint_matches_phase3a_for_every_benchmark_shape() {
    struct Msg8 {
        std::uint64_t seq = 0;
    };
    struct Msg64 {
        std::uint64_t w[8] = {};
    };
    static_assert(sizeof(Msg8) == 8);
    static_assert(sizeof(Msg64) == 64);

    check_phase3b_footprint_matches_phase3a<Msg8, 1024>("8B/1024");
    check_phase3b_footprint_matches_phase3a<Msg8, 4096>("8B/4096");
    check_phase3b_footprint_matches_phase3a<Msg8, 65536>("8B/65536");
    check_phase3b_footprint_matches_phase3a<MarketMessage, 1024>("32B/1024");
    check_phase3b_footprint_matches_phase3a<MarketMessage, 4096>("32B/4096");
    check_phase3b_footprint_matches_phase3a<MarketMessage, 65536>("32B/65536");
    check_phase3b_footprint_matches_phase3a<Msg64, 1024>("64B/1024");
    check_phase3b_footprint_matches_phase3a<Msg64, 4096>("64B/4096");
    check_phase3b_footprint_matches_phase3a<Msg64, 65536>("64B/65536");
}

// The compile-time layout guarantees, restated where the experiment's claims are
// checked. A build that broke any of these would not compile; asserting them
// here is what makes the requirement visible to a reader.
void test_cursor_policy_layout_assertions() {
    using P = lltl::SeparatedRemoteCursorBlock;

    CHECK(sizeof(P) == 2 * lltl::kAssumedCacheLineSize);
    CHECK(sizeof(P) == sizeof(SeparatedCursorBlocks)); // equal to Phase 3A
    CHECK(alignof(P) >= lltl::kAssumedCacheLineSize);
    CHECK(offsetof(P, head) == 0);
    CHECK(offsetof(P, tail) >= lltl::kAssumedCacheLineSize);
    CHECK(offsetof(P, tail) < 2 * lltl::kAssumedCacheLineSize);

    // Thread-owned state on its owner's line, never on the remote cursor's.
    CHECK(offsetof(P, cached_tail) < lltl::kAssumedCacheLineSize);
    CHECK(offsetof(P, cached_head) >= lltl::kAssumedCacheLineSize);
    CHECK(offsetof(P, producer_remote_tail_loads) < lltl::kAssumedCacheLineSize);
    CHECK(offsetof(P, consumer_remote_head_loads) >=
          lltl::kAssumedCacheLineSize);
}

// The layout-report assembly duplicated into the Phase-3B header must agree,
// field for field, with the Phase-3A member function for the same object. This
// is what stops the duplication from drifting into two different definitions of
// the experiment's control.
void test_phase3b_report_builder_matches_phase3a() {
    const std::size_t line = lltl::reported_cache_line_size();

    auto q = std::make_unique<SpscSeparatedCursorRingBuffer<std::uint64_t, 4096>>();
    const auto phase3a = q->cursor_layout_report(line);
    const auto rebuilt = lltl::build_cursor_layout_report(
        CursorLayout::Separated, line, q->cursor_head_address(),
        q->cursor_tail_address(), q->payload_begin_address(),
        q->payload_end_address(), q->object_address(), q->object_size());

    CHECK(rebuilt.layout == phase3a.layout);
    CHECK(rebuilt.reported_line_size == phase3a.reported_line_size);
    CHECK(rebuilt.layout_alignment == phase3a.layout_alignment);
    CHECK(rebuilt.head_address == phase3a.head_address);
    CHECK(rebuilt.tail_address == phase3a.tail_address);
    CHECK(rebuilt.head_line_index == phase3a.head_line_index);
    CHECK(rebuilt.tail_line_index == phase3a.tail_line_index);
    CHECK(rebuilt.payload_begin == phase3a.payload_begin);
    CHECK(rebuilt.payload_end == phase3a.payload_end);
    CHECK(rebuilt.object_address == phase3a.object_address);
    CHECK(rebuilt.object_size == phase3a.object_size);
    CHECK(rebuilt.payload_offset_from_object_base ==
          phase3a.payload_offset_from_object_base);
    CHECK(rebuilt.cursors_same_line == phase3a.cursors_same_line);
    CHECK(rebuilt.cursors_share_payload_line ==
          phase3a.cursors_share_payload_line);
    CHECK(rebuilt.line_size_supported == phase3a.line_size_supported);
    CHECK(rebuilt.same_line_invariant_holds ==
          phase3a.same_line_invariant_holds);
    CHECK(rebuilt.payload_disjoint == phase3a.payload_disjoint);
    CHECK(rebuilt.ok() == phase3a.ok());
}

// NEGATIVE CONTROL on the line-size guard, inherited from Phase 3A and re-run
// here: a host reporting a line LARGER than the assumption must be rejected, or
// the "separated" control could quietly become a same-line control.
void test_layout_guard_rejects_unsupported_line_size() {
    constexpr std::size_t kTooLarge = 2 * lltl::kAssumedCacheLineSize;
    CHECK(!lltl::line_size_supported(kTooLarge));
    CHECK(!lltl::line_size_supported(0));

    auto q = std::make_unique<SpscSeparatedCachedCursorRingBuffer<std::uint64_t, 1024>>();
    const auto r = q->cursor_layout_report(kTooLarge);
    CHECK(!r.cursor.line_size_supported);
    CHECK(!r.ok());
}

// Both modes must keep the phase-3A invariants if the host's real line is
// SMALLER than the 128-byte assumption.
template <RemoteCursorMode Mode>
void t_invariants_hold_under_smaller_line_sizes() {
    auto q = std::make_unique<QueueFor<Mode, std::uint64_t, 1024>>();

    const std::size_t smaller[] = {64, 32, 16};
    for (const std::size_t line : smaller) {
        const auto r = q->cursor_layout_report(line);
        CHECK(!r.cursor.cursors_same_line);
        CHECK(r.cursor.same_line_invariant_holds);
        CHECK(r.cursor.payload_disjoint);
        CHECK(r.cached_tail_colocated_with_owner);
        CHECK(r.cached_head_colocated_with_owner);
        CHECK(r.cached_state_disjoint_from_remote_cursor);
        CHECK(r.ok());
    }
}

// ---------------------------------------------------------------------------
// REFRESH MECHANISM — the tests that make "the cached variant performs fewer
// remote cursor loads" a property of the code rather than a hope.
//
// The single-threaded cases below are fully deterministic, so the counts can be
// asserted EXACTLY. Where a bound is more honest than a number, the bound is a
// provable consequence of the algorithm rather than a measured ratio.
// ---------------------------------------------------------------------------

// Baseline: exactly one remote load per try_push / try_pop CALL, successful or
// not. That is what "the baseline reads the opposite cursor very frequently"
// means, stated as a number the test can fail on.
void test_baseline_performs_one_remote_load_per_call() {
    SpscSeparatedBaselineInstrumented<std::uint64_t, 8> q;

    std::uint64_t push_calls = 0;
    std::uint64_t pop_calls  = 0;
    std::uint64_t v          = 0;

    for (int i = 0; i < 8; ++i) {
        ++push_calls;
        CHECK(q.try_push(static_cast<std::uint64_t>(i)));
    }
    for (int attempt = 0; attempt < 5; ++attempt) { // full: still counts
        ++push_calls;
        CHECK(!q.try_push(99ull));
    }
    for (int i = 0; i < 8; ++i) {
        ++pop_calls;
        CHECK(q.try_pop(v));
    }
    for (int attempt = 0; attempt < 5; ++attempt) { // empty: still counts
        ++pop_calls;
        CHECK(!q.try_pop(v));
    }

    CHECK(q.producer_remote_tail_loads() == push_calls);
    CHECK(q.consumer_remote_head_loads() == pop_calls);
}

// Cached producer: filling an EMPTY queue to exactly Capacity requires ZERO
// remote tail loads, because the cached value (0, matching the initial tail) can
// only be too small, and a too-small cached tail cannot make the queue look full.
// The exact count is asserted, not a bound.
void test_cached_producer_needs_no_remote_load_to_fill_an_empty_queue() {
    SpscSeparatedCachedInstrumented<std::uint64_t, 1024> q;

    for (int i = 0; i < 1024; ++i) {
        CHECK(q.try_push(static_cast<std::uint64_t>(i)));
    }
    CHECK(q.producer_remote_tail_loads() == 0);

    // One attempted push BEYOND capacity is what first requires a refresh — and
    // exactly one, after which the cached tail is current and the queue is
    // correctly reported full.
    CHECK(!q.try_push(1024ull));
    CHECK(q.producer_remote_tail_loads() == 1);

    // Repeating the rejected push DOES refresh each time, and that is the
    // correct behaviour rather than an inefficiency to hide: the cached tail is
    // already current (the consumer has not advanced it), so it still says
    // "may be full", and the algorithm's contract is to refresh on that path.
    // The property worth asserting is that no refresh happens on the SUCCESS
    // path — a producer that can push never reads the remote cursor.
    for (int attempt = 0; attempt < 20; ++attempt) {
        CHECK(!q.try_push(2000ull));
    }
    CHECK(q.producer_remote_tail_loads() == 21); // 1 + 20 may-be-full attempts

    // Once the consumer makes room, the producer still sees the STALE cached
    // tail (0, i.e. "may be full"), so it refreshes — and that one refresh is
    // what lets the push through. This is the cost model of the cached variant
    // in one line: refreshes are paid on the may-be-full path, not per push.
    std::uint64_t v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 0);
    CHECK(q.try_push(9999ull)); // succeeds
    CHECK(q.producer_remote_tail_loads() == 22); // one refresh, then the push
}

// Cached consumer: draining a full queue costs one refresh to learn where head
// is, then no further remote loads until the head value it learned is exhausted.
void test_cached_consumer_learns_head_once_per_epoch() {
    SpscSeparatedCachedInstrumented<std::uint64_t, 1024> q;
    for (int i = 0; i < 1024; ++i) {
        CHECK(q.try_push(static_cast<std::uint64_t>(i)));
    }

    std::uint64_t v = 0;
    for (int i = 0; i < 1024; ++i) {
        CHECK(q.try_pop(v));
        CHECK(v == static_cast<std::uint64_t>(i));
    }
    // Draining 1024 messages cost exactly ONE remote head load: the initial
    // refresh learns head == 1024, and every subsequent pop is authorized by
    // that single learned value. The baseline performs 1024 loads to do the same
    // work.
    CHECK(q.consumer_remote_head_loads() == 1);

    // The queue is now drained, but the consumer only discovers that by
    // refreshing once more — the cached head equals its own tail, which is
    // "maybe empty" rather than "empty".
    CHECK(!q.try_pop(v));
    CHECK(q.consumer_remote_head_loads() == 2);

    // The refreshed value was current, so repeating the failed pop refreshes
    // again: the contract is to re-read on the may-be-empty path.
    CHECK(!q.try_pop(v));
    CHECK(q.consumer_remote_head_loads() == 3);
}

// Burst pattern: the producer and consumer each advance in long runs, which is
// exactly the shape the cache is for. The cached refresh count is asserted
// against the LOGICAL bound of one refresh per Capacity-sized epoch, not against
// a measured ratio.
void test_cached_refresh_count_scales_with_epochs_not_operations() {
    constexpr std::size_t   kCapacity = 1024;
    constexpr std::uint64_t kRounds   = 100;
    constexpr std::uint64_t kPerRound = 1000;

    SpscSeparatedCachedInstrumented<std::uint64_t, kCapacity>    cached;
    SpscSeparatedBaselineInstrumented<std::uint64_t, kCapacity>  baseline;

    std::uint64_t v = 0;
    for (std::uint64_t round = 0; round < kRounds; ++round) {
        for (std::uint64_t i = 0; i < kPerRound; ++i) {
            CHECK(cached.try_push(i));
            CHECK(baseline.try_push(i));
        }
        for (std::uint64_t i = 0; i < kPerRound; ++i) {
            CHECK(cached.try_pop(v));
            CHECK(baseline.try_pop(v));
        }
    }

    const std::uint64_t total_messages = kRounds * kPerRound;

    // The two sides are bounded by DIFFERENT quantities, and saying so is the
    // point of the test:
    //
    //  * The PRODUCER runs ahead of the consumer's released tail, so its cached
    //    tail only goes stale once the head has advanced a whole Capacity past
    //    it — its refreshes are bounded by the number of Capacity-sized epochs.
    //  * The CONSUMER is the lagging side. Each refresh teaches it a head value,
    //    and it then pops until its own tail reaches that value, so its
    //    refreshes are bounded by the number of DRAIN PHASES (plus the one that
    //    begins each phase), not by the number of messages drained.
    const std::uint64_t producer_bound =
        total_messages / static_cast<std::uint64_t>(kCapacity) + 2;
    const std::uint64_t consumer_bound = kRounds + 2;

    // The baseline did exactly one remote load per call.
    CHECK(baseline.producer_remote_tail_loads() == total_messages);
    CHECK(baseline.consumer_remote_head_loads() == total_messages);

    CHECK(cached.producer_remote_tail_loads() <= producer_bound);
    CHECK(cached.consumer_remote_head_loads() <= consumer_bound);

    // ...and therefore far fewer in total. The factor is stated loosely on
    // purpose: the measured ratio here is ~1000x, so a 50x requirement cannot
    // become fragile under machine noise.
    CHECK(cached.producer_remote_tail_loads() * 50 <
          baseline.producer_remote_tail_loads());
    CHECK(cached.consumer_remote_head_loads() * 50 <
          baseline.consumer_remote_head_loads());

    std::printf("  burst %llu messages / capacity %zu: "
                "baseline %llu+%llu loads, cached %llu+%llu\n",
                static_cast<unsigned long long>(total_messages), kCapacity,
                static_cast<unsigned long long>(
                    baseline.producer_remote_tail_loads()),
                static_cast<unsigned long long>(
                    baseline.consumer_remote_head_loads()),
                static_cast<unsigned long long>(
                    cached.producer_remote_tail_loads()),
                static_cast<unsigned long long>(
                    cached.consumer_remote_head_loads()));
}

// Tight alternation (push-1/pop-1) is the OTHER extreme, and it is honest to
// test it because it is where the cache helps LEAST: the consumer is the
// lagging side and its cached head is exhausted almost immediately, so its
// refresh count stays close to one per pop. The producer, which runs ahead of
// the consumer's releases, still refreshes only per epoch.
//
// Asserting this keeps the mechanism claim bounded: the test says where the
// cache helps and where it does not, instead of asserting a uniform win.
void test_cached_tight_alternation_producer_still_wins_consumer_may_not() {
    constexpr std::size_t   kCapacity = 4096;
    constexpr std::uint64_t kRounds   = 100'000;

    SpscSeparatedCachedInstrumented<std::uint64_t, kCapacity>   cached;
    SpscSeparatedBaselineInstrumented<std::uint64_t, kCapacity> baseline;

    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < kRounds; ++i) {
        CHECK(cached.try_push(i));
        CHECK(baseline.try_push(i));
        CHECK(cached.try_pop(v));
        CHECK(baseline.try_pop(v));
        CHECK(v == i);
    }

    const std::uint64_t epoch_bound =
        kRounds / static_cast<std::uint64_t>(kCapacity) + 2;

    CHECK(baseline.producer_remote_tail_loads() == kRounds);
    CHECK(baseline.consumer_remote_head_loads() == kRounds);

    // Producer: the occupancy stays at ~1, so it never looks full and never
    // refreshes until an epoch has elapsed.
    CHECK(cached.producer_remote_tail_loads() <= epoch_bound);

    // Consumer: it is the lagging side here, so its cached head is exhausted by
    // almost every pop. It still never exceeds the baseline, and it is bounded
    // by the number of pops.
    CHECK(cached.consumer_remote_head_loads() <= kRounds);

    std::printf("  tight alternation %llu rounds: "
                "baseline %llu+%llu loads, cached %llu+%llu\n",
                static_cast<unsigned long long>(kRounds),
                static_cast<unsigned long long>(
                    baseline.producer_remote_tail_loads()),
                static_cast<unsigned long long>(
                    baseline.consumer_remote_head_loads()),
                static_cast<unsigned long long>(
                    cached.producer_remote_tail_loads()),
                static_cast<unsigned long long>(
                    cached.consumer_remote_head_loads()));
}

// The mechanism must also hold under REAL concurrency, because that is the
// regime the benchmark measures. This is the one case where the exact counts are
// not deterministic (thread interleaving decides how often "may be full" is
// hit), so only the properties that must hold are asserted:
//
//   * the baseline performs exactly one remote load per ATTEMPT, so its count
//     equals the number of calls including every backpressure retry;
//   * the cached variant performs strictly fewer remote loads than the baseline
//     on both sides;
//   * the cached variant is always at least as good as the baseline.
void test_cached_performs_fewer_remote_loads_under_real_concurrency() {
    constexpr std::size_t   kCapacity = 1024;
    constexpr std::uint64_t kMessages = 2'000'000;

    SpscSeparatedCachedInstrumented<std::uint64_t, kCapacity>   cached;
    SpscSeparatedBaselineInstrumented<std::uint64_t, kCapacity> baseline;

    std::atomic<std::uint64_t> cached_push_calls{0};
    std::atomic<std::uint64_t> baseline_push_calls{0};
    std::atomic<std::uint64_t> cached_pop_calls{0};
    std::atomic<std::uint64_t> baseline_pop_calls{0};
    std::atomic<std::uint64_t> cached_delivered{0};
    std::atomic<std::uint64_t> baseline_delivered{0};
    std::atomic<bool>          cached_order_ok{true};
    std::atomic<bool>          baseline_order_ok{true};

    std::atomic<bool> start{false};

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SpinWaiter wait;
        for (std::uint64_t i = 0; i < kMessages; ++i) {
            const std::uint64_t value = i;
            while (true) {
                cached_push_calls.fetch_add(1, std::memory_order_relaxed);
                if (cached.try_push(value)) {
                    break;
                }
                wait.pause();
            }
            while (true) {
                baseline_push_calls.fetch_add(1, std::memory_order_relaxed);
                if (baseline.try_push(value)) {
                    break;
                }
                wait.pause();
            }
        }
    });

    // The two queues are drained INDEPENDENTLY, each against its own expected
    // sequence. They are fed the same value sequence, but nothing makes their
    // consumption interleave one-for-one — the cached queue is the faster one and
    // pulls ahead — so a single shared counter would be measuring the test's own
    // bookkeeping rather than either queue's ordering.
    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        SpinWaiter    wait;
        std::uint64_t cached_next   = 0;
        std::uint64_t baseline_next = 0;
        std::uint64_t a             = 0;
        std::uint64_t b             = 0;
        while (cached_next < kMessages || baseline_next < kMessages) {
            bool progressed = false;
            if (cached_next < kMessages) {
                cached_pop_calls.fetch_add(1, std::memory_order_relaxed);
                if (cached.try_pop(a)) {
                    if (a != cached_next) {
                        cached_order_ok.store(false, std::memory_order_relaxed);
                    }
                    ++cached_next;
                    cached_delivered.store(cached_next,
                                           std::memory_order_relaxed);
                    progressed = true;
                }
            }
            if (baseline_next < kMessages) {
                baseline_pop_calls.fetch_add(1, std::memory_order_relaxed);
                if (baseline.try_pop(b)) {
                    if (b != baseline_next) {
                        baseline_order_ok.store(false,
                                                std::memory_order_relaxed);
                    }
                    ++baseline_next;
                    baseline_delivered.store(baseline_next,
                                             std::memory_order_relaxed);
                    progressed = true;
                }
            }
            if (!progressed) {
                wait.pause();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    // Both queues transferred every message, in order, with no loss or
    // duplication.
    CHECK(cached_delivered.load(std::memory_order_relaxed) == kMessages);
    CHECK(baseline_delivered.load(std::memory_order_relaxed) == kMessages);
    CHECK(cached_order_ok.load(std::memory_order_relaxed));
    CHECK(baseline_order_ok.load(std::memory_order_relaxed));

    // The baseline counts one remote load per call, so the test's own call
    // counters are the ground truth for it.
    CHECK(baseline.producer_remote_tail_loads() ==
          baseline_push_calls.load(std::memory_order_relaxed));
    CHECK(baseline.consumer_remote_head_loads() ==
          baseline_pop_calls.load(std::memory_order_relaxed));

    // WHAT IS ASSERTED HERE, AND WHY IT IS NOT A MARGIN.
    //
    // The baseline counts one remote load per ATTEMPT, so its counts are exactly
    // the call counts. The cached variant counts one load per attempt on the
    // may-be-full / may-be-empty path only. Therefore, BY CONSTRUCTION and for
    // every interleaving, the cached variant can never exceed the baseline on
    // either side — that is a property of the algorithms, not of this machine:
    //
    //     cached_loads <= calls_that_took_the_may_path <= calls == baseline
    //
    // How the reduction SPLITS between the two sides is a different matter: it
    // depends entirely on which thread is the lagging one, which is a scheduling
    // property. Under a heavily instrumented scheduler the split can move to
    // almost entirely the consumer side (a TSan build does exactly that), which
    // is why no per-side ratio is asserted here.
    CHECK(cached.producer_remote_tail_loads() <=
          baseline.producer_remote_tail_loads());
    CHECK(cached.consumer_remote_head_loads() <=
          baseline.consumer_remote_head_loads());

    // The total must be STRICTLY smaller: at least one may-path attempt avoided a
    // remote load. Two million messages through a ring that is repeatedly filled
    // and drained cannot avoid that.
    const std::uint64_t baseline_total =
        baseline.producer_remote_tail_loads() +
        baseline.consumer_remote_head_loads();
    const std::uint64_t cached_total = cached.producer_remote_tail_loads() +
                                       cached.consumer_remote_head_loads();
    CHECK(cached_total < baseline_total);

    std::printf("  concurrent %llu messages: baseline %llu+%llu loads "
                "(%llu+%llu calls), cached %llu+%llu (split depends on which "
                "side lags; not asserted)\n",
                static_cast<unsigned long long>(kMessages),
                static_cast<unsigned long long>(
                    baseline.producer_remote_tail_loads()),
                static_cast<unsigned long long>(
                    baseline.consumer_remote_head_loads()),
                static_cast<unsigned long long>(
                    baseline_push_calls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    baseline_pop_calls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                    cached.producer_remote_tail_loads()),
                static_cast<unsigned long long>(
                    cached.consumer_remote_head_loads()));
}

// An UNINSTRUMENTED build must not count, and must say so. "We did not count"
// and "we counted zero" must never look the same in the output.
void test_uninstrumented_build_reports_zero_and_says_so() {
    SpscSeparatedBaselineRingBuffer<std::uint64_t, 8> baseline;
    SpscSeparatedCachedCursorRingBuffer<std::uint64_t, 8> cached;

    CHECK(!baseline.instrumented());
    CHECK(!cached.instrumented());

    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        CHECK(baseline.try_push(static_cast<std::uint64_t>(i)));
        CHECK(cached.try_push(static_cast<std::uint64_t>(i)));
    }
    for (int i = 0; i < 8; ++i) {
        CHECK(baseline.try_pop(v));
        CHECK(cached.try_pop(v));
    }

    CHECK(baseline.producer_remote_tail_loads() == 0);
    CHECK(baseline.consumer_remote_head_loads() == 0);
    CHECK(cached.producer_remote_tail_loads() == 0);
    CHECK(cached.consumer_remote_head_loads() == 0);
}

// The modes are compile-time constants, so the dataset's `impl` name is decided
// by the type and cannot be mislabelled at run time.
void test_mode_names_are_distinct() {
    constexpr RemoteCursorMode baseline_mode =
        SpscSeparatedBaselineRingBuffer<int, 8>::mode();
    constexpr RemoteCursorMode cached_mode =
        SpscSeparatedCachedCursorRingBuffer<int, 8>::mode();
    CHECK(baseline_mode == RemoteCursorMode::Direct);
    CHECK(cached_mode == RemoteCursorMode::Cached);
    CHECK(std::string(lltl::remote_cursor_mode_name(RemoteCursorMode::Direct)) ==
          "baseline");
    CHECK(std::string(lltl::remote_cursor_mode_name(RemoteCursorMode::Cached)) ==
          "cached");
}

// ---------------------------------------------------------------------------
// COUNTER WRAP. The counters are monotonic unsigned values allowed to wrap
// modulo 2^width(std::size_t); the Phase-1/3A full and empty tests stay exact
// across the boundary because they are differences of unsigned counters, never
// signed comparisons and never modulo arithmetic on the slot index.
//
// Phase 3B adds comparisons against a THREAD-OWNED cached value, so the same
// argument has to be shown to survive there too.
// ---------------------------------------------------------------------------

// The modular inequality the cached check relies on, tested directly as
// arithmetic over values straddling the wrap boundary. This is the reasoning
// docs/SPSC_REMOTE_CURSOR_CACHE.md states; testing it here means a change to
// that reasoning fails a test rather than only a review.
//
// For a producer: cached_tail is a value the real tail had EARLIER, so the
// modular difference (real_tail - cached_tail) is small, and therefore
// (head - cached_tail) >= (head - real_tail). The two consequences the algorithm
// needs are:
//   * (head - cached_tail) < Capacity  =>  (head - real_tail) < Capacity
//   * (head - cached_tail) can never EXCEED Capacity
void test_modular_inequality_holds_across_the_wrap_boundary() {
    using U = std::size_t;
    constexpr U kMax = std::numeric_limits<U>::max();

    struct Case {
        U head;
        U real_tail;
        U cached_tail;
    };
    // Each case straddles or touches the 2^width boundary.
    const Case cases[] = {
        {5, 3, 2},                         // no wrap involved
        {0, kMax - 1, kMax - 1},           // head wrapped to 0
        {4, kMax - 3, kMax - 3},           // head just past the boundary
        {kMax, kMax - 5, kMax - 5},        // head just below the boundary
        {kMax - 1, kMax - 4, kMax - 4},    // head below, tail below
        {2, 1, 1},
        {2, kMax - 2, kMax - 4},           // wrapped head AND a stale cached tail
    };

    for (const Case& c : cases) {
        // cached_tail is a PAST value of real_tail: it lags by a small amount.
        const U real_lag = c.real_tail - c.cached_tail;
        CHECK(real_lag < 1024);

        const U true_occupancy = c.head - c.real_tail;
        const U cached_occupancy = c.head - c.cached_tail;

        CHECK(true_occupancy <= 8); // the queue can never hold more than Capacity
        // Staleness is conservative: the cached view never looks EMPTIER than
        // reality, it looks FULLER.
        CHECK(cached_occupancy >= true_occupancy);
        CHECK(cached_occupancy == true_occupancy + real_lag);

        // The two consequences the algorithm depends on.
        if (cached_occupancy < 8) {
            CHECK(true_occupancy < 8); // "may be full" was false => safe to push
        }
        CHECK(cached_occupancy <= 8);
    }
}

// The same lemma on the consumer side. Here the invariant is stated as a
// modular DIFFERENCE rather than as an ordering, because an ordering is exactly
// what unsigned counters do not give across the boundary: `t <= cached_head` is
// false for a perfectly correct state in which head has wrapped and the cached
// head has not. The algorithm never relies on that ordering — it relies on
// `cached_head - t`, a small modular difference, being non-zero.
void test_consumer_modular_lemma_across_the_wrap_boundary() {
    using U = std::size_t;
    constexpr U kMax     = std::numeric_limits<U>::max();
    constexpr U kCapacity = 8;

    // (real head, how far the cached head lags behind it) — pairs chosen to
    // straddle the boundary in both directions.
    struct Case {
        U head;
        U cached_lag;
    };
    const Case cases[] = {
        {0, 0},        // empty at the wrapped-to-zero boundary
        {0, 1},        // one message published across the boundary
        {3, 2},        // no wrap involved
        {kMax, 0},     // head just below the boundary
        {kMax, 3},     // cached head further below the boundary
        {2, 2},        // head wrapped to 2, cached head still below it
        {7, 7},        // cached head below the boundary, head above it
    };

    for (const Case& c : cases) {
        // cached_head is a PAST head, so it is reached by subtracting a small
        // modular difference. No signed arithmetic and no ordering is used.
        const U cached_head = c.head - c.cached_lag;
        CHECK(c.head - cached_head == c.cached_lag);

        // The consumer's own tail lags the cached head by the occupancy it
        // believes it has, which is never more than Capacity.
        for (U believed = 0; believed <= kCapacity; ++believed) {
            const U t = cached_head - believed;

            // The modular difference is EXACT, which is what makes the guard
            // safe at the boundary.
            CHECK(cached_head - t == believed);

            if (believed == 0) {
                // cached_head == t is "MAY be empty": the algorithm refreshes
                // rather than assuming empty, so no unpublished slot is read.
                CHECK(cached_head == t);
            } else {
                // Otherwise the consumer proceeds. It is entitled to read slot
                // `t & mask` because head is at least one ahead of t — and that
                // bound is itself a modular difference, not a comparison.
                CHECK(cached_head != t);
                CHECK(c.head - t == c.cached_lag + believed);
                CHECK(c.head - t >= 1);
                CHECK(c.head - t <= kCapacity + c.cached_lag);
            }
        }
    }
}

// Cross the boundary FOR REAL: seed an empty queue one short of it, then push
// and pop across it through the ordinary API, in both modes. The logical
// counters pass 2^width here without 2^width operations.
template <RemoteCursorMode Mode>
void t_operates_across_the_counter_wrap_boundary() {
    using U = std::size_t;
    constexpr U kMax = std::numeric_limits<U>::max();

    auto q = make_seeded_queue<Mode, std::uint64_t, 8>(kMax - 3);
    CHECK(q.empty());

    std::uint64_t v = 0;
    for (int cycle = 0; cycle < 20; ++cycle) {
        for (std::uint64_t i = 0; i < 8; ++i) {
            CHECK(q.try_push(i));
        }
        CHECK(!q.try_push(999ull)); // full, across the boundary
        for (std::uint64_t i = 0; i < 8; ++i) {
            CHECK(q.try_pop(v));
            CHECK(v == i);
        }
        CHECK(!q.try_pop(v)); // empty, across the boundary
    }
    CHECK(q.empty());
}

// The same, at Capacity = 1: the narrowest possible ring, so every single
// operation wraps the physical slot and the counters cross the boundary with the
// smallest possible stride.
template <RemoteCursorMode Mode>
void t_wraps_at_capacity_one_across_the_boundary() {
    using U = std::size_t;
    constexpr U kMax = std::numeric_limits<U>::max();

    auto q = make_seeded_queue<Mode, std::uint64_t, 1>(kMax);
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 64; ++i) {
        CHECK(q.try_push(i));
        CHECK(!q.try_push(i + 1)); // full at Capacity = 1
        CHECK(q.try_pop(v));
        CHECK(v == i);
        CHECK(!q.try_pop(v)); // empty
    }
}

// ---------------------------------------------------------------------------
// Prior phases must be untouched and still usable. Phase 3B is a new type; it
// modifies nothing that came before it.
// ---------------------------------------------------------------------------
void test_prior_phase_types_are_untouched() {
    SpscRingBuffer<std::uint64_t, 1024> frozen;
    CHECK(frozen.empty());
    CHECK(frozen.try_push(1ull));
    std::uint64_t v = 0;
    CHECK(frozen.try_pop(v));
    CHECK(v == 1ull);

    SpscSeparatedCursorRingBuffer<std::uint64_t, 1024> sep;
    CHECK(sep.empty());
    CHECK(sep.try_push(2ull));
    CHECK(sep.try_pop(v));
    CHECK(v == 2ull);

    auto same = std::make_unique<lltl::SpscSameLineRingBuffer<std::uint64_t, 1024>>();
    const auto r = same->cursor_layout_report(lltl::reported_cache_line_size());
    CHECK(r.cursors_same_line); // the Phase-3A control is unchanged
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

    // --- Layout evidence (runs first: if the layout is not what it claims,
    //     nothing below is evidence about remote-load frequency) -------------
    test_cursor_policy_layout_assertions();
    total += summary("layout: cursor policy offsets and footprint");

    test_both_modes_have_identical_footprint();
    total += summary("layout: both modes have identical footprint");

    test_footprint_matches_phase3a_for_every_benchmark_shape();
    total += summary("layout: footprint matches Phase-3A separated policy");

    test_phase3b_report_builder_matches_phase3a();
    total += summary("layout: report builder agrees with Phase-3A");

    test_cached_state_placement_is_verified();
    total += summary("layout: cached state on its owner's line");

    test_cached_placement_check_can_fail();
    total += summary("layout: cached-placement check is falsifiable");

    test_layout_guard_rejects_unsupported_line_size();
    total += summary("layout: guard rejects unsupported line size");

    RUN_BOTH_MODES("layout: separated invariant at runtime",
                   t_both_modes_are_separated_at_runtime);
    RUN_BOTH_MODES("layout: invariants hold for smaller real lines",
                   t_invariants_hold_under_smaller_line_sizes);

    // --- Differential ------------------------------------------------------
    test_all_variants_agree_under_identical_operation_sequence();
    total += summary("differential: baseline/cached/phase3a/model agree");

    // --- Refresh mechanism -------------------------------------------------
    test_mode_names_are_distinct();
    total += summary("mechanism: mode names are compile-time");

    test_uninstrumented_build_reports_zero_and_says_so();
    total += summary("mechanism: uninstrumented build does not count");

    test_baseline_performs_one_remote_load_per_call();
    total += summary("mechanism: baseline loads once per call");

    test_cached_producer_needs_no_remote_load_to_fill_an_empty_queue();
    total += summary("mechanism: cached fill needs zero producer loads");

    test_cached_consumer_learns_head_once_per_epoch();
    total += summary("mechanism: cached drain learns head once per epoch");

    test_cached_refresh_count_scales_with_epochs_not_operations();
    total += summary("mechanism: refreshes scale with epochs, not ops");

    test_cached_tight_alternation_producer_still_wins_consumer_may_not();
    total += summary("mechanism: tight alternation is the honest weak case");

    test_cached_performs_fewer_remote_loads_under_real_concurrency();
    total += summary("mechanism: fewer remote loads under concurrency");

    // --- Counter wrap ------------------------------------------------------
    test_modular_inequality_holds_across_the_wrap_boundary();
    total += summary("wrap: producer modular inequality across 2^width");

    test_consumer_modular_lemma_across_the_wrap_boundary();
    total += summary("wrap: consumer modular lemma across 2^width");

    RUN_BOTH_MODES("wrap: push/pop across the counter boundary",
                   t_operates_across_the_counter_wrap_boundary);
    RUN_BOTH_MODES("wrap: Capacity = 1 across the boundary",
                   t_wraps_at_capacity_one_across_the_boundary);

    // --- Phase-1 correctness protocol, both modes --------------------------
    RUN_BOTH_MODES("new queue is empty", t_new_queue_empty);
    RUN_BOTH_MODES("pop empty returns false", t_pop_empty_returns_false);
    RUN_BOTH_MODES("push one / pop one", t_push_pop_one);
    RUN_BOTH_MODES("FIFO ordering", t_fifo_order);
    RUN_BOTH_MODES("fill exactly Capacity, push-when-full rejected",
                   t_fill_exactly_capacity_then_full_reject);
    RUN_BOTH_MODES("repeated push when full keeps failing",
                   t_repeated_push_when_full_never_succeeds);
    RUN_BOTH_MODES("pop after full drains in order",
                   t_pop_after_full_drains_in_order);
    RUN_BOTH_MODES("repeated pop when empty keeps failing",
                   t_repeated_pop_when_empty_never_succeeds);
    RUN_BOTH_MODES("physical wrap-around (many fill/drain cycles)",
                   t_wrap_around_repeatedly);
    RUN_BOTH_MODES("physical wrap-around at partial occupancy",
                   t_wrap_around_at_partial_occupancy);
    RUN_BOTH_MODES("interleaved fill/drain vs deque model",
                   t_interleaved_fill_drain_cycles);
    RUN_BOTH_MODES("payload: sequence values preserved",
                   t_payload_sequence_values_preserved);
    RUN_BOTH_MODES("payload: structured message copied", t_structured_message_copy);
    RUN_BOTH_MODES("payload: structured message moved", t_structured_message_move);
    RUN_BOTH_MODES("payload: copy vs move overloads exercised",
                   t_copy_vs_move_overloads);
    RUN_BOTH_MODES("concurrent: large deterministic sequence",
                   t_concurrent_producer_consumer);
    RUN_BOTH_MODES("concurrent: rapid slot reuse (Capacity = 2)",
                   t_concurrent_rapid_slot_reuse);
    RUN_BOTH_MODES("concurrent: multi-field payload publication",
                   t_concurrent_multi_field_payload);
    RUN_BOTH_MODES("reference differential (mutex vs variant vs model)",
                   t_reference_differential);

    // --- Prior phases untouched -------------------------------------------
    test_prior_phase_types_are_untouched();
    total += summary("frozen Phase-1 and Phase-3A types still usable");

    std::printf("%s\n", total == 0 ? "spsc remote cursor: all ok"
                                   : "spsc remote cursor: FAILURES");
    return total == 0 ? 0 : 1;
}
