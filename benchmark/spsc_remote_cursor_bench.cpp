// Experiment 02 Phase 3B — remote cursor caching benchmark.
//
// WHAT THIS MEASURES
//
// The same separated-cursor SPSC ring buffer, with the SAME payload storage,
// the SAME footprint, the SAME cursor placement, the SAME capacity, the SAME
// slot indexing, the SAME publication protocol, the SAME retry/yield harness and
// the SAME message types, run in two variants that differ in exactly ONE
// intended implementation treatment:
//
//     baseline  — the Phase-3A separated algorithm: the producer performs an
//                 acquire load of the consumer's `tail` on EVERY try_push, and
//                 the consumer performs an acquire load of the producer's `head`
//                 on EVERY try_pop.
//     cached    — the producer keeps a thread-owned copy of the consumer's tail
//                 and refreshes it only when that copy says the queue MAY be
//                 full; the consumer keeps a thread-owned copy of the
//                 producer's head and refreshes it only when that copy says the
//                 queue MAY be empty.
//
// The release/acquire publication edge is NOT removed in either variant. What
// changes is how OFTEN the remote cursor is read, not what the read costs or
// what it guarantees. See docs/SPSC_REMOTE_CURSOR_CACHE.md.
//
// ONE IMPLEMENTATION PER PROCESS, by design. A process times exactly one
// (impl, message_bytes, capacity) cell, so the two variants' numbers always come
// from separate processes with separately allocated objects. Adjacent pairing
// plus a balanced AB/BA session order is the runner's job
// (scripts/spsc-remote-cursor.sh), not this program's.
//
// TWO MEASUREMENT MODES, AND WHY THEY ARE NOT INTERCHANGEABLE
//
//   --instrument=0  (CANONICAL)  No counting work on the hot path. The counters
//                   exist as storage (so the object layout is identical to the
//                   instrumented build) but no increment is emitted. Throughput
//                   numbers for the canonical dataset come ONLY from here.
//
//   --instrument=1  (MECHANISM)  Counts every actual remote cursor refresh
//                   (the acquire load) and reports the derived per-message rate.
//                   The counters are ORDINARY THREAD-OWNED counters, not global
//                   atomics: an atomic counter incremented by both threads on
//                   every operation would itself be a contended shared line and
//                   would perturb exactly the effect under study.
//
// A throughput number produced by --instrument=1 is NOT the canonical result and
// this program says so in its output and in its summary. If instrumentation were
// free, there would be no need for two modes; since it is not known to be free,
// the honest design is to measure the mechanism and the performance separately
// and never to present one as the other.
//
// LAYOUT IS A PRECONDITION, NOT A RESULT
//
// Phase 3B inherits Phase 3A's separated cursor layout and equal-footprint
// design. Before ANY timing, the process checks that the two variants produce
// the same object size and the same payload offset for this cell; a disagreement
// is a failed experiment, not a slow cell, and exits 3 having timed nothing.
// Every measured repetition then re-verifies, on the object it actually timed,
// that the cursors are on distinct cache lines, that the payload does not share
// a line with either cursor, and that each cached remote cursor sits on its OWN
// owner's line rather than on the remote thread's. A repetition whose layout
// check fails aborts the cell rather than publishing.
//
// Exit codes: 0 ok, 1 validation failure, 2 usage/IO error, 3 layout invariant
// failure.

#include "cache_line.h"
#include "spsc_cursor_layout_ring_buffer.h"
#include "spsc_remote_cursor_ring_buffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Fixed parameters. These are the Phase-2/3A canonical parameters, unchanged:
// Phase 3B changes ONE program-layout/implementation treatment, so every
// harness parameter must stay where the earlier phases put it.
// ---------------------------------------------------------------------------

constexpr std::uint64_t kDefaultMessages = 10'000'000;
constexpr int           kDefaultReps     = 5;
constexpr int           kDefaultWarmup   = 1;

// The HARDENED Phase-2 retry/yield policy, adopted unchanged: yield only after
// this many CONSECUTIVE failed attempts, and reset the run on any success. A
// retry loop that yields on the first miss would be a different harness and its
// numbers would not be comparable with Phase 2 or Phase 3A.
constexpr int kYieldAfterMisses = 1024;

// The two Phase-3B variants.
enum class Impl { Baseline, Cached };

const char* impl_name(Impl i) noexcept {
    switch (i) {
    case Impl::Baseline:
        return "baseline";
    case Impl::Cached:
        return "cached";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Message types — byte-for-byte the Phase-2/3A shapes, so a Phase-3B number is
// read against the same payload work as its Phase-3A control.
// ---------------------------------------------------------------------------

constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

constexpr std::uint64_t rotl7(std::uint64_t x) noexcept {
    return (x << 7) | (x >> 57);
}

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

// 8-byte message type: exactly 8 bytes, trivially copyable, nothrow assignable.
struct Msg8 {
    static constexpr std::size_t kBytes = 8;
    std::uint64_t seq                   = 0;

    static Msg8 make(std::uint64_t s) noexcept {
        Msg8 m;
        m.seq = s;
        return m;
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return mix64(h ^ (seq + 0x9e3779b97f4a7c15ull));
    }
};

// 32-byte message type: an order-book update spread over four words, all derived
// from the sequence number so a torn or stale payload is detectable, not just a
// plausible one.
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
        m.checksum = fold_words(s, m.price, m.qty);
        return m;
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, checksum);
    }
};

// 64-byte message type: the 32-byte shape plus a folded tail word, filling a
// cache line. Its `fold` deliberately mixes the ENTIRE payload so a partially
// published message cannot pass validation.
struct Msg64 {
    static constexpr std::size_t kBytes = 64;
    std::uint64_t seq                   = 0;
    std::uint64_t price                 = 0;
    std::uint64_t qty                   = 0;
    std::uint64_t checksum              = 0;
    std::uint64_t words[4]              = {0, 0, 0, 0};

    static Msg64 make(std::uint64_t s) noexcept {
        Msg64 m;
        m.seq      = s;
        m.price    = derive_price(s);
        m.qty      = derive_qty(s);
        m.checksum = fold_words(s, m.price, m.qty);
        m.words[0] = rotl7(s);
        m.words[1] = rotl7(m.price);
        m.words[2] = rotl7(m.qty);
        m.words[3] = rotl7(m.checksum);
        return m;
    }
    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, checksum, words[0], words[1],
                          words[2], words[3]);
    }
};

#define LLLT_EXP3B_ASSERT_MSG(T, N)                                          \
    static_assert(sizeof(T) == (N), "message type must be exactly " #N       \
                                   " bytes");                                \
    static_assert(std::is_trivially_copyable<T>::value,                      \
                  "message type must be trivially copyable");                \
    static_assert(std::is_nothrow_copy_assignable<T>::value,                 \
                  "message type must be nothrow copy-assignable");           \
    static_assert(std::is_nothrow_move_assignable<T>::value,                 \
                  "message type must be nothrow move-assignable");           \
    static_assert(std::is_nothrow_default_constructible<T>::value,           \
                  "message type must be nothrow default-constructible")

LLLT_EXP3B_ASSERT_MSG(Msg8, 8);
LLLT_EXP3B_ASSERT_MSG(Msg32, 32);
LLLT_EXP3B_ASSERT_MSG(Msg64, 64);

#undef LLLT_EXP3B_ASSERT_MSG

// ---------------------------------------------------------------------------
// Cross-variant footprint gate. Runs BEFORE anything is timed.
//
// Both Phase-3B variants instantiate the SAME cursor policy type, so this holds
// by construction; it is checked on real objects anyway, because "identical by
// construction" is a claim about the source, and the dataset's claim is about
// the objects that were actually measured.
// ---------------------------------------------------------------------------

template <typename Msg, std::size_t Capacity, typename Queue>
int verify_cross_variant_footprint(std::size_t reported_line_size,
                                   Impl               this_impl) {
    using Other = std::conditional_t<
        std::is_same_v<Queue,
                       lltl::SpscSeparatedBaselineRingBuffer<Msg, Capacity>>,
        lltl::SpscSeparatedCachedCursorRingBuffer<Msg, Capacity>,
        lltl::SpscSeparatedBaselineRingBuffer<Msg, Capacity>>;

    auto mine  = std::make_unique<Queue>();
    auto other = std::make_unique<Other>();

    if (sizeof(Queue) != sizeof(Other)) {
        std::fprintf(stderr,
                     "\nCROSS-VARIANT FOOTPRINT INVARIANT FAILED (bytes=%zu "
                     "capacity=%zu): object size differs (%zu vs %zu).\n"
                     "A measured throughput difference could then be caused by "
                     "the object layout rather than by remote-load frequency.\n"
                     "This is a FAILED EXPERIMENT, not a slow cell. Exiting "
                     "non-zero without timing anything.\n",
                     Msg::kBytes, Capacity, sizeof(Queue), sizeof(Other));
        return 3;
    }

    const auto ra = mine->cursor_layout_report(reported_line_size);
    const auto rb = other->cursor_layout_report(reported_line_size);

    const bool agree = (ra.object_size() == rb.object_size()) &&
                       (ra.payload_offset_from_object_base() ==
                        rb.payload_offset_from_object_base()) &&
                       lltl::remote_footprints_agree(ra, rb);

    std::fprintf(stderr,
                 "[footprint] bytes=%zu capacity=%zu impl=%s "
                 "object_size=%zu payload_offset=%zu vs other=%zu/%zu "
                 "cursor_policy_size=%zu cross_variant=%s\n",
                 Msg::kBytes, Capacity, impl_name(this_impl), ra.object_size(),
                 ra.payload_offset_from_object_base(), rb.object_size(),
                 rb.payload_offset_from_object_base(),
                 // Both variants use one policy type, so either accessor gives
                 // the same answer; reported once for the record.
                 Queue::cursor_policy_size(), agree ? "PASS" : "FAIL");

    if (!agree) {
        std::fprintf(stderr,
                     "\nCROSS-VARIANT FOOTPRINT INVARIANT FAILED (bytes=%zu "
                     "capacity=%zu).\n"
                     "The two variants do not produce the same object layout: "
                     "object size or payload offset differs, so a measured\n"
                     "throughput difference could be caused by the payload's "
                     "position rather than by remote-load frequency.\n"
                     "This is a FAILED EXPERIMENT, not a slow cell. Exiting "
                     "non-zero without timing anything.\n",
                     Msg::kBytes, Capacity);
        return 3;
    }
    return 0;
}

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
    bool          instrument    = false;
    std::string   raw_out;
    std::string   summary_out;
    int                argc_saved = 0;
    const char* const* argv_saved = nullptr;

    const char* measurement_mode() const noexcept {
        // The label that goes into the data. "performance" and "mechanism" are
        // never allowed to look like the same kind of number.
        return instrument ? "mechanism" : "performance";
    }
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
        "usage: %s --impl=baseline|cached --message-bytes=8|32|64 "
        "--capacity=1024|4096|65536\n"
        "          [--messages=N] [--reps=R] [--warmup=W] [--instrument=0|1]\n"
        "          [--raw-out=FILE] [--summary-out=FILE]\n"
        "\n"
        "Experiment 02 Phase 3B — remote cursor caching. The ONE implementation\n"
        "treatment is how OFTEN each thread reads the opposite thread's cursor.\n"
        "Cursor placement stays the verified SEPARATED layout in both variants,\n"
        "and both variants have the same footprint, so the payload keeps the same\n"
        "relative offset. The release/acquire publication edge is present in\n"
        "both variants and is NOT removed by the cache.\n"
        "ONE implementation per process: run each --impl as a separate invocation\n"
        "(scripts/spsc-remote-cursor.sh does this, balanced AB/BA).\n"
        "\n"
        "  --impl        REQUIRED, exactly one of baseline|cached.\n"
        "                baseline = Phase-3A separated algorithm: one remote\n"
        "                acquire load per try_push / try_pop. cached = thread-owned\n"
        "                copies of the remote cursor, refreshed only on the\n"
        "                may-be-full / may-be-empty path.\n"
        "  --message-bytes  fixed-size nothrow message type (8, 32 or 64 bytes)\n"
        "  --capacity    queue capacity, dispatched to a compile-time "
        "specialization\n"
        "  --messages    messages to transfer per repetition (default %" PRIu64 ")\n"
        "  --reps        MEASURED repetitions per cell (default %d, all kept)\n"
        "  --warmup      warm-up repetitions per process, EXCLUDED from all "
        "reported and\n"
        "                derived data (default %d)\n"
        "  --instrument  0 = CANONICAL performance mode (no hot-path counting).\n"
        "                1 = MECHANISM mode: count actual remote cursor refreshes\n"
        "                using thread-owned ordinary counters. A throughput number\n"
        "                from mode 1 is NOT the canonical result.\n"
        "  --raw-out     write raw per-repetition CSV here (source of truth)\n"
        "  --summary-out write the derived summary here (default stdout)\n"
        "\n"
        "The host's cache-line size is queried at runtime, and the ACTUAL cursor,\n"
        "cached-state and payload addresses of every measured object are checked\n"
        "against it. If the host reports a line larger than the compile-time\n"
        "layout assumption, or if the two variants do not produce the same object\n"
        "size and payload offset, this program exits non-zero WITHOUT timing\n"
        "anything rather than publishing data whose controls are unverified.\n",
        argv0, kDefaultMessages, kDefaultReps, kDefaultWarmup);
}

// ---- argument helpers (same shape as the Phase-2/3A tools) ----------------

bool parse_u64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char*                    end = nullptr;
    const unsigned long long v   = std::strtoull(s, &end, 10);
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
            if (std::strcmp(v, "baseline") == 0) {
                c.impl     = Impl::Baseline;
                c.impl_set = true;
            } else if (std::strcmp(v, "cached") == 0) {
                c.impl     = Impl::Cached;
                c.impl_set = true;
            } else {
                std::fprintf(stderr,
                             "--impl must be exactly one of baseline|cached (got "
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
            if (!parse_u64(v, n) ||
                !is_supported_capacity(static_cast<std::size_t>(n))) {
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
        } else if (arg_value(argc, argv, i, "--instrument", v)) {
            std::uint64_t n = 0;
            if (!parse_u64(v, n) || n > 1) {
                std::fprintf(stderr, "--instrument must be 0 or 1 (got '%s')\n", v);
                return -1;
            }
            c.instrument = (n == 1);
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
                     "--impl is required: exactly one of baseline|cached. "
                     "Implementations are never timed in the same process.\n");
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
    std::uint64_t elapsed_ns             = 0;
    double        ns_per_message         = 0.0;
    double        messages_per_second    = 0.0;
    std::uint64_t producer_full_retries  = 0;
    std::uint64_t consumer_empty_retries = 0;
    std::uint64_t checksum               = 0;

    // MEASURED MECHANISM. Zero in the canonical build, and the `instrumented`
    // flag below is what keeps "not counted" distinguishable from "counted
    // zero".
    std::uint64_t producer_remote_tail_loads = 0;
    std::uint64_t consumer_remote_head_loads = 0;

    bool ok = false;

    // MEASURED LAYOUT, on the exact object this repetition timed.
    lltl::RemoteCursorLayoutReport layout{};
};

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
// measured repetition, verify the layout of EVERY queue object actually
// measured, and report the derived summary.
template <typename Msg, std::size_t Capacity, typename Queue>
int run_cell(const Config& c, std::size_t reported_line_size) {
    const std::uint64_t n = c.messages;

    // Expected checksum over the exact stream the producer will send, computed
    // ONCE, BEFORE any timing starts.
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

        // ---- runtime layout verification (per repetition, per object) ------
        //
        // A fresh queue object is allocated for every repetition, so the cursor
        // and cached-state ADDRESSES change between repetitions. The Phase-3A
        // separated invariant AND the Phase-3B cached-placement invariant are
        // therefore re-verified on the object that is about to be measured, not
        // asserted once from the type.
        const lltl::RemoteCursorLayoutReport layout =
            q->cursor_layout_report(reported_line_size);
        std::fprintf(stderr, "[layout rep %d/%d] %s\n", rep + 1, total_reps,
                     layout.summary_line().c_str());
        if (!layout.ok()) {
            std::fprintf(stderr,
                         "\nLAYOUT INVARIANT FAILED (impl=%s bytes=%zu cap=%zu "
                         "rep=%d).\n"
                         "cursor_ok=%d cached_placement_ok=%d\n"
                         "Phase 3B changes ONLY the frequency of remote cursor "
                         "loads, so an unverified cursor placement or an "
                         "unverified cached-state placement is a FAILED "
                         "EXPERIMENT, not a slow cell. Exiting non-zero without "
                         "timing anything.\n",
                         impl_name(c.impl), c.message_bytes, Capacity, rep + 1,
                         layout.cursor.ok() ? 1 : 0, layout.ok_cached() ? 1 : 0);
            return 3;
        }

        std::atomic<bool> start{false};
        std::atomic<bool> producer_ready{false};
        std::atomic<bool> consumer_ready{false};

        std::uint64_t producer_full_retries  = 0;
        std::uint64_t consumer_empty_retries = 0;
        std::uint64_t checksum               = 0;
        std::uint64_t delivered              = 0;
        bool          sequence_ok            = true;
        Clock::time_point consumer_end{};

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
                misses = 0; // success: the consecutive-miss run is over
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
                misses = 0; // success: the consecutive-miss run is over
                if (sequence_ok && m.seq == expected) {
                    checksum = m.fold(checksum);
                } else {
                    sequence_ok = false;
                }
                ++expected; // count regardless, so a failure can never hang join()
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

        const std::uint64_t elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(consumer_end - t0)
                .count());

        // The refresh counters are thread-owned ordinary counters, so they are
        // read here, AFTER both threads have been joined, and never during the
        // measurement.
        const std::uint64_t ptl = q->producer_remote_tail_loads();
        const std::uint64_t chr = q->consumer_remote_head_loads();

        // In the canonical (uninstrumented) build the counters must be
        // untouched. Asserting it here means a build that accidentally counted
        // would fail rather than silently publish an instrumented number as the
        // canonical one.
        if (!c.instrument && (ptl != 0 || chr != 0)) {
            std::fprintf(stderr,
                         "\nINSTRUMENTATION LEAK (impl=%s bytes=%zu cap=%zu "
                         "rep=%d): counters are %" PRIu64 "/%" PRIu64 " in an "
                         "uninstrumented run.\n"
                         "A throughput number from an instrumented run is not "
                         "the canonical result; exiting non-zero.\n",
                         impl_name(c.impl), c.message_bytes, Capacity, rep + 1,
                         ptl, chr);
            return 3;
        }

        const bool ok = (delivered == n) && sequence_ok &&
                        (checksum == expected_checksum) && (elapsed_ns > 0);

        RepResult r;
        r.elapsed_ns             = elapsed_ns;
        r.ns_per_message = static_cast<double>(elapsed_ns) / static_cast<double>(n);
        r.messages_per_second =
            static_cast<double>(n) * 1e9 / static_cast<double>(elapsed_ns);
        r.producer_full_retries  = producer_full_retries;
        r.consumer_empty_retries = consumer_empty_retries;
        r.checksum               = checksum;
        r.producer_remote_tail_loads = ptl;
        r.consumer_remote_head_loads = chr;
        r.ok                         = ok;
        r.layout                     = layout;

        if (warmup) {
            std::fprintf(stderr,
                         "[warmup %d/%d] impl=%s bytes=%zu cap=%zu "
                         "elapsed=%" PRIu64 "ns correct=%s (excluded from reported "
                         "data)\n",
                         rep + 1, c.warmup, impl_name(c.impl), c.message_bytes,
                         Capacity, elapsed_ns, ok ? "yes" : "NO");
        } else {
            measured.push_back(r);
            std::fprintf(stderr,
                         "[rep %zu/%d] mode=%s impl=%s bytes=%zu cap=%zu "
                         "elapsed=%" PRIu64 "ns ns_per_message=%.6f "
                         "full_retries=%" PRIu64 " empty_retries=%" PRIu64
                         " correct=%s\n",
                         measured.size(), c.reps, c.measurement_mode(),
                         impl_name(c.impl), c.message_bytes, Capacity,
                         elapsed_ns, r.ns_per_message, r.producer_full_retries,
                         r.consumer_empty_retries, ok ? "yes" : "NO");
            if (c.instrument) {
                std::fprintf(stderr,
                             "            remote_loads: producer_tail=%" PRIu64
                             " consumer_head=%" PRIu64 "\n",
                             r.producer_remote_tail_loads,
                             r.consumer_remote_head_loads);
            }
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
    std::uint64_t ptl_total         = 0;
    std::uint64_t chr_total         = 0;
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
        ptl_total += measured[i].producer_remote_tail_loads;
        chr_total += measured[i].consumer_remote_head_loads;
    }
    const double ns_median  = median_of(ns_values);
    const double mps_median = ns_median > 0.0 ? 1e9 / ns_median : 0.0;
    const double spread_pct = ns_min > 0.0 ? (ns_max / ns_min - 1.0) * 100.0 : 0.0;

    const double total_messages =
        static_cast<double>(measured.size() * n);
    const double ptl_per_message =
        total_messages > 0.0 ? static_cast<double>(ptl_total) / total_messages
                             : 0.0;
    const double chr_per_message =
        total_messages > 0.0 ? static_cast<double>(chr_total) / total_messages
                             : 0.0;

    // ---- raw per-repetition CSV FIRST (source of truth preserved) ----
    //
    // The layout columns are part of the source of truth: they are the address
    // evidence for the very repetitions the throughput columns describe.
    if (!c.raw_out.empty()) {
        std::FILE* f = std::fopen(c.raw_out.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open --raw-out %s\n", c.raw_out.c_str());
            return 2;
        }
        std::fprintf(f,
                     "# Experiment 02 Phase 3B raw repetitions — ONE implementation "
                     "per process.\n"
                     "# ns_per_message = END-TO-END elapsed_ns / messages delivered "
                     "(NOT a per-call latency, NOT one-way handoff).\n"
                     "# The ONE implementation treatment is HOW OFTEN each thread "
                     "reads the opposite thread's cursor. Cursor placement is the\n"
                     "# verified SEPARATED layout in BOTH variants, and both "
                     "variants have the same object_size and payload_offset.\n"
                     "# measurement_mode=performance means no hot-path counting was "
                     "compiled in; remote_load columns are 0 and instrumented=0.\n"
                     "# measurement_mode=mechanism means the counters were live; a "
                     "throughput number from those rows is NOT the canonical result.\n"
                     "# remote loads are ORDINARY THREAD-OWNED counters, read after "
                     "both threads joined — never global atomics on the hot path.\n"
                     "# The cursor/cached addresses and line indices below were "
                     "measured on the SAME object this repetition timed,\n"
                     "# OUTSIDE the timed interval. layout_ok=PASS means the measured "
                     "placement matched the claim; cached_placement_ok=PASS means\n"
                     "# each cached remote cursor sat on its OWN owner's line and not "
                     "on the remote cursor's line.\n"
                     "# All %d measured repetitions are present; %d warm-up "
                     "repetition(s) are excluded from every published figure.\n"
                     "rep,measurement_mode,impl,message_bytes,capacity,message_count,"
                     "elapsed_ns,ns_per_message,messages_per_second,"
                     "producer_full_retries,consumer_empty_retries,checksum,"
                     "correctness,instrumented,producer_remote_tail_loads,"
                     "consumer_remote_head_loads,producer_remote_tail_loads_per_message,"
                     "consumer_remote_head_loads_per_message,"
                     "reported_cache_line_size,head_addr,tail_addr,head_line,"
                     "tail_line,cursors_same_line,layout_ok,"
                     "cached_tail_addr,cached_head_addr,cached_tail_line,"
                     "cached_head_line,cached_placement_ok,"
                     "object_addr,object_size,payload_offset,payload_begin_addr\n",
                     c.reps, c.warmup);
        for (std::size_t i = 0; i < measured.size(); ++i) {
            const RepResult& r = measured[i];
            const double ptl_rate =
                static_cast<double>(r.producer_remote_tail_loads) /
                static_cast<double>(n);
            const double chr_rate =
                static_cast<double>(r.consumer_remote_head_loads) /
                static_cast<double>(n);
            std::fprintf(
                f,
                "%zu,%s,%s,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%.6f,%.3f,%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",PASS,%s,%" PRIu64 ",%" PRIu64
                ",%.9f,%.9f,"
                "%zu,%" PRIuPTR ",%" PRIuPTR ",%zu,%zu,%s,%s,"
                "%" PRIuPTR ",%" PRIuPTR ",%zu,%zu,%s,"
                "%" PRIuPTR ",%zu,%zu,%" PRIuPTR "\n",
                i, c.measurement_mode(), impl_name(c.impl), c.message_bytes,
                Capacity, n, r.elapsed_ns, r.ns_per_message,
                r.messages_per_second, r.producer_full_retries,
                r.consumer_empty_retries, r.checksum,
                c.instrument ? "1" : "0", r.producer_remote_tail_loads,
                r.consumer_remote_head_loads, ptl_rate, chr_rate,
                r.layout.reported_line_size(),
                static_cast<std::uintptr_t>(r.layout.head_address()),
                static_cast<std::uintptr_t>(r.layout.tail_address()),
                r.layout.head_line_index(), r.layout.tail_line_index(),
                r.layout.cursor.cursors_same_line ? "yes" : "no",
                r.layout.cursor.ok() ? "PASS" : "FAIL",
                static_cast<std::uintptr_t>(r.layout.cached_tail_address),
                static_cast<std::uintptr_t>(r.layout.cached_head_address),
                r.layout.cached_tail_line_index, r.layout.cached_head_line_index,
                r.layout.ok_cached() ? "PASS" : "FAIL",
                static_cast<std::uintptr_t>(r.layout.object_address()),
                r.layout.object_size(), r.layout.payload_offset_from_object_base(),
                static_cast<std::uintptr_t>(r.layout.payload_begin()));
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
                 "# Experiment 02 Phase 3B — REMOTE CURSOR CACHING.\n"
                 "# The ONE implementation treatment is the FREQUENCY of remote "
                 "cursor loads: the baseline performs one acquire\n"
                 "# load of the opposite cursor per try_push / try_pop; the cached "
                 "variant keeps a thread-owned copy and refreshes\n"
                 "# it only when that copy says the queue MAY be full (producer) "
                 "or MAY be empty (consumer).\n"
                 "# Everything else is identical by construction: same payload "
                 "storage, same object footprint, same SEPARATED\n"
                 "# cursor placement, same capacity, same slot indexing, same "
                 "publication protocol, same retry/yield harness,\n"
                 "# same message types. There is no batching, no memory-order "
                 "change, no CAS, no affinity and no NUMA tuning.\n"
                 "# The release/acquire publication edge EXISTS IN BOTH VARIANTS. "
                 "The cache changes how OFTEN the remote cursor\n"
                 "# is read, not what reading it guarantees; no memory order was "
                 "weakened or removed.\n"
                 "# ns_per_message = elapsed_ns / messages delivered: it INCLUDES "
                 "queue synchronization, payload assignment,\n"
                 "# cache-coherence traffic, harness retry/backpressure and OS "
                 "scheduling. It is NOT a per-try_push latency,\n"
                 "# NOT a per-try_pop latency, and NOT a one-way handoff time.\n"
                 "# ONE implementation per process: the other implementation was "
                 "NOT timed here.\n"
                 "# This summary describes THIS PROCESS ONLY. It is not a canonical\n"
                 "# result on its own; the canonical dataset pools repetitions "
                 "across balanced processes.\n"
                 "# exact invocation (argv as received):\n");
    for (int i = 0; i < c.argc_saved; ++i) {
        std::fprintf(out, "#   argv[%d]=%s\n", i, c.argv_saved[i]);
    }
    std::fprintf(out,
                 "measurement_mode=%s\n"
                 "instrumented=%s\n"
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
                 "correctness=PASS\n",
                 c.measurement_mode(), c.instrument ? "yes" : "no",
                 impl_name(c.impl), c.message_bytes, Capacity, n, measured.size(),
                 c.warmup, ns_median, ns_min, ns_max, spread_pct, mps_median,
                 full_retry_total, empty_retry_total, measured.back().checksum,
                 expected_checksum);

    // MEASURED MECHANISM, kept in its own block. "instrumented=no" plus zero
    // counters means "not counted"; it must never be read as "counted zero".
    std::fprintf(out,
                 "# ---- MEASURED MECHANISM (only meaningful when "
                 "instrumented=yes) ----\n"
                 "producer_remote_tail_loads_total=%" PRIu64 "\n"
                 "consumer_remote_head_loads_total=%" PRIu64 "\n"
                 "producer_remote_tail_loads_per_message=%.9f\n"
                 "consumer_remote_head_loads_per_message=%.9f\n"
                 "remote_load_counters=thread_owned_ordinary_counters\n"
                 "# In the baseline these equal the try_push / try_pop CALL counts "
                 "and are reported for verification only.\n"
                 "# In the cached variant they count ACTUAL refresh loads, which is "
                 "the mechanism the experiment is about.\n",
                 ptl_total, chr_total, ptl_per_message, chr_per_message);

    // MEASURED LAYOUT.
    std::fprintf(out,
                 "# ---- MEASURED LAYOUT (Phase-3A separated + Phase-3B cached "
                 "placement) ----\n"
                 "layout_invariant=%s\n"
                 "cached_placement_invariant=%s\n"
                 "layout_first_measured_rep=%s\n"
                 "cursor_policy_size=%zu\n"
                 "object_size=%zu\n"
                 "payload_offset_from_object_base=%zu\n"
                 "cursor_layout=%s\n"
                 "cache_line_size_reported=%zu\n"
                 "cache_line_size_assumed=%zu\n"
                 "# object_size and payload_offset_from_object_base must be "
                 "identical for the baseline and cached\n"
                 "# processes of this cell; the canonical runner compares them "
                 "across the pair and refuses to publish\n"
                 "# if they differ.\n",
                 measured.front().layout.cursor.ok() ? "PASS" : "FAIL",
                 measured.front().layout.ok_cached() ? "PASS" : "FAIL",
                 measured.front().layout.summary_line().c_str(),
                 Queue::cursor_policy_size(),
                 measured.front().layout.object_size(),
                 measured.front().layout.payload_offset_from_object_base(),
                 lltl::cursor_layout_name(measured.front().layout.layout()),
                 reported_line_size, lltl::kAssumedCacheLineSize);

    std::fprintf(out,
                 "# per-repetition detail — every measured repetition, none "
                 "discarded:\n"
                 "rep,elapsed_ns,ns_per_message,messages_per_second,"
                 "producer_full_retries,consumer_empty_retries,checksum,"
                 "head_line,tail_line,cached_tail_line,cached_head_line\n");
    for (std::size_t i = 0; i < measured.size(); ++i) {
        const RepResult& r = measured[i];
        std::fprintf(out,
                     "%zu,%" PRIu64 ",%.6f,%.3f,%" PRIu64 ",%" PRIu64
                     ",%" PRIu64 ",%zu,%zu,%zu,%zu\n",
                     i, r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                     r.producer_full_retries, r.consumer_empty_retries,
                     r.checksum, r.layout.head_line_index(),
                     r.layout.tail_line_index(), r.layout.cached_tail_line_index,
                     r.layout.cached_head_line_index);
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

// The cross-variant footprint gate runs FIRST, before anything is timed, so a
// cell whose object layout moved between the variants can never reach the point
// of producing numbers.
template <typename Msg, std::size_t Capacity, typename Queue>
int run_gated_cell(const Config& c, std::size_t line) {
    const int footprint =
        verify_cross_variant_footprint<Msg, Capacity, Queue>(line, c.impl);
    if (footprint != 0) {
        return footprint;
    }
    return run_cell<Msg, Capacity, Queue>(c, line);
}

// Phase 3B has FOUR instantiations per (message, capacity): two variants times
// two instrumentation settings. Both settings share one source body; the
// template parameter is what makes the counting compile-time rather than a
// runtime branch, so the canonical build carries no counting work at all.
template <typename Msg, std::size_t Capacity>
int dispatch_variant(const Config& c, std::size_t line) {
    using BaselinePlain =
        lltl::RemoteCursorRingBuffer<Msg, Capacity, lltl::RemoteCursorMode::Direct,
                                     false>;
    using BaselineInstr =
        lltl::RemoteCursorRingBuffer<Msg, Capacity, lltl::RemoteCursorMode::Direct,
                                     true>;
    using CachedPlain =
        lltl::RemoteCursorRingBuffer<Msg, Capacity, lltl::RemoteCursorMode::Cached,
                                     false>;
    using CachedInstr =
        lltl::RemoteCursorRingBuffer<Msg, Capacity, lltl::RemoteCursorMode::Cached,
                                     true>;

    switch (c.impl) {
    case Impl::Baseline:
        return c.instrument ? run_gated_cell<Msg, Capacity, BaselineInstr>(c, line)
                            : run_gated_cell<Msg, Capacity, BaselinePlain>(c, line);
    case Impl::Cached:
        return c.instrument ? run_gated_cell<Msg, Capacity, CachedInstr>(c, line)
                            : run_gated_cell<Msg, Capacity, CachedPlain>(c, line);
    }
    return 2; // unreachable: the parser accepts exactly two values
}

template <typename Msg>
int dispatch_capacity(const Config& c, std::size_t line) {
    switch (c.capacity) {
    case 1024:
        return dispatch_variant<Msg, 1024>(c, line);
    case 4096:
        return dispatch_variant<Msg, 4096>(c, line);
    case 65536:
        return dispatch_variant<Msg, 65536>(c, line);
    default:
        return 2; // unreachable: the parser accepts exactly three values
    }
}

int dispatch_message(const Config& c, std::size_t line) {
    switch (c.message_bytes) {
    case 8:
        return dispatch_capacity<Msg8>(c, line);
    case 32:
        return dispatch_capacity<Msg32>(c, line);
    case 64:
        return dispatch_capacity<Msg64>(c, line);
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

    // -----------------------------------------------------------------------
    // The experiment's own precondition, checked BEFORE anything is timed.
    //
    // The compile-time alignment creates the INTENDED candidate layout; the
    // per-object address check in each repetition is what is authoritative. This
    // early guard rejects a host whose reported interference block is larger than
    // the layout was built for, because there the compile-time alignment can no
    // longer guarantee that the separated cursor blocks land in distinct real
    // blocks — and a control that is not what it claims would make the
    // experiment report the opposite of the truth. There is no safe way to
    // continue on such a host: FAIL, and publish nothing.
    // -----------------------------------------------------------------------
    const std::size_t reported_line = lltl::reported_cache_line_size();
    std::fprintf(stderr,
                 "[host] cache_line_size_reported=%zu cache_line_size_assumed=%zu\n",
                 reported_line, lltl::kAssumedCacheLineSize);
    if (!lltl::line_size_supported(reported_line)) {
        std::fprintf(stderr,
                     "\nLAYOUT ASSUMPTION UNSUPPORTED BY HOST: reported cache-line "
                     "size %zu is not usable with the compile-time layout "
                     "assumption of %zu bytes.\n"
                     "Phase 3B keeps the verified separated cursor layout, so a "
                     "host whose real line size exceeds this\n"
                     "assumption cannot be relied on to keep the two cursor blocks "
                     "distinct.\nRefusing to measure; exiting non-zero rather than "
                     "publishing data whose layout is unverified.\n",
                     reported_line, lltl::kAssumedCacheLineSize);
        return 3;
    }

    // -----------------------------------------------------------------------
    // The canonical/mechanism distinction is a property of the RUN, not a note
    // in a document. It is printed before any measurement so a captured log can
    // never be mistaken for the other mode.
    // -----------------------------------------------------------------------
    std::fprintf(stderr,
                 "[mode] measurement_mode=%s instrumented=%s "
                 "(canonical throughput figures come ONLY from "
                 "measurement_mode=performance)\n",
                 c.measurement_mode(), c.instrument ? "yes" : "no");

    return dispatch_message(c, reported_line);
}
