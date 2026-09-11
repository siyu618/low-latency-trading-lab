// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 2: the deterministic
// two-thread END-TO-END THROUGHPUT BASELINE.
//
// RESEARCH QUESTION
//   How much steady-state end-to-end message-transfer throughput does the frozen
//   Phase-1 SPSC protocol provide, relative to a mutex-serialized bounded queue,
//   under controlled message sizes and queue capacities?
//
//   The answer is NOT claimed in advance and is NOT encoded anywhere in this
//   file. The tool measures and reports; docs/SPSC_THROUGHPUT.md interprets.
//
// WHAT IS MEASURED (and what is not)
//   Exactly the system model of Phase 1:  one producer thread -> queue -> one
//   consumer thread. `ns_per_message` is the END-TO-END elapsed wall time for
//   the complete transfer of N messages divided by the N messages actually
//   delivered. It therefore includes queue synchronization, payload assignment,
//   cache-coherence traffic on the two cursors, the benchmark's retry /
//   backpressure behaviour, and OS scheduling of the two threads.
//
//   It is NOT a per-try_push latency, NOT a per-try_pop latency, and NOT a
//   one-way handoff time. Those would each require a different harness (and
//   would each be a different, much noisier, measurement). This tool must never
//   be cited as if it reported them.
//
// ONE IMPLEMENTATION PER PROCESS
//   --impl=mutex and --impl=spsc are separate invocations. The two queues are
//   NEVER timed inside the same interval or the same process, because a single
//   process timing both would share one address space, one thread pool's worth
//   of scheduler state, one set of warmed caches, and one frequency history.
//   scripts/spsc-throughput.sh runs the canonical matrix that way.
//
// THE FROZEN BASELINE
//   The SPSC side is the Phase-1 implementation EXACTLY as frozen: unpadded,
//   adjacent head_/tail_ cursors, no cached remote cursor, no batching, no
//   modified memory orders. Those are Phase 3's controlled variables and this
//   benchmark must not touch them, or Phase 3 loses its baseline.
//
// TIMED INTERVAL DISCIPLINE
//   Thread creation, queue allocation, argument parsing, the expected-checksum
//   pre-pass and all printing happen OUTSIDE the timed interval. The producer
//   and consumer both signal READY, main waits for both, and only then records
//   t0 and publishes the start flag. The consumer records t1 immediately after
//   its final successful pop. No random number generation is used anywhere in
//   this program: message payloads are a closed-form function of the sequence
//   number (see "message construction" below).
//
// RETRY / BACKPRESSURE POLICY
//   The queues themselves stay non-blocking, non-spinning APIs (Phase 1 is
//   frozen). The HARNESS retries: a failed try_push is a producer_full_retry, a
//   failed try_pop is a consumer_empty_retry. Both sides use the SAME policy —
//   busy retry, with an occasional std::this_thread::yield() after a run of
//   misses (never sleep). These counters are reported as observable metrics:
//   they quantify backpressure. They are NOT correctness failures.
//
// MESSAGE CONSTRUCTION
//   Fixed-size, trivially copyable, nothrow, allocation-free value types of 8,
//   32 and 64 bytes, each carrying at least a sequence number. The extra words
//   are a deterministic function of `seq` (a splitmix64 finalizer plus cheap
//   multiply-adds) — closed-form arithmetic, not an RNG. Construction is
//   byte-for-byte identical for both implementations, and it sits in the
//   producer's measured path because a real feed handler also assembles its
//   message before enqueueing it. There are no strings, no dynamic allocation
//   and no pointer-owned payloads in the measured path.
//
// VALIDATION (every run, timed or not)
//   The consumer checks that the sequence number of every delivered message is
//   exactly the expected next one — which is simultaneously the "no gaps, no
//   duplicates, no reordering" check — and folds every received word into a
//   checksum. Before timing starts, the same fold is computed over the exact
//   stream the producer will send. The run PASSES only if all N messages were
//   delivered, every sequence number matched, and the two checksums agree. On
//   failure the tool prints the failure and exits NON-ZERO so a runner can
//   refuse to publish the cell.
//
// USAGE
//   spsc_throughput_bench --impl=mutex|spsc --message-bytes=8|32|64
//                         --capacity=1024|4096|65536
//                         [--messages=N] [--reps=R] [--warmup=W]
//                         [--raw-out=FILE] [--summary-out=FILE]
//
//   --impl is REQUIRED and must name exactly one implementation; "both"/"all"
//   are rejected. Capacities and message sizes are dispatched through small
//   explicit switches onto compile-time template parameters — there is no
//   runtime-capacity queue and the Phase-1 API is unchanged.
//
// See docs/SPSC_THROUGHPUT.md for the methodology, the canonical matrix, and
// the analysis of the measured results.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "mutex_bounded_queue.h"
#include "spsc_ring_buffer.h"

namespace {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

// Default message count: large enough that the timed interval is dominated by
// steady-state transfer rather than by startup transients, small enough that the
// full 18-cell canonical matrix stays practical. The exact value used is printed
// in — and required by — every result.
constexpr std::uint64_t kDefaultMessages = 10'000'000;
constexpr int           kDefaultReps     = 5;
constexpr int           kDefaultWarmup   = 1;

// Busy-retry discipline, same shape as the Phase-1 test waiter: retry in a tight
// loop, and only after this many consecutive misses hand the core over with
// std::this_thread::yield(). Never sleep — sleeping would measure the scheduler.
constexpr std::uint64_t kYieldAfterMisses = 1024;

enum class Impl { Mutex, Spsc };

// ---------------------------------------------------------------------------
// Message types — fixed size, trivially copyable, nothrow, allocation-free.
//
// These deliberately satisfy the Phase-1 queue requirements (default
// constructible + nothrow copy/move assignable) so that neither queue is
// measuring anything other than the transfer.
// ---------------------------------------------------------------------------

// splitmix64 finalizer: a cheap, well-mixed, deterministic bijection on 64 bits.
// Used only to give the payload words realistic-looking, position-dependent
// values. It is NOT a random number generator and carries no hidden state.
constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr std::uint64_t rotl7(std::uint64_t x) noexcept {
    return (x << 7) | (x >> 57);
}

// Fold a message's words into a running order-sensitive checksum. Rotate-then-xor
// is two ALU ops per word, so the checksum costs the consumer almost nothing
// while still being sensitive to both the values and their positions.
template <typename... Words>
constexpr std::uint64_t fold_words(std::uint64_t h, Words... words) noexcept {
    ((h = rotl7(h) ^ static_cast<std::uint64_t>(words)), ...);
    return h;
}

// Deterministic payload fields, all closed-form in `seq`.
constexpr std::uint64_t derive_price(std::uint64_t seq) noexcept {
    return 900'000ull + (seq % 100'000ull);
}
constexpr std::uint64_t derive_qty(std::uint64_t seq) noexcept {
    return 1ull + (seq % 1'000ull);
}

struct Msg8 {
    std::uint64_t seq = 0;

    static constexpr std::size_t kBytes = 8;

    static Msg8 make(std::uint64_t s) noexcept { return Msg8{s}; }

    std::uint64_t fold(std::uint64_t h) const noexcept { return fold_words(h, seq); }
};

struct Msg32 {
    std::uint64_t seq   = 0; // sequence number: present in every message type
    std::uint64_t price = 0;
    std::uint64_t qty   = 0;
    std::uint64_t tag   = 0;

    static constexpr std::size_t kBytes = 32;

    static Msg32 make(std::uint64_t s) noexcept {
        return Msg32{s, derive_price(s), derive_qty(s), mix64(s)};
    }

    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, tag);
    }
};

struct Msg64 {
    std::uint64_t seq   = 0;
    std::uint64_t price = 0;
    std::uint64_t qty   = 0;
    std::uint64_t tag   = 0;
    std::uint64_t w0    = 0;
    std::uint64_t w1    = 0;
    std::uint64_t w2    = 0;
    std::uint64_t w3    = 0;

    static constexpr std::size_t kBytes = 64;

    static Msg64 make(std::uint64_t s) noexcept {
        const std::uint64_t t = mix64(s);
        return Msg64{s,
                     derive_price(s),
                     derive_qty(s),
                     t,
                     t * 3ull + 1ull, // cheap, independent-looking derived words
                     t * 5ull + 2ull,
                     t * 7ull + 3ull,
                     t * 11ull + 4ull};
    }

    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, tag, w0, w1, w2, w3);
    }
};

// The Phase-2 message contract, asserted rather than assumed: exact size,
// trivially copyable, and nothrow-assignable so that a transfer never runs user
// cleanup, allocator code, or an exception path.
#define LLLT_EXP2_ASSERT_MSG(Type, Bytes)                                      \
    static_assert(Type::kBytes == (Bytes),                                     \
                  #Type " must declare kBytes == " #Bytes);                    \
    static_assert(sizeof(Type) == Type::kBytes,                                \
                  #Type " must occupy exactly kBytes bytes (no padding)");     \
    static_assert(std::is_trivially_copyable_v<Type>,                          \
                  #Type " must be trivially copyable");                        \
    static_assert(std::is_default_constructible_v<Type>,                       \
                  #Type " must be default-constructible (Phase-1 queues)");    \
    static_assert(std::is_nothrow_copy_assignable_v<Type>,                     \
                  #Type " must be nothrow copy-assignable");                   \
    static_assert(std::is_nothrow_move_assignable_v<Type>,                     \
                  #Type " must be nothrow move-assignable")

LLLT_EXP2_ASSERT_MSG(Msg8, 8);
LLLT_EXP2_ASSERT_MSG(Msg32, 32);
LLLT_EXP2_ASSERT_MSG(Msg64, 64);

#undef LLLT_EXP2_ASSERT_MSG

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    Impl          impl{};
    bool          impl_set      = false;
    std::size_t   message_bytes = 0;
    std::size_t   capacity      = 0;
    std::uint64_t messages      = kDefaultMessages;
    int           reps          = kDefaultReps;
    int           warmup        = kDefaultWarmup;
    std::string   raw_out;
    std::string   summary_out;
    // The exact effective invocation, echoed into every result so a measurement
    // is always traceable to the command that produced it.
    int                argc_saved = 0;
    const char* const* argv_saved = nullptr;
};

constexpr std::size_t kCapacities[] = {1024, 4096, 65536};

bool is_supported_capacity(std::size_t c) noexcept {
    for (std::size_t ok : kCapacities) {
        if (ok == c) {
            return true;
        }
    }
    return false;
}

void usage(const char* argv0) {
    std::printf(
        "usage: %s --impl=mutex|spsc --message-bytes=8|32|64 "
        "--capacity=1024|4096|65536\n"
        "          [--messages=N] [--reps=R] [--warmup=W]\n"
        "          [--raw-out=FILE] [--summary-out=FILE]\n"
        "\n"
        "Experiment 02 Phase 2 — two-thread end-to-end message-transfer "
        "throughput.\n"
        "ONE implementation per process: run --impl=mutex and --impl=spsc as\n"
        "separate invocations (scripts/spsc-throughput.sh does this).\n"
        "\n"
        "  --impl           REQUIRED, exactly one of mutex|spsc\n"
        "  --message-bytes  fixed-size nothrow message type (8, 32 or 64 bytes)\n"
        "  --capacity       queue capacity, dispatched to a compile-time "
        "specialization\n"
        "  --messages       messages to transfer per repetition (default %" PRIu64 ")\n"
        "  --reps           MEASURED repetitions per cell (default %d, all kept)\n"
        "  --warmup         untimed, unreported warm-up repetitions (default %d)\n"
        "  --raw-out        write raw per-repetition CSV here (source of truth)\n"
        "  --summary-out    write the derived summary here (default stdout)\n"
        "\n"
        "ns_per_message = end-to-end elapsed_ns / messages DELIVERED. It is not a\n"
        "per-try_push or per-try_pop latency and not a one-way handoff time.\n",
        argv0, kDefaultMessages, kDefaultReps, kDefaultWarmup);
}

// ---- argument helpers -----------------------------------------------------

bool parse_u64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char*          end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint64_t>(v);
    return true;
}

bool parse_int(const char* s, int& out) {
    std::uint64_t v = 0;
    if (!parse_u64(s, v) || v > 1'000'000ull) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// Accepts both "--name=value" (preferred) and "--name value".
bool arg_value(int argc, char** argv, int& i, const char* name, const char*& out) {
    const std::size_t n = std::strlen(name);
    if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=') {
        out = argv[i] + n + 1;
        return true;
    }
    if (std::strcmp(argv[i], name) == 0) {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s requires a value\n", name);
            return false;
        }
        out = argv[++i];
        return true;
    }
    return false;
}

int parse_args(int argc, char** argv, Config& c) {
    bool bytes_set = false;

    c.argc_saved = argc;
    c.argv_saved = argv;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = nullptr;

        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 1; // caller treats 1 as "help printed, exit 0"
        }
        if (arg_value(argc, argv, i, "--impl", v)) {
            if (std::strcmp(v, "mutex") == 0) {
                c.impl     = Impl::Mutex;
                c.impl_set = true;
            } else if (std::strcmp(v, "spsc") == 0) {
                c.impl     = Impl::Spsc;
                c.impl_set = true;
            } else {
                std::fprintf(stderr,
                             "--impl must be exactly one of mutex|spsc (got "
                             "'%s'); this benchmark times ONE implementation per "
                             "process by design\n",
                             v);
                return -1;
            }
        } else if (arg_value(argc, argv, i, "--message-bytes", v)) {
            std::uint64_t n = 0;
            if (!parse_u64(v, n) || (n != 8 && n != 32 && n != 64)) {
                std::fprintf(stderr, "--message-bytes must be 8, 32 or 64 (got '%s')\n", v);
                return -1;
            }
            c.message_bytes = static_cast<std::size_t>(n);
            bytes_set       = true;
        } else if (arg_value(argc, argv, i, "--capacity", v)) {
            std::uint64_t n = 0;
            if (!parse_u64(v, n) || !is_supported_capacity(static_cast<std::size_t>(n))) {
                std::fprintf(stderr,
                             "--capacity must be 1024, 4096 or 65536 (got '%s'); "
                             "capacity is dispatched to a compile-time "
                             "specialization, so the set is closed by design\n",
                             v);
                return -1;
            }
            c.capacity = static_cast<std::size_t>(n);
        } else if (arg_value(argc, argv, i, "--messages", v)) {
            if (!parse_u64(v, c.messages) || c.messages < 1) {
                std::fprintf(stderr, "--messages must be >= 1 (got '%s')\n", v);
                return -1;
            }
        } else if (arg_value(argc, argv, i, "--reps", v)) {
            if (!parse_int(v, c.reps) || c.reps < 1) {
                std::fprintf(stderr, "--reps must be >= 1 (got '%s')\n", v);
                return -1;
            }
        } else if (arg_value(argc, argv, i, "--warmup", v)) {
            if (!parse_int(v, c.warmup)) {
                std::fprintf(stderr, "invalid --warmup '%s'\n", v);
                return -1;
            }
        } else if (arg_value(argc, argv, i, "--raw-out", v)) {
            c.raw_out = v;
        } else if (arg_value(argc, argv, i, "--summary-out", v)) {
            c.summary_out = v;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", a);
            usage(argv[0]);
            return -1;
        }
    }

    if (!c.impl_set) {
        std::fprintf(stderr,
                     "--impl is required: exactly one of mutex|spsc. The two "
                     "implementations are never timed in the same process.\n");
        return -1;
    }
    if (!bytes_set) {
        std::fprintf(stderr, "--message-bytes is required (8, 32 or 64)\n");
        return -1;
    }
    if (c.capacity == 0) {
        std::fprintf(stderr, "--capacity is required (1024, 4096 or 65536)\n");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The harness
// ---------------------------------------------------------------------------

struct RepResult {
    std::uint64_t elapsed_ns            = 0;
    double        ns_per_message        = 0.0;
    double        messages_per_second   = 0.0;
    std::uint64_t producer_full_retries = 0;
    std::uint64_t consumer_empty_retries = 0;
    std::uint64_t checksum              = 0;
    bool          ok                    = false;
};

const char* impl_name(Impl i) noexcept {
    return i == Impl::Mutex ? "mutex" : "spsc";
}

// Median of a copy of `values` (odd count -> middle element; even count -> mean
// of the two middle elements). Used for the headline figure. min/max are
// reported alongside it and NO repetition is ever discarded.
double median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    }
    return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// Run `warmup + reps` timed transfers of N messages through `Queue`, keep every
// measured repetition, and report the derived summary.
//
// Template parameters are the whole point of the dispatch: Msg fixes the message
// type, Capacity fixes the queue capacity, Queue fixes the implementation. All
// three are compile-time so neither queue pays for a runtime decision.
template <typename Msg, std::size_t Capacity, typename Queue>
int run_cell(const Config& c) {
    const std::uint64_t n = c.messages;

    // Expected checksum over the exact stream the producer will send, computed
    // ONCE, BEFORE any timing starts. This is what makes the end-of-run checksum
    // comparison a real end-to-end payload check rather than a self-consistency
    // tautology.
    std::uint64_t expected_checksum = 0;
    for (std::uint64_t s = 0; s < n; ++s) {
        expected_checksum = Msg::make(s).fold(expected_checksum);
    }

    std::vector<RepResult> measured;
    measured.reserve(static_cast<std::size_t>(c.reps));

    const int total_reps = c.warmup + c.reps;

    for (int rep = 0; rep < total_reps; ++rep) {
        const bool warmup = rep < c.warmup;

        // ---- everything below this point is OUTSIDE the timed interval ----
        auto q = std::make_unique<Queue>();

        std::atomic<bool> start{false};
        std::atomic<bool> producer_ready{false};
        std::atomic<bool> consumer_ready{false};

        // Written by exactly one thread each; read by main only after join(), so
        // no atomics are needed and no synchronization is implied into the timed
        // path.
        std::uint64_t      producer_full_retries  = 0;
        std::uint64_t      consumer_empty_retries = 0;
        std::uint64_t      checksum               = 0;
        std::uint64_t      delivered              = 0;
        bool               sequence_ok            = true;
        Clock::time_point  consumer_end{};

        std::thread producer([&] {
            producer_ready.store(true, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield(); // ready-wait: NOT part of the measurement
            }
            std::uint64_t misses = 0;
            for (std::uint64_t s = 0; s < n; ++s) {
                const Msg m = Msg::make(s);
                while (!q->try_push(m)) {
                    ++producer_full_retries;
                    if (++misses >= kYieldAfterMisses) {
                        misses = 0;
                        std::this_thread::yield();
                    }
                }
            }
        });

        std::thread consumer([&] {
            consumer_ready.store(true, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield(); // ready-wait: NOT part of the measurement
            }
            std::uint64_t expected = 0;
            std::uint64_t misses   = 0;
            Msg           m{};
            while (expected < n) {
                if (!q->try_pop(m)) {
                    ++consumer_empty_retries;
                    if (++misses >= kYieldAfterMisses) {
                        misses = 0;
                        std::this_thread::yield();
                    }
                    continue;
                }
                // Sequence check: the delivered seq must be exactly the next
                // expected one. That single comparison is simultaneously the
                // no-gap, no-duplicate and no-reordering check.
                if (sequence_ok && m.seq == expected) {
                    checksum = m.fold(checksum);
                } else {
                    sequence_ok = false;
                }
                // Count the pop regardless, so that a failing run still drains
                // exactly N messages and the producer can always finish — a
                // validation failure must never turn into a hung join().
                ++expected;
            }
            delivered    = expected;
            consumer_end = Clock::now();
        });

        while (!producer_ready.load(std::memory_order_acquire) ||
               !consumer_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // ---- timed interval: both threads are ready and parked on `start` ----
        const Clock::time_point t0 = Clock::now();
        start.store(true, std::memory_order_release);

        producer.join();
        consumer.join();
        // ---- end of timed interval; t1 was taken by the CONSUMER itself ----

        const std::uint64_t elapsed_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(consumer_end - t0)
                    .count());

        const bool ok = (delivered == n) && sequence_ok &&
                        (checksum == expected_checksum) && (elapsed_ns > 0);

        RepResult r;
        r.elapsed_ns             = elapsed_ns;
        r.ns_per_message         = static_cast<double>(elapsed_ns) / static_cast<double>(n);
        r.messages_per_second    = static_cast<double>(n) * 1e9 / static_cast<double>(elapsed_ns);
        r.producer_full_retries  = producer_full_retries;
        r.consumer_empty_retries = consumer_empty_retries;
        r.checksum               = checksum;
        r.ok                     = ok;

        if (warmup) {
            std::fprintf(stderr,
                         "[warmup %d/%d] impl=%s bytes=%zu cap=%zu "
                         "elapsed=%" PRIu64 "ns correct=%s (untimed, excluded)\n",
                         rep + 1, c.warmup, impl_name(c.impl), c.message_bytes,
                         Capacity, elapsed_ns, ok ? "yes" : "NO");
        } else {
            measured.push_back(r);
            std::fprintf(stderr,
                         "[rep %zu/%d] impl=%s bytes=%zu cap=%zu elapsed=%" PRIu64
                         "ns ns_per_message=%.6f full_retries=%" PRIu64
                         " empty_retries=%" PRIu64 " correct=%s\n",
                         measured.size(), c.reps, impl_name(c.impl), c.message_bytes,
                         Capacity, elapsed_ns, r.ns_per_message,
                         r.producer_full_retries, r.consumer_empty_retries,
                         ok ? "yes" : "NO");
        }

        if (!ok) {
            std::fprintf(stderr,
                         "\nVALIDATION FAILED (impl=%s bytes=%zu cap=%zu "
                         "delivered=%" PRIu64 "/%" PRIu64 " sequence_ok=%d "
                         "checksum=%" PRIu64 " expected=%" PRIu64 ")\n"
                         "This cell is NOT publishable; exiting non-zero.\n",
                         impl_name(c.impl), c.message_bytes, Capacity, delivered, n,
                         sequence_ok ? 1 : 0, checksum, expected_checksum);
            return 1;
        }
    }

    // ---- derived summary (no second run: everything comes from `measured`) ----

    std::vector<double> ns_values;
    ns_values.reserve(measured.size());
    double        ns_min = 0.0;
    double        ns_max = 0.0;
    std::uint64_t full_retry_total  = 0;
    std::uint64_t empty_retry_total = 0;
    for (std::size_t i = 0; i < measured.size(); ++i) {
        const double v = measured[i].ns_per_message;
        ns_values.push_back(v);
        if (i == 0 || v < ns_min) {
            ns_min = v;
        }
        if (i == 0 || v > ns_max) {
            ns_max = v;
        }
        full_retry_total += measured[i].producer_full_retries;
        empty_retry_total += measured[i].consumer_empty_retries;
    }
    const double ns_median = median_of(ns_values);
    // messages_per_second is the reciprocal of seconds-per-message: since
    // ns_per_message = elapsed_ns / n, we have n / (elapsed_ns * 1e-9)
    // = 1e9 / ns_per_message. The per-repetition rows compute it from their own
    // elapsed_ns; this is the same quantity taken at the median rate.
    const double mps_median = ns_median > 0.0 ? 1e9 / ns_median : 0.0;
    const double spread_pct = ns_min > 0.0 ? (ns_max / ns_min - 1.0) * 100.0 : 0.0;

    // ---- raw per-repetition CSV FIRST (source of truth preserved) ----
    if (!c.raw_out.empty()) {
        std::FILE* f = std::fopen(c.raw_out.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open --raw-out %s\n", c.raw_out.c_str());
            return 2;
        }
        std::fprintf(f,
                     "# Experiment 02 Phase 2 raw repetitions — ONE implementation "
                     "per process.\n"
                     "# ns_per_message = END-TO-END elapsed_ns / messages delivered "
                     "(NOT a per-call latency, NOT one-way handoff).\n"
                     "# All %d measured repetitions are present; %d untimed warm-up "
                     "repetition(s) are excluded.\n"
                     "rep,impl,message_bytes,capacity,message_count,elapsed_ns,"
                     "ns_per_message,messages_per_second,producer_full_retries,"
                     "consumer_empty_retries,checksum,correctness\n",
                     c.reps, c.warmup);
        for (std::size_t i = 0; i < measured.size(); ++i) {
            const RepResult& r = measured[i];
            std::fprintf(f,
                         "%zu,%s,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%.6f,%.3f,%" PRIu64
                         ",%" PRIu64 ",%" PRIu64 ",PASS\n",
                         i, impl_name(c.impl), c.message_bytes, Capacity, n,
                         r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                         r.producer_full_retries, r.consumer_empty_retries,
                         r.checksum);
        }
        if (std::fclose(f) != 0) {
            std::fprintf(stderr, "error closing --raw-out %s\n", c.raw_out.c_str());
            return 2;
        }
    }

    // ---- summary ----
    std::FILE* out = stdout;
    if (!c.summary_out.empty()) {
        out = std::fopen(c.summary_out.c_str(), "w");
        if (out == nullptr) {
            std::fprintf(stderr, "cannot open --summary-out %s\n",
                         c.summary_out.c_str());
            return 2;
        }
    }

    std::fprintf(out,
                 "# Experiment 02 Phase 2 — two-thread END-TO-END message-transfer "
                 "throughput.\n"
                 "# ns_per_message = elapsed_ns / messages delivered: it INCLUDES "
                 "queue synchronization,\n"
                 "# payload assignment, cache-coherence traffic, harness "
                 "retry/backpressure and OS scheduling.\n"
                 "# It is NOT a per-try_push latency, NOT a per-try_pop latency, and "
                 "NOT a one-way handoff time.\n"
                 "# ONE implementation per process: the other implementation was NOT "
                 "timed here.\n"
                 "# exact invocation (argv as received):\n");
    for (int i = 0; i < c.argc_saved; ++i) {
        std::fprintf(out, "#   argv[%d]=%s\n", i, c.argv_saved[i]);
    }
    std::fprintf(out,
                 "impl=%s\n"
                 "message_bytes=%zu\n"
                 "capacity=%zu\n"
                 "message_count=%" PRIu64 "\n"
                 "measured_reps=%zu\n"
                 "warmup_reps=%d\n"
                 "median_ns_per_message=%.6f\n"
                 "min_ns_per_message=%.6f\n"
                 "max_ns_per_message=%.6f\n"
                 "spread_pct=%.3f\n"
                 "median_messages_per_second=%.3f\n"
                 "producer_full_retries_total=%" PRIu64 "\n"
                 "consumer_empty_retries_total=%" PRIu64 "\n"
                 "checksum=%" PRIu64 "\n"
                 "expected_checksum=%" PRIu64 "\n"
                 "correctness=PASS\n"
                 "# per-repetition detail — every measured repetition, none "
                 "discarded:\n"
                 "rep,elapsed_ns,ns_per_message,messages_per_second,"
                 "producer_full_retries,consumer_empty_retries,checksum\n",
                 impl_name(c.impl), c.message_bytes, Capacity, n, measured.size(),
                 c.warmup, ns_median, ns_min, ns_max, spread_pct, mps_median,
                 full_retry_total, empty_retry_total, measured.back().checksum,
                 expected_checksum);
    for (std::size_t i = 0; i < measured.size(); ++i) {
        const RepResult& r = measured[i];
        std::fprintf(out, "%zu,%" PRIu64 ",%.6f,%.3f,%" PRIu64 ",%" PRIu64
                          ",%" PRIu64 "\n",
                     i, r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                     r.producer_full_retries, r.consumer_empty_retries, r.checksum);
    }

    if (out != stdout) {
        if (std::fclose(out) != 0) {
            std::fprintf(stderr, "error closing --summary-out %s\n",
                         c.summary_out.c_str());
            return 2;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Compile-time dispatch — explicit small switches, no runtime-capacity storage.
// ---------------------------------------------------------------------------

template <typename Msg, std::size_t Capacity>
int dispatch_impl(const Config& c) {
    switch (c.impl) {
    case Impl::Mutex:
        return run_cell<Msg, Capacity, lltl::MutexBoundedQueue<Msg, Capacity>>(c);
    case Impl::Spsc:
        return run_cell<Msg, Capacity, lltl::SpscRingBuffer<Msg, Capacity>>(c);
    }
    return 2; // unreachable: the parser accepts exactly two values
}

template <typename Msg>
int dispatch_capacity(const Config& c) {
    switch (c.capacity) {
    case 1024:
        return dispatch_impl<Msg, 1024>(c);
    case 4096:
        return dispatch_impl<Msg, 4096>(c);
    case 65536:
        return dispatch_impl<Msg, 65536>(c);
    default:
        return 2; // unreachable: the parser accepts exactly three values
    }
}

int dispatch_message(const Config& c) {
    switch (c.message_bytes) {
    case 8:
        return dispatch_capacity<Msg8>(c);
    case 32:
        return dispatch_capacity<Msg32>(c);
    case 64:
        return dispatch_capacity<Msg64>(c);
    default:
        return 2; // unreachable: the parser accepts exactly three values
    }
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    const int parsed = parse_args(argc, argv, c);
    if (parsed == 1) {
        return 0; // --help
    }
    if (parsed != 0) {
        return 2;
    }
    return dispatch_message(c);
}
