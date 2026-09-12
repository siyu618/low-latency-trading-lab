#pragma once

// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3B.
//
// RemoteCursorRingBuffer: the SAME single-producer / single-consumer ring
// buffer as the frozen Phase-1 SpscRingBuffer and the Phase-3A separated
// control, with exactly ONE intended algorithmic treatment made controllable —
// REMOTE-CURSOR CACHING: whether each thread consults a thread-owned cached copy
// of the opposite cursor and refreshes it only when the cached value is
// insufficient to prove progress is safe. Reduced remote-load frequency is the
// primary mechanism of that treatment; the treatment also carries its own local
// fast-path bookkeeping (a cached-value read, a comparison and a branch).
//
// ---------------------------------------------------------------------------
// WHY THIS IS A SEPARATE TYPE
// ---------------------------------------------------------------------------
// Phase 3A already established a controlled SEPARATED cursor layout and verified
// it at runtime. Phase 3B must not disturb that: cursor placement stays the
// verified separated layout, and the payload keeps the same offset. The one new
// treatment is whether a thread consults a thread-owned cached copy of the
// opposite cursor and refreshes it only when the cached value says the queue MAY
// be full (producer) or MAY be empty (consumer).
//
// The claim "the two Phase-3B variants differ in one intended algorithmic
// treatment — remote-cursor caching" is enforced BY CONSTRUCTION rather than by
// discipline, exactly as Phase 3A did. It is a claim about the ALGORITHM AND
// OBJECT LAYOUT, not about the emitted instruction stream: the two variants are
// distinct template instantiations and necessarily have different machine code
// (see "WHAT THE MEASURED DIFFERENCE DOES AND DOES NOT ISOLATE" below).
//
//   * There is ONE algorithm body, below, written once. `try_push` and
//     `try_pop` are a single function each, with the cached/uncached decision
//     made by `if constexpr` on a template parameter — not by a second,
//     copy-pasted implementation that can drift.
//   * BOTH variants use the SAME cursor policy type, SeparatedRemoteCursorBlock.
//     So "identical object footprint, identical payload offset, identical cursor
//     cache-line placement" is not an assertion about two similar types: it is
//     the same type, and it cannot differ.
//   * The memory orders are unchanged from the Phase-1/3A baseline: relaxed load
//     of the caller's OWN cursor, acquire load of the REMOTE cursor, release
//     store to publish. Phase 3B changes HOW OFTEN the acquire load happens. It
//     does not change what the acquire load guarantees, and it removes nothing.
//   * The frozen Phase-1 SpscRingBuffer and the Phase-3A
//     CursorLayoutRingBuffer are NOT touched and NOT subclassed. Phase 3A's file
//     (include/spsc_cursor_layout_ring_buffer.h) is included here READ-ONLY, for
//     its CursorLayoutReport evidence type.
//
// ---------------------------------------------------------------------------
// WHAT PHASE 3B DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// Remote-cursor caching is the ONLY intended treatment. The following are all
// separate, later, controlled experiments and are intentionally absent here:
//
//   * NO change to cursor placement. Both variants use the SEPARATED layout,
//     verified at runtime exactly as in Phase 3A. The Phase-3A same-line control
//     is not part of this comparison at all — combining the two treatments would
//     make neither attributable.
//   * NO batching, NO bulk transfer, NO change to how many messages one call
//     moves.
//   * NO memory-order changes of any kind, in either direction. Not even a
//     "stronger is safer" upgrade, and not a relaxation of either release store.
//   * NO compare_exchange / fetch_add / read-modify-write. Each cursor still has
//     exactly one writer, so none is needed.
//   * NO mutex, NO condition variable, NO CAS, NO MPSC/MPMC, NO CPU affinity or
//     pinning, NO NUMA tuning, NO tail-latency instrumentation.
//
// ---------------------------------------------------------------------------
// WHAT THE MEASURED DIFFERENCE DOES AND DOES NOT ISOLATE
// ---------------------------------------------------------------------------
// The cached variant performs FEWER acquire loads of the remote cursor. That is
// the mechanism. Whether fewer remote observations produce a wall-clock
// difference is an empirical question this header does not answer — and the
// answer is not guaranteed, because:
//
//   * the remaining remote loads are the ones that were on the critical path
//     anyway (the refresh happens precisely when progress is blocked), while the
//     elided ones may have been served from a cache line that was already in the
//     local coherence state;
//   * the cached variant adds a thread-local read-modify-write of its own cached
//     value on the fast path, plus a branch, which is not free;
//   * at macOS/Apple-Silicon scheduling granularity, a small coherence-traffic
//     difference may be swamped by scheduler placement, DVFS and thermal noise.
//
// A cell where remote loads fall dramatically and throughput does not improve is
// therefore a REAL possible outcome, not a contradiction. Both are measured
// separately and reported separately.
//
// Because the treatment carries that local bookkeeping, a measured throughput
// difference must NOT be described as isolating the cost of one remote atomic
// load. What the treatment bundles — fewer remote loads, plus a cached-value
// read and a branch — is what the difference is between.
//
// ---------------------------------------------------------------------------
// WHY A STALE CACHED REMOTE CURSOR IS SAFE
// ---------------------------------------------------------------------------
// The cached copies are NOT atomic and are NOT shared: `cached_tail` is written
// and read only by the producer, `cached_head` only by the consumer. They are
// ordinary thread-owned members. No synchronization exists for them, and none is
// needed, because each cached copy only ever holds a value the real cursor had
// at some EARLIER moment. A cached value can therefore be stale, but it is never
// speculative: it cannot name a cursor position the remote thread has not
// actually reached.
//
// "Earlier" must be stated in MODULAR DISTANCE, not in ordinary numeric
// ordering. The cursors are unsigned counters that wrap, so `cached_tail <=
// tail` is NOT a valid way to say "cached_tail is behind tail": at
// cached_tail = SIZE_MAX - 3 and tail = 2 the cached value IS behind, and the
// ordinary comparison says the opposite. The quantities that are actually
// meaningful are differences, and they are what the code compares.
//
//   * PRODUCER. Let `h` be the producer's own head and write every difference
//     modulo 2^width(std::size_t):
//
//         real_occupancy   = h - real_tail
//         cached_occupancy = h - cached_tail
//         remote_progress  = real_tail - cached_tail
//
//     `tail` only ever increases, so cached_tail is a past value of tail and
//     remote_progress is a small non-negative distance. Substituting:
//
//         cached_occupancy = real_occupancy + remote_progress     (exactly)
//
//     The fast path proceeds only when cached_occupancy < Capacity. With
//     remote_progress >= 0 that gives real_occupancy <= cached_occupancy <
//     Capacity: the queue really does have room. A stale cached_tail can only
//     make the producer UNDER-count the slots the consumer has released, i.e.
//     report FALSE FULL and pay an extra refresh — never overwrite a slot the
//     consumer has not released.
//   * CONSUMER. Let `t` be the consumer's own tail. The same construction gives
//     cached_head - t <= real_head - t, and the fast path proceeds only when
//     cached_head - t != 0. A non-zero modular distance means at least one
//     message was published at or before the cached observation, and `head` only
//     ever increases, so that message is still available: real_head - t != 0 as
//     well. A stale cached_head can only report FALSE EMPTY — never read a
//     payload that was never published.
//
// Both steps are exact only because the differences above stay small: `head` and
// `tail` never differ by more than Capacity (Phase 1's constraint, unchanged),
// and a cached value lags its cursor by far less than 2^63, so no difference
// aliases into a large wrapped-around value. The counter-wrap tests exercise
// this through the ordinary API across the boundary rather than assuming it; see
// the TEST-ONLY seed constructor below.
//
// Correctness still rests on the acquire loads, unchanged:
//   * the producer's refresh `cached_tail = tail_.load(acquire)` is what orders
//     the payload write after the consumer's finished read of the slot being
//     recycled;
//   * the consumer's refresh `cached_head = head_.load(acquire)` is what orders
//     the payload read after the producer's publication of it.
// Phase 3B reduces the FREQUENCY of those loads. It does not replace them, and
// it removes no release/acquire pair from the protocol. See
// docs/SPSC_REMOTE_CURSOR_CACHE.md for the full argument, including the two
// induction steps that make the abbreviated check exact.
//
// ---------------------------------------------------------------------------
// WHY THE CACHED VALUES SIT WHERE THEY DO
// ---------------------------------------------------------------------------
// A cached copy must not introduce a NEW cross-thread false-sharing relationship
// on top of the layout Phase 3A already controlled. So each cached value is
// stored with the state its OWNING thread already owns:
//
//   line 0  | head (producer writes, consumer reads) | cached_tail (producer only)
//   line 1  | tail (consumer writes, producer reads) | cached_head (consumer only)
//   ------  | payload
//
// Line 0 is already producer-written and consumer-read; adding producer-private
// storage to it creates no new sharing. Line 1 is already consumer-written and
// producer-read; adding consumer-private storage to it likewise creates none.
// Neither cached value is ever touched by the thread that does not own it, and
// the runtime layout report below verifies exactly that on the object that ran.
//
// The instrumentation counters live in the same two lines, under the same
// ownership rule: each is written only by the thread whose line it is.
// ---------------------------------------------------------------------------

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "cache_line.h"
#include "spsc_cursor_layout_ring_buffer.h"

namespace lltl {

// Which remote-cursor treatment a Phase-3B instantiation is. Compile-time only;
// it selects one branch of the single algorithm body below.
enum class RemoteCursorMode { Direct, Cached };

// The dataset's `impl` names. `Direct` is the baseline: every try_push/try_pop
// loads the remote cursor exactly as the Phase-3A separated control does.
inline const char* remote_cursor_mode_name(RemoteCursorMode m) noexcept {
    return m == RemoteCursorMode::Direct ? "baseline" : "cached";
}

// ---------------------------------------------------------------------------
// SeparatedRemoteCursorBlock — the ONE Phase-3B cursor policy, used by BOTH
// variants, so the two cannot differ in object layout at all.
//
// The layout is Phase 3A's verified SEPARATED layout (head and tail in distinct
// assumed interference blocks, exactly 2 * kAssumedCacheLineSize bytes total),
// with the cached remote cursors and the instrumentation counters added to the
// block of the thread that owns them.
//
// Ownership (enforced by the algorithm, verified at runtime):
//   head                        producer writes, consumer reads   (atomic)
//   cached_tail                 producer ONLY                     (plain)
//   producer_remote_tail_loads  producer ONLY                     (plain)
//   tail                        consumer writes, producer reads   (atomic)
//   cached_head                 consumer ONLY                     (plain)
//   consumer_remote_head_loads  consumer ONLY                     (plain)
//
// The counters are ordinary non-atomic members on purpose. They are thread-owned
// state, incremented only by their owner and read by the harness only after the
// owning thread has been joined. Putting a shared atomic counter in the timed
// hot path would add exactly the cross-thread traffic the experiment is trying
// to vary, i.e. it would make the instrument the experiment.
//
// `head` and `tail` keep the offsets Phase 3A verified (0 and
// kAssumedCacheLineSize), so nothing about the placement Phase 3A established
// moves.
// ---------------------------------------------------------------------------
struct alignas(kAssumedCacheLineSize) SeparatedRemoteCursorBlock {
    static constexpr CursorLayout kLayout = CursorLayout::Separated;

    // ---- producer-owned assumed interference block (line 0) ---------------
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> head{0}; // offset 0
    std::size_t   cached_tail{0};                 // offset 8  — producer only
    std::uint64_t producer_remote_tail_loads{0};  // offset 16 — producer only

    // ---- consumer-owned assumed interference block (line 1) ---------------
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> tail{0}; // offset 128
    std::size_t   cached_head{0};                 // offset 136 — consumer only
    std::uint64_t consumer_remote_head_loads{0};  // offset 144 — consumer only
};

// ---------------------------------------------------------------------------
// Compile-time layout guarantees. If a future edit (or an ABI whose atomic size
// differs) broke any of these, the build fails here rather than the experiment
// silently measuring an uncontrolled layout.
// ---------------------------------------------------------------------------
static_assert(sizeof(SeparatedRemoteCursorBlock) == 2 * kAssumedCacheLineSize,
              "the Phase-3B cursor policy must occupy exactly two assumed "
              "interference blocks, one per cursor, with the cached remote "
              "cursor and the instrumentation counter of each thread stored "
              "beside the cursor that thread already owns");
static_assert(alignof(SeparatedRemoteCursorBlock) >= kAssumedCacheLineSize);

// Each cursor in its OWN assumed block, at the offsets Phase 3A verified.
static_assert(offsetof(SeparatedRemoteCursorBlock, head) == 0);
static_assert(offsetof(SeparatedRemoteCursorBlock, tail) >=
              kAssumedCacheLineSize);
static_assert(offsetof(SeparatedRemoteCursorBlock, tail) <
              2 * kAssumedCacheLineSize);

// Thread-owned state must sit on its OWNER's line. Moving a cached value into
// the other thread's block would create precisely the new cross-thread sharing
// relationship Phase 3B must not add.
static_assert(offsetof(SeparatedRemoteCursorBlock, cached_tail) <
                  kAssumedCacheLineSize,
              "cached_tail is producer-owned and must live in the producer's "
              "assumed interference block, with head");
static_assert(offsetof(SeparatedRemoteCursorBlock, cached_head) >=
                  kAssumedCacheLineSize,
              "cached_head is consumer-owned and must live in the consumer's "
              "assumed interference block, with tail");
static_assert(offsetof(SeparatedRemoteCursorBlock, producer_remote_tail_loads) <
              kAssumedCacheLineSize);
static_assert(offsetof(SeparatedRemoteCursorBlock, consumer_remote_head_loads) >=
              kAssumedCacheLineSize);

// Equal footprint with the Phase-3A separated policy, so the payload keeps the
// offset the Phase-3A layout verification established: Phase 3B measures a
// different treatment, not a different object layout.
static_assert(sizeof(SeparatedRemoteCursorBlock) == sizeof(SeparatedCursorBlocks),
              "the Phase-3B cursor policy must have the same footprint as the "
              "Phase-3A separated policy, so the payload array that follows it "
              "starts at the same object offset; otherwise Phase 3B would "
              "change object layout as well as remote-load frequency");
static_assert(alignof(SeparatedRemoteCursorBlock) ==
              alignof(SeparatedCursorBlocks));

// ---------------------------------------------------------------------------
// build_cursor_layout_report — assemble the Phase-3A CursorLayoutReport from
// raw addresses.
//
// CursorLayoutRingBuffer computes this inline for its own objects. Phase 3B is a
// different type and cannot reuse that member function, so the assembly is
// written here as a free function over the SAME primitives from cache_line.h.
// The two implementations are required to agree: the Phase-3B test suite builds
// a report both ways for the same object and fails if any field differs, so the
// duplication cannot drift unnoticed.
// ---------------------------------------------------------------------------
inline CursorLayoutReport build_cursor_layout_report(
    CursorLayout layout, std::size_t reported_line_size,
    std::uintptr_t head_address, std::uintptr_t tail_address,
    std::uintptr_t payload_begin, std::uintptr_t payload_end,
    std::uintptr_t object_address, std::size_t object_size) noexcept {
    CursorLayoutReport report;
    report.layout             = layout;
    report.reported_line_size = reported_line_size;
    report.layout_alignment   = kAssumedCacheLineSize;
    report.head_address       = head_address;
    report.tail_address       = tail_address;
    report.payload_begin      = payload_begin;
    report.payload_end        = payload_end;
    report.object_address     = object_address;
    report.object_size        = object_size;
    report.payload_offset_from_object_base = payload_begin - object_address;

    report.head_line_index = cache_line_index(head_address, reported_line_size);
    report.tail_line_index = cache_line_index(tail_address, reported_line_size);

    report.cursors_same_line =
        same_cache_line(head_address, tail_address, reported_line_size);

    report.line_size_supported = line_size_supported(reported_line_size);

    const bool expected_same_line = (layout == CursorLayout::SameLine);
    report.same_line_invariant_holds =
        (report.cursors_same_line == expected_same_line);

    const auto block_overlaps_payload = [&](std::uintptr_t address) {
        if (reported_line_size == 0) {
            return false;
        }
        const std::uintptr_t block_start =
            static_cast<std::uintptr_t>(
                cache_line_index(address, reported_line_size)) *
            reported_line_size;
        const std::uintptr_t block_end = block_start + reported_line_size;
        return block_start < payload_end && payload_begin < block_end;
    };

    report.cursors_share_payload_line = block_overlaps_payload(head_address) ||
                                        block_overlaps_payload(tail_address);
    report.payload_disjoint = !report.cursors_share_payload_line;

    return report;
}

// ---------------------------------------------------------------------------
// RemoteCursorLayoutReport — the Phase-3A layout evidence for a Phase-3B object,
// PLUS the two checks Phase 3B adds to its own control.
//
// The Phase-3A evidence is embedded unchanged: the separated invariant
// (head_line != tail_line), the line-size support guard, and cursor/payload line
// disjointness. Phase 3B adds:
//
//   * each cached remote cursor sits on its OWNER's interference block, so it
//     shares a line with the cursor that thread already owns and with nothing
//     the other thread writes; and
//   * neither cached value shares a line with the REMOTE cursor, which is the
//     statement that no new cross-thread sharing relationship was introduced.
//
// `ok()` is the conjunction the experiment requires before a cell may be
// published. A cell whose report is not ok() must FAIL rather than publish: an
// unverified layout is not evidence about layout.
// ---------------------------------------------------------------------------
struct RemoteCursorLayoutReport {
    CursorLayoutReport cursor{}; // Phase-3A evidence, reused unchanged

    std::uintptr_t cached_tail_address{0};
    std::uintptr_t cached_head_address{0};
    std::size_t    cached_tail_line_index{0};
    std::size_t    cached_head_line_index{0};

    bool cached_tail_colocated_with_owner{false};
    bool cached_head_colocated_with_owner{false};
    bool cached_state_disjoint_from_remote_cursor{false};

    // Convenience passthroughs, so a caller does not have to reach through
    // `.cursor` for the fields the dataset records.
    CursorLayout layout() const noexcept { return cursor.layout; }
    std::size_t  reported_line_size() const noexcept {
        return cursor.reported_line_size;
    }
    std::uintptr_t head_address() const noexcept { return cursor.head_address; }
    std::uintptr_t tail_address() const noexcept { return cursor.tail_address; }
    std::size_t  head_line_index() const noexcept {
        return cursor.head_line_index;
    }
    std::size_t  tail_line_index() const noexcept {
        return cursor.tail_line_index;
    }
    std::uintptr_t object_address() const noexcept {
        return cursor.object_address;
    }
    std::size_t  object_size() const noexcept { return cursor.object_size; }
    std::size_t  payload_offset_from_object_base() const noexcept {
        return cursor.payload_offset_from_object_base;
    }
    std::uintptr_t payload_begin() const noexcept { return cursor.payload_begin; }

    bool ok() const noexcept {
        return cursor.ok() && cached_tail_colocated_with_owner &&
               cached_head_colocated_with_owner &&
               cached_state_disjoint_from_remote_cursor;
    }

    // One-line, greppable form. Reports MEASURED addresses and indices.
    std::string summary_line() const {
        std::string s = cursor.summary_line();
        s += " cached_tail_addr=0x";
        s += hex(cached_tail_address);
        s += " cached_head_addr=0x";
        s += hex(cached_head_address);
        s += " cached_tail_line=" + std::to_string(cached_tail_line_index);
        s += " cached_head_line=" + std::to_string(cached_head_line_index);
        s += " cached_placement=";
        s += ok_cached() ? "PASS" : "FAIL";
        return s;
    }

    // The Phase-3B half of ok(), reported separately so the two claims never
    // look like one.
    bool ok_cached() const noexcept {
        return cached_tail_colocated_with_owner &&
               cached_head_colocated_with_owner &&
               cached_state_disjoint_from_remote_cursor;
    }

private:
    static std::string hex(std::uintptr_t value) {
        static const char* digits = "0123456789abcdef";
        std::string        out;
        if (value == 0) {
            return "0";
        }
        while (value != 0) {
            out.insert(out.begin(), digits[value & 0xF]);
            value >>= 4;
        }
        return out;
    }
};

// Cross-variant footprint agreement, delegating to the Phase-3A definition: the
// objects are the same size and the payload starts at the same offset from the
// object base. Under Phase 3B both variants instantiate the SAME cursor policy,
// so this holds by construction; it is still checked at runtime on real objects
// so that the claim is verified rather than assumed.
inline bool remote_footprints_agree(const RemoteCursorLayoutReport& a,
                                    const RemoteCursorLayoutReport& b) noexcept {
    return footprints_agree(a.cursor, b.cursor);
}

// ---------------------------------------------------------------------------
// RemoteCursorRingBuffer — the single Phase-3B algorithm body.
//
// `Mode` selects the treatment; `Instrumented` selects whether the thread-owned
// remote-load counters are incremented. Both are compile-time, so the canonical
// (uninstrumented) build carries no counting work at all: the counters exist as
// storage — keeping the object layout identical across modes — but no increment
// is emitted.
//
// The counter storage is present in EVERY configuration on purpose. Making it
// conditional would give the instrumented and uninstrumented builds different
// object layouts, and the instrumented run is supposed to differ from the
// canonical one only in the counting work, not in where anything lives.
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity, RemoteCursorMode Mode,
          bool Instrumented = false>
class RemoteCursorRingBuffer {
    static_assert(Capacity > 0, "RemoteCursorRingBuffer requires Capacity > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "RemoteCursorRingBuffer requires a power-of-two Capacity so "
                  "that slot = counter & (Capacity - 1) needs no modulo");
    static_assert(std::is_default_constructible_v<T>,
                  "Phase 3B stores T in a preallocated std::array; T must be "
                  "default-constructible");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "Experiment 02 requires a platform where "
                  "std::atomic<std::size_t> is always lock-free: the SPSC "
                  "cursor protocol is only lock-free where the cursor atomic "
                  "is. No mutex fallback is provided by design.");

public:
    using cursor_policy = SeparatedRemoteCursorBlock;

    // Which treatment this instantiation is, and whether it counts.
    static constexpr RemoteCursorMode mode() noexcept { return Mode; }
    static constexpr bool instrumented() noexcept { return Instrumented; }
    static constexpr CursorLayout layout() noexcept {
        return CursorLayout::Separated;
    }

    static constexpr std::size_t cursor_policy_size() noexcept {
        return sizeof(SeparatedRemoteCursorBlock);
    }

    static constexpr std::size_t payload_alignment() noexcept {
        return alignof(std::array<T, Capacity>) > kAssumedCacheLineSize
                   ? alignof(std::array<T, Capacity>)
                   : kAssumedCacheLineSize;
    }

    RemoteCursorRingBuffer() = default;

    // -----------------------------------------------------------------------
    // TEST-ONLY seed constructor.
    //
    // The cursors are monotonic unsigned counters that are allowed to wrap
    // modulo 2^width(std::size_t). Exercising that boundary directly would need
    // 2^64 operations, which is not a test anyone can run, so this constructor
    // starts the queue at a chosen cursor value instead. It exists so the test
    // suite can place the counters a few steps below the wrap boundary and then
    // push/pop ACROSS it through the ordinary API.
    //
    // It is deliberately restrictive: the queue must be EMPTY (head == tail),
    // because a non-empty seeded queue would claim that slots hold real
    // messages when they hold default-constructed ones. The cached remote
    // cursors are seeded to the same value as the cursors they cache, which is
    // the state a freshly constructed queue at that counter value would be in.
    //
    // The frozen Phase-1 and Phase-3A types have no equivalent hook, and this
    // type has no API to move a counter backwards afterwards.
    // -----------------------------------------------------------------------
    struct SeedEmptyAtCounterForTest {
        std::size_t counter;
    };

    explicit RemoteCursorRingBuffer(SeedEmptyAtCounterForTest seed) noexcept {
        cursors_.head.store(seed.counter, std::memory_order_relaxed);
        cursors_.tail.store(seed.counter, std::memory_order_relaxed);
        cursors_.cached_tail = seed.counter;
        cursors_.cached_head = seed.counter;
    }

    RemoteCursorRingBuffer(const RemoteCursorRingBuffer&)            = delete;
    RemoteCursorRingBuffer& operator=(const RemoteCursorRingBuffer&) = delete;
    RemoteCursorRingBuffer(RemoteCursorRingBuffer&&)                 = delete;
    RemoteCursorRingBuffer& operator=(RemoteCursorRingBuffer&&)      = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // --- Producer side (call ONLY from the single producer thread) ---------
    //
    // The only algorithmic difference between the two modes is the first block
    // below: whether the cached value is consulted before the remote cursor is
    // read. Everything after it is byte-for-byte the same source.
    //
    // Direct: one acquire load of the remote `tail` per call, exactly as the
    // Phase-3A separated control does.
    //
    // Cached: the thread-owned `cached_tail` is consulted first. The modular
    // distance `h - cached_tail` is `cached_occupancy`, and the bounded-distance
    // invariant keeps it <= Capacity, so the test below is `cached_occupancy <
    // Capacity` — which proves the queue cannot be full and skips the remote
    // load. Otherwise the cached value is refreshed with exactly the same
    // acquire load the baseline performs, and the test is repeated. A stale
    // cached value can therefore only cause an extra refresh and a FALSE FULL,
    // never a slot reuse the consumer has not released.
    //
    // Both modes then perform the identical payload write and the identical
    // release store. The release store is what publishes the payload and is
    // never weakened.
    bool try_push(const T& v) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t h = cursors_.head.load(std::memory_order_relaxed);
        if constexpr (Mode == RemoteCursorMode::Cached) {
            if (h - cursors_.cached_tail == Capacity) {
                refresh_cached_tail_();
                if (h - cursors_.cached_tail == Capacity) {
                    return false; // full
                }
            }
        } else {
            if constexpr (Instrumented) {
                ++cursors_.producer_remote_tail_loads;
            }
            if (h - cursors_.tail.load(std::memory_order_acquire) == Capacity) {
                return false; // full
            }
        }
        slots_[h & (Capacity - 1)] = v;                        // payload write
        cursors_.head.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    bool try_push(T&& v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t h = cursors_.head.load(std::memory_order_relaxed);
        if constexpr (Mode == RemoteCursorMode::Cached) {
            if (h - cursors_.cached_tail == Capacity) {
                refresh_cached_tail_();
                if (h - cursors_.cached_tail == Capacity) {
                    return false; // full
                }
            }
        } else {
            if constexpr (Instrumented) {
                ++cursors_.producer_remote_tail_loads;
            }
            if (h - cursors_.tail.load(std::memory_order_acquire) == Capacity) {
                return false; // full
            }
        }
        slots_[h & (Capacity - 1)] = std::move(v);             // payload write
        cursors_.head.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    // --- Consumer side (call ONLY from the single consumer thread) ---------
    //
    // Dual to the producer. The modular distance `cached_head - t` is the
    // number of messages that were available as of the cached observation, so
    // `cached_head != t` means that distance is non-zero and data is available
    // (the invariant that cached_head lies chronologically between t and the
    // real head, never outside them, is what the refresh below maintains) and
    // the remote load is skipped. Otherwise the cached value is refreshed with
    // the same acquire load the baseline performs. A stale cached value can
    // cause a FALSE EMPTY, never a read of a payload that has not been
    // published.
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t t = cursors_.tail.load(std::memory_order_relaxed);
        if constexpr (Mode == RemoteCursorMode::Cached) {
            if (cursors_.cached_head == t) {
                refresh_cached_head_();
                if (cursors_.cached_head == t) {
                    return false; // empty
                }
            }
        } else {
            if constexpr (Instrumented) {
                ++cursors_.consumer_remote_head_loads;
            }
            if (cursors_.head.load(std::memory_order_acquire) == t) {
                return false; // empty
            }
        }
        out = std::move(slots_[t & (Capacity - 1)]);           // payload read
        cursors_.tail.store(t + 1, std::memory_order_release); // release slot
        return true;
    }

    // --- Observers ---------------------------------------------------------
    // ADVISORY only, exactly as in the frozen Phase-1 type and in Phase 3A, and
    // IDENTICAL in both modes: the treatment does not touch this path, so a
    // difference here could not be attributed to remote-load frequency.
    bool empty() const noexcept {
        return cursors_.head.load(std::memory_order_relaxed) ==
               cursors_.tail.load(std::memory_order_relaxed);
    }

    // --- Instrumentation (thread-owned; read only after joining the owner) --
    //
    // These counters are ordinary non-atomic members written by exactly one
    // thread each. Reading them from the harness is safe because the harness
    // does so only after joining that thread, which is a synchronization point.
    // They are NOT safe to read concurrently with a running producer/consumer,
    // and they are NOT a synchronization mechanism.
    //
    // In an uninstrumented build no increment is emitted, so both counters stay
    // zero; `instrumented()` says which configuration produced a number, so "we
    // did not count" and "we counted zero" can never look the same.
    std::uint64_t producer_remote_tail_loads() const noexcept {
        return cursors_.producer_remote_tail_loads;
    }
    std::uint64_t consumer_remote_head_loads() const noexcept {
        return cursors_.consumer_remote_head_loads;
    }

    // --- Layout introspection (NOT part of the transfer path) --------------

    std::uintptr_t cursor_head_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(&cursors_.head);
    }
    std::uintptr_t cursor_tail_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(&cursors_.tail);
    }
    std::uintptr_t cached_tail_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(&cursors_.cached_tail);
    }
    std::uintptr_t cached_head_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(&cursors_.cached_head);
    }
    std::uintptr_t payload_begin_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(slots_.data());
    }
    std::uintptr_t payload_end_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(slots_.data() + Capacity);
    }
    std::uintptr_t object_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(this);
    }
    std::size_t object_size() const noexcept {
        return sizeof(RemoteCursorRingBuffer);
    }

    RemoteCursorLayoutReport cursor_layout_report(
        std::size_t reported_line_size) const noexcept {
        RemoteCursorLayoutReport report;
        report.cursor = build_cursor_layout_report(
            CursorLayout::Separated, reported_line_size, cursor_head_address(),
            cursor_tail_address(), payload_begin_address(),
            payload_end_address(), object_address(), object_size());

        report.cached_tail_address = cached_tail_address();
        report.cached_head_address = cached_head_address();
        report.cached_tail_line_index =
            cache_line_index(report.cached_tail_address, reported_line_size);
        report.cached_head_line_index =
            cache_line_index(report.cached_head_address, reported_line_size);

        // Each cached value must sit on its OWNER's line: with the cursor that
        // thread already owns, and NOT with the remote cursor.
        report.cached_tail_colocated_with_owner =
            same_cache_line(report.cached_tail_address,
                            report.cursor.head_address, reported_line_size);
        report.cached_head_colocated_with_owner =
            same_cache_line(report.cached_head_address,
                            report.cursor.tail_address, reported_line_size);

        report.cached_state_disjoint_from_remote_cursor =
            !same_cache_line(report.cached_tail_address,
                             report.cursor.tail_address, reported_line_size) &&
            !same_cache_line(report.cached_head_address,
                             report.cursor.head_address, reported_line_size);

        return report;
    }

private:
    // The refresh. Exactly the acquire load the baseline performs on every call,
    // performed here only when the cached value says it is needed. The acquire
    // is what orders this thread's payload access against the remote thread's
    // publication/finished-read; it is never relaxed and never removed.
    void refresh_cached_tail_() noexcept {
        if constexpr (Instrumented) {
            ++cursors_.producer_remote_tail_loads;
        }
        cursors_.cached_tail = cursors_.tail.load(std::memory_order_acquire);
    }

    void refresh_cached_head_() noexcept {
        if constexpr (Instrumented) {
            ++cursors_.consumer_remote_head_loads;
        }
        cursors_.cached_head = cursors_.head.load(std::memory_order_acquire);
    }

    // Layout note — the SAME member order and payload alignment as Phase 3A, so
    // the payload lands at the same offset the Phase-3A layout verification
    // established. Nothing here is a second controlled variable.
    SeparatedRemoteCursorBlock                             cursors_{};
    alignas(kAssumedCacheLineSize) std::array<T, Capacity> slots_{};
};

// The Phase-3B controls. Named types rather than a flag, so the distinction is
// visible at every use site and cannot be defaulted away — and so that reading a
// benchmark invocation tells you which treatment ran and whether it counted.
template <typename T, std::size_t Capacity>
using SpscSeparatedBaselineRingBuffer =
    RemoteCursorRingBuffer<T, Capacity, RemoteCursorMode::Direct, false>;

template <typename T, std::size_t Capacity>
using SpscSeparatedCachedCursorRingBuffer =
    RemoteCursorRingBuffer<T, Capacity, RemoteCursorMode::Cached, false>;

template <typename T, std::size_t Capacity>
using SpscSeparatedBaselineInstrumented =
    RemoteCursorRingBuffer<T, Capacity, RemoteCursorMode::Direct, true>;

template <typename T, std::size_t Capacity>
using SpscSeparatedCachedInstrumented =
    RemoteCursorRingBuffer<T, Capacity, RemoteCursorMode::Cached, true>;

} // namespace lltl
