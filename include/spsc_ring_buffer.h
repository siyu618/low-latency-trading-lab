#pragma once

// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 1 (correctness).
//
// SpscRingBuffer: a fixed-capacity, single-producer / single-consumer ring
// buffer that transfers whole messages between exactly one producer thread and
// exactly one consumer thread with NO mutex and NO CAS. Cross-thread ordering
// comes only from the minimal acquire/release pairs described below; the
// producer and consumer otherwise never wait on each other's cursors.
//
// This models a low-latency trading pipeline:
//
//     Feed / Decoder Thread  ->  SPSC  ->  OrderBook / Strategy Thread
//
// Ownership model (the reason no CAS is needed):
//     head_  (#messages produced) is written ONLY by the producer
//     tail_  (#messages consumed) is written ONLY by the consumer
//     each thread reads the OTHER thread's cursor as a hint and nothing more.
//   Because each atomic has exactly one writer, a read-modify-write cycle is
//   never required: no two threads ever race to update the same cursor, so
//   compare_exchange would be pure waste. See docs/SPSC_MEMORY_MODEL.md.
//
// THREADING CONTRACT (enforced by documentation, not by runtime locks):
//   * exactly one producer thread may call try_push concurrently;
//   * exactly one consumer thread may call try_pop concurrently;
//   * any other arrangement (>= 2 producers, >= 2 consumers, or a thread
//     calling both push and pop concurrently with a counterpart) is
//     UNSUPPORTED and undefined.
//
// Indexing: monotonically increasing 64-bit counters select the physical slot
// with a bit mask (Capacity is a power of two):
//     slot = counter & (Capacity - 1)
// The counters count messages ever produced/consumed and are allowed to wrap
// past 2^64 only in the sense documented below; the mask keeps working because
// the slot position is periodic in Capacity and never uses modulo arithmetic.
//
// Full capacity is used (no wasted slot): with monotonic counters the full and
// empty states are distinguished on the counter domain —
//     empty:  head_ == tail_
//     full:   head_ - tail_ == Capacity
// — so there is no ambiguity about a shared physical slot. See
// "Counter wrap-around" in docs/SPSC_MEMORY_MODEL.md for why unsigned
// subtraction stays correct across wrap.
//
// Phase 1 simplifications (deliberate, documented, to be revisited later):
//   * Payload storage is std::array<T, Capacity>. T must be
//     default-constructible (array elements always exist) and assignable; the
//     producer stores into / the consumer reads from a live element with
//     ordinary assignment, never placement-new. No dynamic allocation occurs
//     inside try_push / try_pop.
//   * Cursors are cached nowhere: every call loads both cursors. The
//     remote-cursor-cache (cached_head/cached_tail) optimization is a SEPARATE,
//     later, controlled experiment and is intentionally NOT implemented here.
//   * Cursor layout is deliberately packed and unpadded (head_ then tail_,
//     adjacent). False-sharing isolation is a SEPARATE, later, controlled
//     experiment (Phase 3) and is intentionally NOT done here.
//
// Memory ordering (minimum, see SPSC_MEMORY_MODEL.md for the full argument):
//   Producer  try_push:
//     local_head = head_.load(relaxed)   // own cursor: only I write it
//     tail_.load(acquire)                // gate slot reuse (see below)
//     slots_[local_head & (Cap-1)] = v   // payload write
//     head_.store(local_head+1, release) // publish payload
//   Consumer  try_pop:
//     local_tail = tail_.load(relaxed)   // own cursor: only I write it
//     head_.load(acquire)                // gate data availability (see below)
//     out = std::move(slots_[local_tail & (Cap-1)]) // payload read
//     tail_.store(local_tail+1, release) // release the slot for reuse
//
// Exceptions: the queue machinery itself never throws and never allocates. If
// T's copy/move assignment throws (i.e. when the noexcept specifications below
// evaluate to false) the exception propagates out of try_push / try_pop, the
// cursor is NOT advanced, and the only side effect is that one payload slot may
// have been partially assigned. Intended message types are nothrow-movable and
// never hit this path.
// ---------------------------------------------------------------------------

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace lltl {

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity > 0, "SpscRingBuffer requires Capacity > 0");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscRingBuffer requires a power-of-two Capacity so that "
                  "slot = counter & (Capacity - 1) needs no modulo");
    static_assert(std::is_default_constructible_v<T>,
                  "Phase 1 stores T in a preallocated std::array; T must be "
                  "default-constructible");

public:
    SpscRingBuffer() = default;

    // Not copyable / movable: atomics are not copyable and transferring the
    // buffer between threads while one side is mid-call would break ownership.
    SpscRingBuffer(const SpscRingBuffer&)            = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&)                 = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&)      = delete;

    // Maximum number of elements the buffer can hold. Compile-time constant.
    // Because full is "head - tail == Capacity" (not one slot short), every
    // slot is usable.
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // --- Producer side (call ONLY from the single producer thread) ---------

    // Enqueue `v`. Returns true if enqueued; false if the buffer is full.
    // Never blocks, never spins, never sleeps, never allocates.
    //
    // Reuse gate: slot = local_head & (Capacity-1) was last written by the
    // producer when head_ was (local_head - Capacity). That occupant may only
    // be overwritten once the consumer has FINISHED reading it, which the
    // consumer announces by advancing tail_. The acquire load below therefore
    // observes the consumer's release store of tail_ and orders our payload
    // write after the consumer's read of the old occupant.
    bool try_push(const T& v) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == Capacity) {
            return false; // full
        }
        slots_[h & (Capacity - 1)] = v; // payload write (may throw for a throwing T)
        head_.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    // Enqueue by moving from `v`. Same contract as the copy overload.
    bool try_push(T&& v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == Capacity) {
            return false; // full
        }
        slots_[h & (Capacity - 1)] = std::move(v); // payload write
        head_.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    // --- Consumer side (call ONLY from the single consumer thread) ---------

    // Dequeue the oldest element into `out` by move assignment. Returns true
    // if an element was removed; false if the buffer is empty (out untouched).
    // Never blocks, never spins, never sleeps, never allocates.
    //
    // Availability gate: slot = local_tail & (Capacity-1) may be read only
    // after the producer has published the payload. The acquire load below
    // observes the producer's release store of head_ and orders our payload
    // read after the producer's write of it (and of every earlier message).
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) {
            return false; // empty
        }
        out = std::move(slots_[t & (Capacity - 1)]); // payload read
        // Release the slot: the producer may overwrite it only after seeing
        // this store (and the payload read above is sequenced before it).
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // --- Observers ---------------------------------------------------------

    // True if no element is in flight (head_ == tail_). For a single thread —
    // or a thread that is itself the only caller on its side — this is exact.
    // Concurrently with the other side it is only a point-in-time hint: the
    // remote cursor may move immediately after this load. Control flow between
    // the two threads must use the try_push / try_pop return values, not this.
    bool empty() const noexcept {
        // relaxed: nothing is ordered here; the caller gets a snapshot only.
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

private:
    // Layout note: payload storage first, then the two cursors, deliberately
    // packed and unpadded. head_ (producer-owned) and tail_ (consumer-owned)
    // sit adjacent, so the two threads contend on a shared cache line — that
    // false sharing is the intentional Phase-1 baseline. Phase 3 will run the
    // controlled experiment (packed vs separated/padded); no padding here.
    std::array<T, Capacity> slots_{};
    std::atomic<std::size_t> head_{0}; // # produced; written ONLY by producer
    std::atomic<std::size_t> tail_{0}; // # consumed; written ONLY by consumer
};

} // namespace lltl
