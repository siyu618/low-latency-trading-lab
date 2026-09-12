// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3A: the CONTROLLED
// CURSOR-PLACEMENT (coherence-layout) experiment.
//
// RESEARCH QUESTION
//   Holding the algorithm, the payload storage and indexing, the full/empty
//   semantics, the memory orders, the retry policy and the message types fixed,
//   how does two-thread end-to-end throughput change when the producer-owned
//   head cursor and the consumer-owned tail cursor are FORCED to share one cache
//   line, versus being placed in distinct cache lines?
//
//   Exactly ONE variable is changed: cursor cache-line placement. Both cursor
//   policies have the SAME footprint, so the payload array that follows them
//   starts at the SAME object offset in both variants and the payload does not
//   move between cache sets (see EQUAL FOOTPRINT below). The two variants are
//   one algorithm body with two layout policies — see
//   include/spsc_cursor_layout_ring_buffer.h. No cached remote cursor, no
//   batching, no CAS, no affinity, no memory-order change: those are Phase 3B and
//   later, and combining any of them here would destroy the attribution.
//
// WHAT THE MEASURED DIFFERENCE DOES AND DOES NOT ISOLATE
//   The two cursors are not purely independent write-only state. The producer
//   writes head and READS tail (acquire) at the reuse gate; the consumer writes
//   tail and READS head (acquire) at the availability gate. Those reads are
//   required for correctness and are present in BOTH variants.
//
//   So cursor placement moves two things at once, by construction:
//     A. line-granularity interference between the two INDEPENDENT own-cursor
//        WRITES (the false-sharing component), which separating removes; and
//     B. whether the two legitimately shared cursor values sit on one coherence
//        line (which may reduce the coherence working set) or on two.
//
//   A wall-clock same-line vs separated difference therefore measures CONTROLLED
//   CURSOR PLACEMENT, not "pure false-sharing cost". A separated-faster result
//   is consistent with reduced false-sharing interference; it does not by itself
//   prove that false sharing caused the whole measured gap. This tool has no
//   hardware counters, so it cannot decompose the two. See
//   docs/SPSC_FALSE_SHARING.md.
//
// WHAT IS MEASURED (and what is not)
//   Identical to Phase 2: `ns_per_message` is the END-TO-END elapsed wall time
//   for the complete transfer of N messages divided by the N messages delivered.
//   It includes queue synchronization, payload assignment, cache-coherence
//   traffic, the harness's retry/backpressure behaviour and OS scheduling. It is
//   NOT a per-try_push or per-try_pop latency and NOT a one-way handoff time.
//
// WHY THIS IS NOT THE PHASE-2 BENCHMARK
//   docs/results/spsc-throughput/ is a FROZEN canonical dataset and Phase-2
//   tooling is not touched by Phase 3A. This is a separate binary with its own
//   result directory (docs/results/spsc-false-sharing/). The frozen natural
//   (unpadded) SPSC from Phase 1 remains the historical baseline; it appears here
//   only as an OBSERVATIONAL optional row (`--impl=natural`) and is explicitly
//   NOT part of any causal comparison, because an unpadded pair of adjacent
//   cursors is not by itself evidence of same-line placement (Phase 2 had no
//   address verification at all).
//
// EQUAL FOOTPRINT — the Phase 3A.1 controlled-variable requirement
//   In the original Phase-3A design the same-line policy was 128 bytes and the
//   separated policy 256, so the payload array declared after them began at
//   object offset 128 in one variant and 256 in the other — a second changed
//   variable that also moved the payload into a different cache set. Both
//   policies are now 2 * kAssumedCacheLineSize bytes: the same-line variant keeps
//   BOTH cursors in the first line and reserves an inert second line purely to
//   match the footprint. This tool refuses to publish unless, for the exact
//   message type and capacity being timed, the two variants agree on object size
//   and on payload offset from the object base.
//
// THE LAYOUT EVIDENCE IS PART OF THE MEASUREMENT
//   An experiment that does not verify the layout it claims is worthless, so this
//   tool refuses to publish without it:
//
//     * the host's cache-line size is QUERIED AT RUNTIME (sysctlbyname on
//       Apple, sysconf on Linux). The canonical host reports 128 bytes, not the
//       64 most code assumes;
//     * if the host reports a line LARGER than the compile-time layout
//       assumption, the process FAILS before timing anything (the separated
//       control's blocks could fall inside one real line and the experiment would
//       report the opposite of the truth);
//     * the CROSS-VARIANT FOOTPRINT GATE runs before timing: both policies are
//       instantiated for this message type and capacity and must agree on object
//       size and payload offset, on real objects, not merely by construction;
//     * for every repetition, the actual cursor addresses of the queue object
//       that is about to be measured are converted to line indices under that
//       reported size, and the layout invariant (same-line: indices EQUAL;
//       separated: indices DIFFERENT) must hold, as must cursor/payload line
//       disjointness. A repetition whose invariant is false terminates the
//       process non-zero and the cell is not publishable;
//     * all of this happens OUTSIDE the timed interval, and the addresses, the
//       object size and the payload offset are recorded per repetition in the raw
//       CSV.
//
// ONE IMPLEMENTATION PER PROCESS
//   --impl=same_line, --impl=separated and --impl=natural are separate
//   invocations. No two of them are ever timed inside one interval or one
//   address space. scripts/spsc-false-sharing.sh runs the canonical matrix that
//   way, pairing the two layouts of a bytes/capacity cell as ADJACENT processes
//   with the order balanced AB/BA across four sessions.
//
// WHAT A REPETITION AND A SESSION ARE — AND ARE NOT
//   Every repetition constructs a FRESH producer thread, a FRESH consumer
//   thread, a fresh queue object (hence fresh cursor ADDRESSES, which is why the
//   layout is re-verified every repetition) and fresh start/ready flags. A
//   repetition does not inherit thread placement from the repetition before it.
//   A "session" is only a grouping of repetitions inside one process/address-
//   space lifetime. Between-repetition and between-process variation may reflect
//   scheduler placement, migration, P/E-core selection, DVFS and thermal state,
//   allocator/address placement and background load. This tool measures the
//   resulting distribution; it does not identify which factor produced any
//   particular fast or slow run.
//
// RETRY / BACKPRESSURE POLICY
//   Byte-for-byte the Phase-2 policy: a failed try_push/try_pop is counted and
//   retried immediately; std::this_thread::yield() is called only after
//   kYieldAfterMisses CONSECUTIVE failures, and the consecutive-miss counter is
//   reset by every success. Neither queue ever waits on queue state internally.
//   The retry counters are observable backpressure metrics, not failures.
//   Note for interpretation: because a failed attempt feeds straight back into
//   the next attempt, and a yield eventually follows a long enough miss run, a
//   small difference in the underlying handoff cost can be amplified into a much
//   larger end-to-end throughput difference. This tool measures the end-to-end
//   result; it does not decompose it.
//
// MESSAGE CONSTRUCTION AND VALIDATION
//   The Phase-2 message types (8/32/64 bytes, trivially copyable, nothrow,
//   allocation-free, deterministic closed-form payloads) and the Phase-2
//   validation gate: exactly N delivered, every sequence number exactly the
//   expected next one, and a final checksum matching an independently
//   precomputed fold over the exact stream the producer sends. Any failure exits
//   NON-ZERO so a runner can refuse to publish the cell.
//
// USAGE
//   spsc_false_sharing_bench --impl=same_line|separated|natural
//                            --message-bytes=8|32|64 --capacity=1024|4096|65536
//                            [--messages=N] [--reps=R] [--warmup=W]
//                            [--raw-out=FILE] [--summary-out=FILE]
//
// See docs/SPSC_FALSE_SHARING.md for the methodology and the analysis.
// ---------------------------------------------------------------------------

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
#include <utility>
#include <vector>

#include "cache_line.h"
#include "mutex_bounded_queue.h"
#include "spsc_cursor_layout_ring_buffer.h"
#include "spsc_ring_buffer.h"

namespace {

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

// Same defaults as Phase 2, deliberately: the two layouts must be compared under
// the same measurement regime as the frozen baseline they are read against.
constexpr std::uint64_t kDefaultMessages = 10'000'000;
constexpr int           kDefaultReps     = 5;
constexpr int           kDefaultWarmup   = 1;

// Same consecutive-miss discipline as Phase 2 (see the policy note above).
constexpr std::uint64_t kYieldAfterMisses = 1024;

// Which queue is being timed. Natural is OBSERVATIONAL and is never part of a
// causal comparison in this experiment.
enum class Impl { SameLine, Separated, Natural };

const char* impl_name(Impl i) noexcept {
    switch (i) {
    case Impl::SameLine:
        return "same_line";
    case Impl::Separated:
        return "separated";
    case Impl::Natural:
        return "natural";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Message types — the Phase-2 types, unchanged and asserted the same way.
// ---------------------------------------------------------------------------

constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr std::uint64_t rotl7(std::uint64_t x) noexcept {
    return (x << 7) | (x >> 57);
}

template <typename... Words>
constexpr std::uint64_t fold_words(std::uint64_t h, Words... words) noexcept {
    ((h = rotl7(h) ^ static_cast<std::uint64_t>(words)), ...);
    return h;
}

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
                     t * 3ull + 1ull,
                     t * 5ull + 2ull,
                     t * 7ull + 3ull,
                     t * 11ull + 4ull};
    }

    std::uint64_t fold(std::uint64_t h) const noexcept {
        return fold_words(h, seq, price, qty, tag, w0, w1, w2, w3);
    }
};

#define LLLT_EXP3A_ASSERT_MSG(Type, Bytes)                                     \
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

LLLT_EXP3A_ASSERT_MSG(Msg8, 8);
LLLT_EXP3A_ASSERT_MSG(Msg32, 32);
LLLT_EXP3A_ASSERT_MSG(Msg64, 64);

#undef LLLT_EXP3A_ASSERT_MSG

// ---------------------------------------------------------------------------
// Layout-introspection detection. The two Phase-3A layouts expose the runtime
// cursor-address evidence; the frozen natural baseline deliberately does not
// (its header is frozen and must not be modified), so for it the layout
// evidence is simply UNAVAILABLE — which is itself recorded, because "we did not
// verify it" and "we verified it" must never look the same in the output.
// ---------------------------------------------------------------------------
template <typename Q, typename = void>
struct has_layout_report : std::false_type {};

template <typename Q>
struct has_layout_report<
    Q, std::void_t<decltype(std::declval<const Q&>().cursor_layout_report(
           std::size_t{}))>> : std::true_type {};

// ---------------------------------------------------------------------------
// Cross-variant footprint gate (Phase 3A.1).
//
// The equal-footprint requirement is asserted at compile time in the header, but
// the thing the experiment actually needs is that the payload does not move —
// and that is a property of real objects. So both policies are instantiated here
// for the SAME message type and capacity, and their measured object size and
// payload offset must agree. A cell that fails this is not publishable: it would
// change cursor placement AND payload layout at once, which is exactly the flaw
// Phase 3A.1 exists to remove.
//
// Nothing here is timed. Both probe objects are destroyed before run_cell
// allocates anything, so the measured repetitions are unaffected.
// ---------------------------------------------------------------------------
template <typename Q, typename = void>
struct has_opposite_variant : std::false_type {};

template <typename Q>
struct has_opposite_variant<
    Q, std::void_t<typename lltl::opposite_variant<Q>::type>>
    : std::true_type {};

template <typename Msg, std::size_t Capacity, typename Queue>
int verify_cross_variant_footprint(std::size_t reported_line_size) {
    if constexpr (!has_opposite_variant<Queue>::value) {
        return 0; // the frozen natural baseline is not one of the two controls
    } else {
        using Other = lltl::opposite_variant_t<Queue>;

        static_assert(sizeof(Queue) == sizeof(Other),
                      "the two Phase-3A cursor policies must produce queue "
                      "objects of identical size, or the payload offset moves "
                      "between the variants and the experiment changes two "
                      "variables at once");

        auto mine  = std::make_unique<Queue>();
        auto other = std::make_unique<Other>();

        const auto r_mine  = mine->cursor_layout_report(reported_line_size);
        const auto r_other = other->cursor_layout_report(reported_line_size);

        const bool agree = lltl::footprints_agree(r_mine, r_other);

        std::fprintf(stderr,
                     "[footprint] bytes=%zu capacity=%zu\n"
                     "  %-9s object_size=%zu payload_offset=%zu "
                     "cursor_policy_size=%zu\n"
                     "  %-9s object_size=%zu payload_offset=%zu "
                     "cursor_policy_size=%zu\n"
                     "  cross_variant_footprint=%s\n",
                     Msg::kBytes, Capacity,
                     lltl::cursor_layout_name(r_mine.layout), r_mine.object_size,
                     r_mine.payload_offset_from_object_base,
                     Queue::cursor_policy_size(),
                     lltl::cursor_layout_name(r_other.layout), r_other.object_size,
                     r_other.payload_offset_from_object_base,
                     Other::cursor_policy_size(),
                     agree ? "PASS" : "FAIL");

        if (!agree) {
            std::fprintf(stderr,
                         "\nCROSS-VARIANT FOOTPRINT INVARIANT FAILED "
                         "(bytes=%zu capacity=%zu).\n"
                         "The two cursor policies do not produce the same object "
                         "layout: object size or payload offset differs, so a "
                         "measured throughput difference could be caused by the "
                         "payload's position rather than by cursor placement. "
                         "This is a FAILED\nEXPERIMENT, not a slow cell. Exiting "
                         "non-zero without timing anything.\n",
                         Msg::kBytes, Capacity);
            return 3;
        }
        return 0;
    }
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
    std::string   raw_out;
    std::string   summary_out;
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
        "usage: %s --impl=same_line|separated|natural "
        "--message-bytes=8|32|64 --capacity=1024|4096|65536\n"
        "          [--messages=N] [--reps=R] [--warmup=W]\n"
        "          [--raw-out=FILE] [--summary-out=FILE]\n"
        "\n"
        "Experiment 02 Phase 3A — controlled cursor-placement experiment: the ONE\n"
        "variable is the cache-line placement of the two SPSC cursors, whose two\n"
        "policies have the same footprint so the payload does not move.\n"
        "ONE implementation per process: run each --impl as a separate invocation\n"
        "(scripts/spsc-false-sharing.sh does this, balanced AB/BA).\n"
        "\n"
        "  --impl           REQUIRED, exactly one of same_line|separated|natural.\n"
        "                   same_line|separated are the two Phase-3A controls and\n"
        "                   form the causal comparison. natural is the FROZEN\n"
        "                   Phase-1/2 SPSC, OBSERVATIONAL ONLY, never part of the\n"
        "                   causal conclusion.\n"
        "  --message-bytes  fixed-size nothrow message type (8, 32 or 64 bytes)\n"
        "  --capacity       queue capacity, dispatched to a compile-time "
        "specialization\n"
        "  --messages       messages to transfer per repetition (default %" PRIu64 ")\n"
        "  --reps           MEASURED repetitions per cell (default %d, all kept)\n"
        "  --warmup         warm-up repetitions per process, EXCLUDED from all "
        "reported and\n"
        "                   derived data (default %d)\n"
        "  --raw-out        write raw per-repetition CSV here (source of truth)\n"
        "  --summary-out    write the derived summary here (default stdout)\n"
        "\n"
        "The host's cache-line size is queried at runtime, and the ACTUAL cursor\n"
        "and payload addresses of every measured object are checked against it.\n"
        "If the host reports a line larger than the compile-time layout\n"
        "assumption, or if the two cursor policies do not produce the same object\n"
        "size and payload offset, this program exits non-zero WITHOUT timing\n"
        "anything rather than publishing data whose controls are unverified.\n",
        argv0, kDefaultMessages, kDefaultReps, kDefaultWarmup);
}

// ---- argument helpers (same shape as the Phase-2 tool) --------------------

bool parse_u64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char*                      end = nullptr;
    const unsigned long long   v   = std::strtoull(s, &end, 10);
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
            if (std::strcmp(v, "same_line") == 0) {
                c.impl     = Impl::SameLine;
                c.impl_set = true;
            } else if (std::strcmp(v, "separated") == 0) {
                c.impl     = Impl::Separated;
                c.impl_set = true;
            } else if (std::strcmp(v, "natural") == 0) {
                c.impl     = Impl::Natural;
                c.impl_set = true;
            } else {
                std::fprintf(stderr,
                             "--impl must be exactly one of "
                             "same_line|separated|natural (got '%s'); this "
                             "benchmark times ONE implementation per process by "
                             "design\n",
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
                     "--impl is required: exactly one of "
                     "same_line|separated|natural. Implementations are never "
                     "timed in the same process.\n");
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
    bool          ok                     = false;
    lltl::CursorLayoutReport layout{}; // zeroed when layout evidence is unavailable
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
// measured repetition, verify the cursor layout of EVERY queue object actually
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
        // ADDRESSES change between repetitions. The invariant is therefore
        // re-verified on the object that is about to be measured, not asserted
        // once from the type. This is the difference between "the layout is what
        // we claim" and "the layout happened to be what we claim in one sample".
        lltl::CursorLayoutReport layout{};
        if constexpr (has_layout_report<Queue>::value) {
            layout = q->cursor_layout_report(reported_line_size);
            std::fprintf(stderr, "[layout rep %d/%d] %s\n", rep + 1, total_reps,
                         layout.summary_line().c_str());
            if (!layout.ok()) {
                std::fprintf(stderr,
                             "\nLAYOUT INVARIANT FAILED (impl=%s bytes=%zu "
                             "cap=%zu rep=%d).\n"
                             "This is a FAILED EXPERIMENT, not a slow cell: an "
                             "unverified cursor placement cannot be published as "
                             "evidence about cursor placement. Exiting non-zero "
                             "without timing anything.\n",
                             impl_name(c.impl), c.message_bytes, Capacity, rep + 1);
                return 3;
            }
        } else {
            // The frozen natural baseline exposes no address evidence at all:
            // its header is frozen and has no cursor accessors. `layout` stays
            // zero-initialized and is never published for this impl (the raw CSV
            // and the summary write "NA" / "NOT_VERIFIED" instead), which keeps
            // "we did not verify it" visibly distinct from "we verified it".
            std::fprintf(stderr,
                         "[layout rep %d/%d] impl=%s layout_verified=NO "
                         "(frozen Phase-1 SPSC exposes no cursor addresses; "
                         "observational row only)\n",
                         rep + 1, total_reps, impl_name(c.impl));
        }

        std::atomic<bool> start{false};
        std::atomic<bool> producer_ready{false};
        std::atomic<bool> consumer_ready{false};

        std::uint64_t     producer_full_retries  = 0;
        std::uint64_t     consumer_empty_retries = 0;
        std::uint64_t     checksum               = 0;
        std::uint64_t     delivered              = 0;
        bool              sequence_ok            = true;
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
        r.ok                     = ok;
        r.layout                 = layout;

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
                         "[rep %zu/%d] impl=%s bytes=%zu cap=%zu elapsed=%" PRIu64
                         "ns ns_per_message=%.6f full_retries=%" PRIu64
                         " empty_retries=%" PRIu64 " correct=%s\n",
                         measured.size(), c.reps, impl_name(c.impl),
                         c.message_bytes, Capacity, elapsed_ns, r.ns_per_message,
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
    const double ns_median  = median_of(ns_values);
    const double mps_median = ns_median > 0.0 ? 1e9 / ns_median : 0.0;
    const double spread_pct = ns_min > 0.0 ? (ns_max / ns_min - 1.0) * 100.0 : 0.0;

    const bool layout_available = has_layout_report<Queue>::value;

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
                     "# Experiment 02 Phase 3A raw repetitions — ONE implementation "
                     "per process.\n"
                     "# ns_per_message = END-TO-END elapsed_ns / messages delivered "
                     "(NOT a per-call latency, NOT one-way handoff).\n"
                     "# The cursor addresses/line indices below were measured on the "
                     "SAME object this repetition timed,\n"
                     "# OUTSIDE the timed interval. layout_ok=PASS means the "
                     "measured placement matched the claim.\n"
                     "# object_addr/object_size/payload_offset/payload_begin_addr "
                     "are that same object's footprint evidence:\n"
                     "# the two cursor policies must agree on object_size and on "
                     "payload_offset so the payload does not\n"
                     "# move between cache sets when cursor placement changes.\n"
                     "# All %d measured repetitions are present; %d warm-up "
                     "repetition(s) are excluded from every published figure.\n"
                     "rep,impl,message_bytes,capacity,message_count,elapsed_ns,"
                     "ns_per_message,messages_per_second,producer_full_retries,"
                     "consumer_empty_retries,checksum,correctness,"
                     "reported_cache_line_size,head_addr,tail_addr,head_line,"
                     "tail_line,cursors_same_line,layout_ok,object_addr,"
                     "object_size,payload_offset,payload_begin_addr\n",
                     c.reps, c.warmup);
        for (std::size_t i = 0; i < measured.size(); ++i) {
            const RepResult& r = measured[i];
            if (layout_available) {
                std::fprintf(f,
                             "%zu,%s,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%.6f,%.3f,%" PRIu64
                             ",%" PRIu64 ",%" PRIu64 ",PASS,"
                             "%zu,%" PRIuPTR ",%" PRIuPTR ",%zu,%zu,%s,%s,"
                             "%" PRIuPTR ",%zu,%zu,%" PRIuPTR "\n",
                             i, impl_name(c.impl), c.message_bytes, Capacity, n,
                             r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                             r.producer_full_retries, r.consumer_empty_retries,
                             r.checksum, r.layout.reported_line_size,
                             static_cast<std::uintptr_t>(r.layout.head_address),
                             static_cast<std::uintptr_t>(r.layout.tail_address),
                             r.layout.head_line_index, r.layout.tail_line_index,
                             r.layout.cursors_same_line ? "yes" : "no",
                             r.layout.ok() ? "PASS" : "FAIL",
                             static_cast<std::uintptr_t>(r.layout.object_address),
                             r.layout.object_size,
                             r.layout.payload_offset_from_object_base,
                             static_cast<std::uintptr_t>(r.layout.payload_begin));
            } else {
                std::fprintf(f,
                             "%zu,%s,%zu,%zu,%" PRIu64 ",%" PRIu64 ",%.6f,%.3f,%" PRIu64
                             ",%" PRIu64 ",%" PRIu64 ",PASS,"
                             "NA,NA,NA,NA,NA,NA,NOT_VERIFIED,"
                             "NA,NA,NA,NA\n",
                             i, impl_name(c.impl), c.message_bytes, Capacity, n,
                             r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                             r.producer_full_retries, r.consumer_empty_retries,
                             r.checksum);
            }
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
                 "# Experiment 02 Phase 3A — CONTROLLED cursor-placement "
                 "(coherence-layout) experiment.\n"
                 "# The ONE variable is the cache-line placement of the two SPSC "
                 "cursors; both cursor policies have the\n"
                 "# same footprint, so the payload array starts at the same object "
                 "offset in both variants.\n"
                 "# ns_per_message = elapsed_ns / messages delivered: it INCLUDES "
                 "queue synchronization,\n"
                 "# payload assignment, cache-coherence traffic, harness "
                 "retry/backpressure and OS scheduling.\n"
                 "# It is NOT a per-try_push latency, NOT a per-try_pop latency, and "
                 "NOT a one-way handoff time.\n"
                 "# The cursors also carry REQUIRED true sharing (each side reads the "
                 "other's cursor), so a same-line vs\n"
                 "# separated difference measures cursor placement, not "
                 "'pure false-sharing cost'.\n"
                 "# ONE implementation per process: the other implementations were "
                 "NOT timed here.\n"
                 "# This summary describes THIS PROCESS ONLY. It is not a canonical\n"
                 "# result on its own; the canonical dataset pools repetitions across\n"
                 "# balanced processes. See docs/SPSC_FALSE_SHARING.md.\n"
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
                 "layout_evidence=%s\n"
                 "cache_line_size_reported=%zu\n"
                 "cache_line_size_assumed=%zu\n",
                 impl_name(c.impl), c.message_bytes, Capacity, n, measured.size(),
                 c.warmup, ns_median, ns_min, ns_max, spread_pct, mps_median,
                 full_retry_total, empty_retry_total, measured.back().checksum,
                 expected_checksum,
                 layout_available ? "VERIFIED_PER_REPETITION" : "UNAVAILABLE",
                 reported_line_size, lltl::kAssumedCacheLineSize);
    if constexpr (has_layout_report<Queue>::value) {
        std::fprintf(out,
                     "layout_invariant=%s\n"
                     "layout_first_measured_rep=%s\n"
                     "cursor_policy_size=%zu\n"
                     "object_size=%zu\n"
                     "payload_offset_from_object_base=%zu\n"
                     "cursor_layouts_measured=%s\n"
                     "# object_size and payload_offset_from_object_base must be "
                     "identical for the same-line and\n"
                     "# separated processes of this cell; the canonical runner "
                     "compares them across the pair and\n"
                     "# refuses to publish if they differ (Phase 3A.1).\n",
                     measured.front().layout.ok() ? "PASS" : "FAIL",
                     measured.front().layout.summary_line().c_str(),
                     Queue::cursor_policy_size(),
                     measured.front().layout.object_size,
                     measured.front().layout.payload_offset_from_object_base,
                     lltl::cursor_layout_name(measured.front().layout.layout));
    }
    if (!layout_available) {
        std::fprintf(out,
                     "layout_invariant=NOT_VERIFIED\n"
                     "layout_first_measured_rep=NA\n"
                     "# The frozen Phase-1 SPSC exposes no cursor addresses, so no "
                     "placement claim is made for it.\n"
                     "# This row is OBSERVATIONAL and is NOT part of the causal "
                     "same-line vs separated comparison.\n");
    }
    std::fprintf(out,
                 "# per-repetition detail — every measured repetition, none "
                 "discarded:\n"
                 "rep,elapsed_ns,ns_per_message,messages_per_second,"
                 "producer_full_retries,consumer_empty_retries,checksum,"
                 "head_line,tail_line,cursors_same_line\n");
    for (std::size_t i = 0; i < measured.size(); ++i) {
        const RepResult& r = measured[i];
        if (layout_available) {
            std::fprintf(out,
                         "%zu,%" PRIu64 ",%.6f,%.3f,%" PRIu64 ",%" PRIu64
                         ",%" PRIu64 ",%zu,%zu,%s\n",
                         i, r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                         r.producer_full_retries, r.consumer_empty_retries,
                         r.checksum, r.layout.head_line_index,
                         r.layout.tail_line_index,
                         r.layout.cursors_same_line ? "yes" : "no");
        } else {
            std::fprintf(out,
                         "%zu,%" PRIu64 ",%.6f,%.3f,%" PRIu64 ",%" PRIu64
                         ",%" PRIu64 ",NA,NA,NA\n",
                         i, r.elapsed_ns, r.ns_per_message, r.messages_per_second,
                         r.producer_full_retries, r.consumer_empty_retries,
                         r.checksum);
        }
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

// A Phase-3A control cell: the cross-variant footprint gate runs FIRST, before
// anything is timed, so a cell whose payload offset moved between the variants
// can never reach the point of producing numbers.
template <typename Msg, std::size_t Capacity, typename Queue>
int run_control_cell(const Config& c, std::size_t line) {
    const int footprint = verify_cross_variant_footprint<Msg, Capacity, Queue>(line);
    if (footprint != 0) {
        return footprint;
    }
    return run_cell<Msg, Capacity, Queue>(c, line);
}

template <typename Msg, std::size_t Capacity>
int dispatch_impl(const Config& c, std::size_t line) {
    switch (c.impl) {
    case Impl::SameLine:
        return run_control_cell<Msg, Capacity,
                                lltl::SpscSameLineRingBuffer<Msg, Capacity>>(
            c, line);
    case Impl::Separated:
        return run_control_cell<
            Msg, Capacity,
            lltl::SpscSeparatedCursorRingBuffer<Msg, Capacity>>(c, line);
    case Impl::Natural:
        return run_cell<Msg, Capacity, lltl::SpscRingBuffer<Msg, Capacity>>(c,
                                                                            line);
    }
    return 2; // unreachable: the parser accepts exactly three values
}

template <typename Msg>
int dispatch_capacity(const Config& c, std::size_t line) {
    switch (c.capacity) {
    case 1024:
        return dispatch_impl<Msg, 1024>(c, line);
    case 4096:
        return dispatch_impl<Msg, 4096>(c, line);
    case 65536:
        return dispatch_impl<Msg, 65536>(c, line);
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
    // longer guarantee that the separated control's cursor blocks land in
    // distinct real blocks — and a control that is not what it claims would make
    // the experiment report the opposite of the truth. There is no safe way to
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
                     "Phase 3A changes ONLY cursor cache-line placement, so a host "
                     "whose real line size exceeds this\n"
                     "assumption cannot be relied on to keep the same-line and "
                     "separated controls distinct.\nRefusing to measure; exiting "
                     "non-zero rather than publishing data whose layout is "
                     "unverified.\n",
                     reported_line, lltl::kAssumedCacheLineSize);
        return 3;
    }

    return dispatch_message(c, reported_line);
}
