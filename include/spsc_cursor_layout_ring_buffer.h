#pragma once

// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3A.
//
// CursorLayoutRingBuffer: the SAME single-producer / single-consumer ring
// buffer as the frozen Phase-1 SpscRingBuffer, with exactly ONE thing made
// controllable — the cache-line placement of the two cursors.
//
// ---------------------------------------------------------------------------
// WHY THIS IS A SEPARATE TYPE AND NOT A SUBCLASS OR A TEMPLATE FLAG ON THE
// FROZEN ONE
// ---------------------------------------------------------------------------
// Phase 3A is a controlled experiment whose whole validity rests on the claim
// "the only difference between the two variants is cursor cache-line
// placement". That claim is easy to state and easy to break: a copy-pasted
// second implementation drifts, and the drift is invisible because both copies
// still compile and both still pass their correctness tests.
//
// So the claim is enforced BY CONSTRUCTION rather than by discipline:
//
//   * There is ONE algorithm body, below, written once. It is a byte-for-byte
//     semantic copy of the frozen Phase-1 body: identical full/empty tests,
//     identical slot indexing, identical memory orders (relaxed own-cursor
//     load, acquire remote-cursor load, release publish), identical
//     try_push(copy) / try_push(move) / try_pop / empty surface, identical
//     noexcept specifications and identical static_asserts.
//   * The ONLY template parameter that differs between the two variants is
//     CursorLayoutPolicy, which contributes storage and NOTHING else. It has no
//     functions the algorithm calls, so it cannot alter control flow even by
//     accident.
//   * The frozen SpscRingBuffer is NOT touched, NOT subclassed, and NOT
//     modified. It remains the historical natural/unpadded Phase-2 baseline.
//
// The two variants are the named aliases at the bottom of this header:
//
//   SpscSameLineRingBuffer        — both cursors in ONE interference block
//   SpscSeparatedCursorRingBuffer — cursors in DISTINCT interference blocks
//
// ---------------------------------------------------------------------------
// WHAT PHASE 3A DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// Cursor layout is the ONLY variable. The following are all separate,
// later, controlled experiments and are intentionally absent here:
//
//   * NO cached remote cursor (no cached_head_ / cached_tail_). Reducing the
//     NUMBER of remote cursor observations is Phase 3B and must never be
//     combined with the layout change, or neither effect is attributable.
//   * NO batching, NO bulk transfer, NO change to how many messages a call
//     moves.
//   * NO memory-order changes of any kind, in either direction. Not even a
//     "stronger is safer" upgrade: a fence would be a second variable.
//   * NO compare_exchange / fetch_add / read-modify-write. Each cursor still
//     has exactly one writer, so none is needed (see the frozen header's
//     ownership argument, which applies here unchanged).
//   * NO CPU affinity, pinning or priority changes.
//   * NO change to the payload type, the payload indexing, or the full/empty
//     semantics.
//
// ---------------------------------------------------------------------------
// FALSE SHARING vs TRUE SHARING — WHAT THIS TYPE DOES AND DOES NOT REMOVE
// ---------------------------------------------------------------------------
// head_ is written only by the producer and tail_ only by the consumer, so
// neither thread ever writes the other's cursor. When the two cursors share a
// line, the producer's release store of head_ invalidates the line that also
// holds tail_, and the consumer's next access to its own tail_ therefore misses
// — even though the two threads never touch the same object. That is FALSE
// sharing: an artefact of line granularity, and the thing the separated layout
// removes.
//
// The separated layout does NOT remove the legitimate traffic: the producer
// must still read tail_ (acquire) and the consumer must still read head_
// (acquire), because those reads are correctness gates on slot reuse and
// payload availability (frozen header, "Reuse gate" / "Availability gate").
// Those are REQUIRED remote observations and they remain in both variants. Any
// measured difference between the two variants is therefore attributable to
// line granularity, not to the existence of cross-thread communication.
// See docs/SPSC_FALSE_SHARING.md.
// ---------------------------------------------------------------------------

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "cache_line.h"

namespace lltl {

// Which cursor placement a policy implements. Used by the report and by the
// benchmark's cell naming; it does not influence the algorithm.
enum class CursorLayout { SameLine, Separated };

inline const char* cursor_layout_name(CursorLayout layout) noexcept {
    return layout == CursorLayout::SameLine ? "same_line" : "separated";
}

// ---------------------------------------------------------------------------
// SameLineCursorBlock — both cursors inside ONE interference block.
//
// This is deliberately NOT "declare head; then tail; and hope". The block is
// aligned to kAssumedCacheLineSize and its SIZE is asserted equal to
// kAssumedCacheLineSize, which makes "both cursors are in the same assumed
// interference block" a property of the type rather than of a particular
// object's address:
//
//   * every complete object of this type is kAssumedCacheLineSize-aligned, so
//     each occupies exactly one assumed block and is never split across two;
//   * both members live inside that block by definition of sizeof.
//
// The member offsets (0 and 8) additionally keep both cursors inside the same
// block for any real line size that could be reported (>= 16), so the property
// survives a host whose line size is smaller than the assumption.
//
// Note what is NOT here: no padding member "between" the cursors, no payload,
// and no unrelated hot metadata. The block contains the two cursors and nothing
// the algorithm reads or writes.
// ---------------------------------------------------------------------------
struct SameLineCursorBlock {
    static constexpr CursorLayout kLayout = CursorLayout::SameLine;

    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
};

// ---------------------------------------------------------------------------
// SeparatedCursorBlocks — each cursor in its OWN interference block.
//
// Both members carry the alignment, so head is at offset 0 and tail at offset
// kAssumedCacheLineSize of a block whose size is asserted to be exactly two
// assumed blocks. Neither cursor therefore begins in, or shares, the other's
// assumed block, and neither block contains any other member.
//
// The measured addresses are still verified per-instance at runtime: this
// static layout is the guarantee for the ASSUMED line size, and
// verify_cursor_layout()/cursor_layout_report() is the guarantee for the line
// size the host actually reports.
// ---------------------------------------------------------------------------
struct SeparatedCursorBlocks {
    static constexpr CursorLayout kLayout = CursorLayout::Separated;

    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> head{0};
    alignas(kAssumedCacheLineSize) std::atomic<std::size_t> tail{0};
};

// Compile-time guarantee that each policy's storage has the shape claimed
// above. If a future edit (or an ABI whose atomic size differs) broke the
// layout, the build fails here rather than the experiment silently measuring
// the wrong thing.
static_assert(sizeof(SameLineCursorBlock) == 1 * kAssumedCacheLineSize,
              "the same-line cursor block must occupy exactly one assumed "
              "interference block, so that both cursors are in it");
static_assert(sizeof(SeparatedCursorBlocks) == 2 * kAssumedCacheLineSize,
              "the separated cursor blocks must occupy exactly two assumed "
              "interference blocks, one per cursor");
static_assert(alignof(SameLineCursorBlock) >= kAssumedCacheLineSize);
static_assert(alignof(SeparatedCursorBlocks) >= kAssumedCacheLineSize);

// ---------------------------------------------------------------------------
// CursorLayoutReport — the runtime address/layout evidence for ONE object.
//
// Produced before any timing starts, never inside a timed interval. This is
// what makes the experiment's controls falsifiable: the same-line variant is
// only a same-line control if `cursors_same_line` is true for the object that
// actually ran, and the separated variant is only a separated control if it is
// false for the object that actually ran. `ok()` is the conjunction the
// experiment requires before a cell may be published.
// ---------------------------------------------------------------------------
struct CursorLayoutReport {
    CursorLayout   layout{};
    std::size_t    reported_line_size{0};  // from the host at runtime
    std::size_t    layout_alignment{0};    // compile-time assumption in use
    std::uintptr_t head_address{0};
    std::uintptr_t tail_address{0};
    std::size_t    head_line_index{0};
    std::size_t    tail_line_index{0};
    std::uintptr_t payload_begin{0};
    std::uintptr_t payload_end{0};
    bool           cursors_same_line{false};
    bool           cursors_share_payload_line{false};
    bool           line_size_supported{false};
    bool           same_line_invariant_holds{false};
    bool           payload_disjoint{false};

    // Every Phase-3A precondition. A cell whose report is not ok() must FAIL
    // rather than publish: an unverified layout is not evidence about layout.
    bool ok() const noexcept {
        return line_size_supported && same_line_invariant_holds &&
               payload_disjoint;
    }

    // One-line, greppable form for the dataset metadata and the invariant log.
    // Reports the MEASURED addresses and indices, not the intended ones.
    std::string summary_line() const {
        std::string s;
        s += "layout=";
        s += cursor_layout_name(layout);
        s += " alignment=" + std::to_string(layout_alignment);
        s += " reported_cache_line_size=" + std::to_string(reported_line_size);
        s += " head_addr=0x";
        s += hex(head_address);
        s += " tail_addr=0x";
        s += hex(tail_address);
        s += " head_line=" + std::to_string(head_line_index);
        s += " tail_line=" + std::to_string(tail_line_index);
        s += " same_line=";
        s += cursors_same_line ? "yes" : "no";
        s += " cursor_shares_payload_line=";
        s += cursors_share_payload_line ? "yes" : "no";
        s += " line_size_supported=";
        s += line_size_supported ? "yes" : "no";
        s += " same_line_invariant=";
        s += same_line_invariant_holds ? "PASS" : "FAIL";
        s += " payload_disjoint=";
        s += payload_disjoint ? "PASS" : "FAIL";
        s += " layout_ok=";
        s += ok() ? "PASS" : "FAIL";
        return s;
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

// ---------------------------------------------------------------------------
// CursorLayoutRingBuffer — the single algorithm body.
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity, typename CursorLayoutPolicy>
class CursorLayoutRingBuffer {
    static_assert(Capacity > 0,
                  "CursorLayoutRingBuffer requires Capacity > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "CursorLayoutRingBuffer requires a power-of-two Capacity so "
                  "that slot = counter & (Capacity - 1) needs no modulo");
    static_assert(std::is_default_constructible_v<T>,
                  "Phase 3A stores T in a preallocated std::array; T must be "
                  "default-constructible");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "Experiment 02 requires a platform where "
                  "std::atomic<std::size_t> is always lock-free: the SPSC "
                  "cursor protocol (relaxed/acquire/release) is only lock-free "
                  "where the cursor atomic is. No mutex fallback is provided.");
    static_assert(
        std::is_same_v<CursorLayoutPolicy, SameLineCursorBlock> ||
            std::is_same_v<CursorLayoutPolicy, SeparatedCursorBlocks>,
        "CursorLayoutPolicy must be one of the two declared Phase-3A cursor "
        "layout policies; the experiment has exactly two controls and adding a "
        "third here would silently change what is being compared");

public:
    using layout_policy = CursorLayoutPolicy;

    // Which control this instantiation is. Compile-time; used for reporting and
    // for the benchmark's cell naming.
    static constexpr CursorLayout layout() noexcept {
        return CursorLayoutPolicy::kLayout;
    }

    CursorLayoutRingBuffer() = default;

    // Not copyable / movable: atomics are not copyable and transferring the
    // buffer between threads while one side is mid-call would break ownership.
    // Identical to the frozen Phase-1 type.
    CursorLayoutRingBuffer(const CursorLayoutRingBuffer&)            = delete;
    CursorLayoutRingBuffer& operator=(const CursorLayoutRingBuffer&) = delete;
    CursorLayoutRingBuffer(CursorLayoutRingBuffer&&)                 = delete;
    CursorLayoutRingBuffer& operator=(CursorLayoutRingBuffer&&)      = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // --- Producer side (call ONLY from the single producer thread) ---------
    //
    // Semantically identical to SpscRingBuffer::try_push, including the
    // memory orders and the reuse-gate argument (frozen header). Reproduced
    // here rather than inherited so that Phase 3A owns no code path through the
    // frozen type.
    bool try_push(const T& v) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t h = cursors_.head.load(std::memory_order_relaxed);
        if (h - cursors_.tail.load(std::memory_order_acquire) == Capacity) {
            return false; // full
        }
        slots_[h & (Capacity - 1)] = v;                // payload write
        cursors_.head.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    bool try_push(T&& v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t h = cursors_.head.load(std::memory_order_relaxed);
        if (h - cursors_.tail.load(std::memory_order_acquire) == Capacity) {
            return false; // full
        }
        slots_[h & (Capacity - 1)] = std::move(v);     // payload write
        cursors_.head.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    // --- Consumer side (call ONLY from the single consumer thread) ---------
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t t = cursors_.tail.load(std::memory_order_relaxed);
        if (cursors_.head.load(std::memory_order_acquire) == t) {
            return false; // empty
        }
        out = std::move(slots_[t & (Capacity - 1)]);   // payload read
        cursors_.tail.store(t + 1, std::memory_order_release);
        return true;
    }

    // --- Observers ---------------------------------------------------------
    // ADVISORY only, exactly as in the frozen Phase-1 type: two independent
    // relaxed loads, not a coherent pair-snapshot.
    bool empty() const noexcept {
        return cursors_.head.load(std::memory_order_relaxed) ==
               cursors_.tail.load(std::memory_order_relaxed);
    }

    // --- Layout introspection (Phase 3A only; NOT part of the transfer path) -
    //
    // These read no cursor state and take no part in any push/pop. They exist
    // so the experiment can MEASURE the layout it is claiming, on the object
    // that actually ran, instead of asserting it from the type.

    std::uintptr_t cursor_head_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(&cursors_.head);
    }

    std::uintptr_t cursor_tail_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(&cursors_.tail);
    }

    std::uintptr_t payload_begin_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(slots_.data());
    }

    std::uintptr_t payload_end_address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(slots_.data() + Capacity);
    }

    // Build the full runtime layout evidence for THIS object under the line
    // size the host reports. Pure observation; safe to call before any thread
    // exists and outside every timed interval.
    CursorLayoutReport cursor_layout_report(
        std::size_t reported_line_size) const noexcept {
        CursorLayoutReport report;
        report.layout             = layout();
        report.reported_line_size = reported_line_size;
        report.layout_alignment   = kAssumedCacheLineSize;
        report.head_address       = cursor_head_address();
        report.tail_address       = cursor_tail_address();
        report.payload_begin      = payload_begin_address();
        report.payload_end        = payload_end_address();

        report.head_line_index =
            cache_line_index(report.head_address, reported_line_size);
        report.tail_line_index =
            cache_line_index(report.tail_address, reported_line_size);

        report.cursors_same_line =
            same_cache_line(report.head_address, report.tail_address,
                            reported_line_size);

        report.line_size_supported = line_size_supported(reported_line_size);

        // The control is only a control if the MEASURED placement matches the
        // placement this variant claims. A "same line" object whose cursors
        // straddle a line, or a "separated" object whose cursors share one, is
        // not evidence about false sharing in either direction.
        const bool expected_same_line = (layout() == CursorLayout::SameLine);
        report.same_line_invariant_holds =
            (report.cursors_same_line == expected_same_line);

        // Cursor interference blocks must not contain payload storage: a
        // cursor line that also holds payload would add a second, uncontrolled
        // sharing relationship to the experiment.
        const auto block_overlaps_payload = [&](std::uintptr_t address) {
            if (reported_line_size == 0) {
                return false;
            }
            const std::uintptr_t block_start =
                static_cast<std::uintptr_t>(
                    cache_line_index(address, reported_line_size)) *
                reported_line_size;
            const std::uintptr_t block_end = block_start + reported_line_size;
            return block_start < report.payload_end &&
                   report.payload_begin < block_end;
        };

        report.cursors_share_payload_line =
            block_overlaps_payload(report.head_address) ||
            block_overlaps_payload(report.tail_address);
        report.payload_disjoint = !report.cursors_share_payload_line;

        return report;
    }

private:
    // Layout note — the ONE thing Phase 3A controls.
    //
    //   cursors_ : the cursor policy's block(s). SameLine keeps both cursors in
    //              one assumed interference block; Separated puts each in its
    //              own. Nothing else lives in either block.
    //   slots_   : payload storage, explicitly aligned to an interference
    //              boundary so that no payload line is also a cursor line, in
    //              EITHER variant. The payload's alignment, indexing and
    //              internal layout are identical across the two variants, so
    //              the payload is not a second variable.
    //
    // Both variants use this same member ORDER (cursors first, payload second)
    // and the same payload alignment; the only difference between them is what
    // CursorLayoutPolicy contributes.
    CursorLayoutPolicy cursors_{};
    alignas(kAssumedCacheLineSize) std::array<T, Capacity> slots_{};
};

// The two Phase-3A controls. Named types rather than a bool flag, so that the
// distinction is visible at every use site and cannot be defaulted away.
template <typename T, std::size_t Capacity>
using SpscSameLineRingBuffer =
    CursorLayoutRingBuffer<T, Capacity, SameLineCursorBlock>;

template <typename T, std::size_t Capacity>
using SpscSeparatedCursorRingBuffer =
    CursorLayoutRingBuffer<T, Capacity, SeparatedCursorBlocks>;

} // namespace lltl
