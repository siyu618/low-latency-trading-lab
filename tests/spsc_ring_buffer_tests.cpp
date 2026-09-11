// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 1 correctness tests.
//
// Coverage:
//   * single-thread semantic tests of SpscRingBuffer — empty, push/pop, FIFO,
//     fill-to-capacity, full rejection, drain, physical wrap-around, and many
//     fill/drain cycles;
//   * payload tests — exact sequence preservation, and structured messages that
//     verify the copy overload truly copies and the move overload truly moves;
//   * a concurrent test — one producer, one consumer, a large deterministic
//     sequence, the consumer verifying every value arrives exactly once, in
//     exact order (any duplicate, missing, or reordered message fails);
//   * a reference differential test — identical deterministic logical
//     operations fed to MutexBoundedQueue and SpscRingBuffer, both required to
//     agree with each other and with a std::deque model at every step.
//
// The suite doubles as an exit-code self-test (LLDB_SELFTEST_FAIL): the
// concurrency is exercised only in the normal run.
//
// A plain CHECK macro reports the file/line of the first failing assertion;
// failures accumulate into a total and main() returns non-zero on any failure,
// so CTest genuinely fails on a bad run. No external test framework is used.

#include "mutex_bounded_queue.h"
#include "spsc_ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <utility>

using lltl::MutexBoundedQueue;
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
        std::printf("[ok] %-42s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-42s (%d/%d checks FAILED)\n", suite, n_fail, g_checks);
    }
    g_failures = 0;
    g_checks   = 0;
    return n_fail;
}

// ---------------------------------------------------------------------------
// Single-thread semantic tests. Capacity is deliberately small (8) so that
// full/empty and physical wrap-around are cheap to reach.
// ---------------------------------------------------------------------------
void test_new_queue_empty() {
    SpscRingBuffer<int, 8> q;
    CHECK(q.empty());
    CHECK(q.capacity() == 8);
}

void test_pop_empty_returns_false() {
    SpscRingBuffer<int, 8> q;
    int v = 123;
    CHECK(!q.try_pop(v));
    CHECK(v == 123); // out must be untouched on failure
    CHECK(q.empty());
}

void test_push_pop_one() {
    SpscRingBuffer<int, 8> q;
    CHECK(q.try_push(42));
    CHECK(!q.empty());
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK(v == 42);
    CHECK(q.empty());
}

void test_fifo_order() {
    SpscRingBuffer<int, 8> q;
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

void test_fill_exactly_capacity_then_full_reject() {
    SpscRingBuffer<int, 8> q;
    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_push(i));
    }
    CHECK(!q.empty());
    CHECK(q.try_push(100) == false); // full: head - tail == Capacity
    int v = -1;
    CHECK(q.try_pop(v));
    CHECK(v == 0);
}

void test_pop_after_full_drains_in_order() {
    SpscRingBuffer<int, 8> q;
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

// Wrap the PHYSICAL ring many times: each of the Capacity slots is overwritten
// and re-read repeatedly while the monotonic counters grow far past Capacity.
// The logical sequence must stay contiguous across every fill/drain boundary.
void test_wrap_around_repeatedly() {
    SpscRingBuffer<std::uint64_t, 8> q;
    constexpr int kCycles = 100;
    std::uint64_t next = 0; // next logical value expected at pop
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

// Fill/drain cycles interleaved at varying occupancy: push a few, pop a few,
// push many, etc. The model deque is the source of truth for FIFO order.
void test_interleaved_fill_drain_cycles() {
    SpscRingBuffer<int, 8> q;
    std::deque<int> model;
    std::uint64_t rng = 0x9E3779B97F4A7C15ull; // fixed seed (splitmix64-style)
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
            const std::size_t n = want + 1 > model.size() ? model.size() : want + 1;
            for (std::size_t i = 0; i < n; ++i) {
                int v = -1;
                CHECK(q.try_pop(v));
                CHECK(v == model.front());
                model.pop_front();
            }
        }
    }
    // Drain whatever is left and require the model and the queue agree exactly.
    while (!model.empty()) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK(v == model.front());
        model.pop_front();
    }
    CHECK(q.empty());
}

// ---------------------------------------------------------------------------
// Payload tests.
// ---------------------------------------------------------------------------
void test_payload_sequence_values_preserved() {
    SpscRingBuffer<std::uint64_t, 16> q;
    constexpr std::uint64_t kBase = 1'000'000'000ull; // exercise wide values
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(q.try_push(kBase + static_cast<std::uint64_t>(i)));
    }
    for (std::size_t i = 0; i < 16; ++i) {
        std::uint64_t v = 0;
        CHECK(q.try_pop(v));
        CHECK(v == kBase + static_cast<std::uint64_t>(i));
    }
}

// A structured message: default-constructible, copy/move assignable. Mirrors a
// decoded market-data message (sequence + text payload).
struct Message {
    std::uint64_t seq = 0;
    std::string   text;

    Message() = default;
    Message(std::uint64_t s, std::string t) : seq(s), text(std::move(t)) {}
};

void test_structured_message_copy() {
    SpscRingBuffer<Message, 4> q;
    Message src(7, "bid update");
    CHECK(q.try_push(src)); // const& overload -> must COPY
    src.text = "mutated after push"; // must not affect the enqueued copy
    Message out;
    CHECK(q.try_pop(out));
    CHECK(out.seq == 7);
    CHECK(out.text == "bid update");
}

void test_structured_message_move() {
    SpscRingBuffer<Message, 4> q;
    Message tmp(9, "ask update");
    CHECK(q.try_push(std::move(tmp))); // && overload -> must MOVE
    Message out;
    CHECK(q.try_pop(out));
    CHECK(out.seq == 9);
    CHECK(out.text == "ask update");
}

// Proves WHICH element operation each API call performs, so the overloads are
// exercised rather than assumed. Single-threaded => plain counters are fine.
struct CopyMoveProbe {
    std::uint64_t v = 0;
    static int copies;
    static int moves;

    CopyMoveProbe() = default;
    explicit CopyMoveProbe(std::uint64_t x) : v(x) {}
    CopyMoveProbe(const CopyMoveProbe& o) : v(o.v) { ++copies; }
    CopyMoveProbe(CopyMoveProbe&& o) noexcept : v(o.v) { ++moves; }
    CopyMoveProbe& operator=(const CopyMoveProbe& o) { v = o.v; ++copies; return *this; }
    CopyMoveProbe& operator=(CopyMoveProbe&& o) noexcept { v = o.v; ++moves; return *this; }
};
int CopyMoveProbe::copies = 0;
int CopyMoveProbe::moves   = 0;

void test_copy_vs_move_overloads() {
    CopyMoveProbe::copies = 0;
    CopyMoveProbe::moves   = 0;
    {
        SpscRingBuffer<CopyMoveProbe, 4> q;
        CopyMoveProbe a(1);
        CHECK(q.try_push(a));                  // const& -> copy into the slot
        CHECK(q.try_push(CopyMoveProbe(2)));   // &&     -> move into the slot
        CopyMoveProbe out1;
        CopyMoveProbe out2;
        CHECK(q.try_pop(out1));                // move out of the slot
        CHECK(q.try_pop(out2));
        CHECK(out1.v == 1);
        CHECK(out2.v == 2);
    }
    CHECK(CopyMoveProbe::moves >= 2);  // 2nd push + both pops must have moved
    CHECK(CopyMoveProbe::copies == 1); // exactly the const& push copied
}

// ---------------------------------------------------------------------------
// Concurrent test: one producer, one consumer. The producer pushes the
// deterministic sequence [0, N); the consumer requires every pop to equal the
// next expected value. Any duplicate, missing, or out-of-order message fails
// the run. Both threads spin on a start gate so they overlap.
// ---------------------------------------------------------------------------
void test_concurrent_producer_consumer() {
    constexpr std::size_t kCapacity = 1024;
    constexpr std::uint64_t kMessages = 1ull << 20; // 1,048,576 messages

    SpscRingBuffer<std::uint64_t, kCapacity> q;
    std::atomic<bool> start{false};
    std::atomic<bool> consumer_ok{true};
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
                std::this_thread::yield(); // buffer full; wait for the consumer
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
                    // keep draining so the producer can finish, but record failure
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

// ---------------------------------------------------------------------------
// Reference differential test: identical deterministic logical operations are
// fed to MutexBoundedQueue (the correctness reference) and SpscRingBuffer; at
// every step both must agree with each other AND with a std::deque model. This
// pins the SPSC buffer's FIFO semantics to the mutex queue's.
// ---------------------------------------------------------------------------
void test_reference_differential() {
    constexpr std::size_t kCapacity = 64;
    MutexBoundedQueue<std::uint64_t, kCapacity> mq;
    SpscRingBuffer<std::uint64_t, kCapacity>    sq;
    std::deque<std::uint64_t> model;

    std::uint64_t rng  = 0xD1B54A32D192ED03ull; // fixed seed
    std::uint64_t next = 0;                      // next value to push

    constexpr int kOps = 100'000;
    for (int op = 0; op < kOps; ++op) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull; // LCG
        const std::uint64_t roll = rng >> 33; // [0, 2^31)

        // Empty => cannot pop, must push. Full => cannot push, must pop.
        // Otherwise a random push-biased choice (2/3 push) keeps the buffer
        // oscillating between empty and full across the run.
        const bool must_push = model.empty();
        const bool must_pop  = !must_push && model.size() == kCapacity;
        const bool do_push   = must_push || (!must_pop && (roll % 3u) < 2u);

        if (do_push) {
            const bool m_ok = mq.try_push(next);
            const bool s_ok = sq.try_push(next);
            CHECK(m_ok); // model not full => both must accept
            CHECK(m_ok == s_ok);
            model.push_back(next);
            ++next;
        } else {
            std::uint64_t m_val = 0;
            std::uint64_t s_val = 0;
            const bool m_ok = mq.try_pop(m_val);
            const bool s_ok = sq.try_pop(s_val);
            CHECK(m_ok); // model non-empty => both must pop
            CHECK(m_ok == s_ok);
            CHECK(m_val == s_val);
            CHECK(m_val == model.front());
            model.pop_front();
        }
        // The two queues must agree on emptiness at every step.
        CHECK(mq.empty() == sq.empty());
        CHECK(mq.empty() == model.empty());
    }

    // Drain the remainder; both queues and the model must stay in lock-step.
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
// Exit-code self-test. A failed CHECK must produce a non-zero exit status.
// Invoked via the LLDB_SELFTEST_FAIL environment variable (see main).
// ---------------------------------------------------------------------------
void self_test_exit_code() {
    std::printf("  (self-test) intentionally failing one CHECK...\n");
    CHECK(1 == 2); // must trip
}

} // namespace

int main() {
    // Exit-code self-test mode: run ONLY the deliberately-failing suite so the
    // caller can verify the process returns non-zero when a CHECK fails.
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("exit-code self-test: expecting non-zero exit\n");
        self_test_exit_code();
        const int n = summary("self-test (expected FAIL)");
        return n == 0 ? 0 : 1;
    }

    int total = 0;

    test_new_queue_empty();
    total += summary("new queue is empty");

    test_pop_empty_returns_false();
    total += summary("pop empty returns false");

    test_push_pop_one();
    total += summary("push one / pop one");

    test_fifo_order();
    total += summary("FIFO ordering");

    test_fill_exactly_capacity_then_full_reject();
    total += summary("fill exactly Capacity, push-when-full rejected");

    test_pop_after_full_drains_in_order();
    total += summary("pop after full drains in order");

    test_wrap_around_repeatedly();
    total += summary("physical wrap-around (many fill/drain cycles)");

    test_interleaved_fill_drain_cycles();
    total += summary("interleaved fill/drain vs deque model");

    test_payload_sequence_values_preserved();
    total += summary("payload: sequence values preserved");

    test_structured_message_copy();
    total += summary("payload: structured message copied");

    test_structured_message_move();
    total += summary("payload: structured message moved");

    test_copy_vs_move_overloads();
    total += summary("payload: copy vs move overloads exercised");

    test_concurrent_producer_consumer();
    total += summary("concurrent: SPSC large deterministic sequence");

    test_reference_differential();
    total += summary("reference differential (mutex vs spsc vs model)");

    std::printf("%s\n", total == 0 ? "spsc ring buffer: all ok"
                                   : "spsc ring buffer: FAILURES");
    return total == 0 ? 0 : 1;
}
