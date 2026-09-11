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
//     each thread reads the OTHER thread's cursor as a remote SYNCHRONIZATION
//     cursor: it is a correctness gate, not an advisory hint. The consumer's
//     acquire load of head_ gates safe payload consumption; the producer's
//     acquire load of tail_ gates safe slot reuse. (Both gates are exact when
//     they permit an access, and merely conservative when they do not — see
//     docs/SPSC_MEMORY_MODEL.md §3.3.)
//   Because each atomic has exactly one writer, a read-modify-write cycle is
//   never required: no two threads ever race to update the same cursor, so
//   compare_exchange would be pure waste. See docs/SPSC_MEMORY_MODEL.md.
//
// Lock-free requirement: C++ does not universally require
// std::atomic<std::size_t> to be lock-free, so this experiment states the
// property explicitly with a static_assert below. Where it holds, the cursor
// PROTOCOL (relaxed / acquire / release on head_ and tail_) is lock-free. That
// says nothing about T's own copy/move assignment, which runs arbitrary user
// code and may allocate, block, or throw. No mutex fallback is provided.
//
// THREADING CONTRACT (enforced by documentation, not by runtime locks):
//   * exactly one producer thread may call try_push concurrently;
//   * exactly one consumer thread may call try_pop concurrently;
//   * any other arrangement (>= 2 producers, >= 2 consumers, or a thread
//     calling both push and pop concurrently with a counterpart) is
//     UNSUPPORTED and undefined.
//
// Indexing: monotonically increasing unsigned std::size_t counters (64-bit on
// the canonical 64-bit development hosts, but the reasoning below is generic to
// the width of std::size_t) select the physical slot with a bit mask (Capacity
// is a power of two):
//     slot = counter & (Capacity - 1)
// The counters count messages ever produced/consumed and are allowed to wrap
// modulo 2^width(std::size_t); the mask keeps working because the slot position
// is periodic in Capacity and never uses modulo arithmetic.
//
// Full capacity is used (no wasted slot): with monotonic counters the full and
// empty states are distinguished on the counter domain —
//     empty:  head_ == tail_
//     full:   head_ - tail_ == Capacity
// — so there is no ambiguity about a shared physical slot. See
// "Counter wrap-around" in docs/SPSC_MEMORY_MODEL.md for why unsigned
// subtraction stays correct across wrap.
//
// Cursor atomic operations per transfer: one successful try_push performs
// three cursor atomics (head_.load relaxed, tail_.load acquire, head_.store
// release) and one successful try_pop performs three (tail_.load relaxed,
// head_.load acquire, tail_.store release) — six per complete producer ->
// consumer message transfer in this Phase-1 baseline. Reducing that count is
// the motivation for the deferred remote-cursor-cache experiment; it is NOT
// implemented here.
//
// Allocation: the queue's own storage is preallocated (see below) and the queue
// implementation performs no allocator calls in the hot path. T's copy/move
// assignment is arbitrary user code and may itself allocate, throw, or block —
// so no claim is made that a transfer is allocation-free or non-blocking in
// general.
//
// Phase 1 simplifications (deliberate, documented, to be revisited later):
//   * Payload storage is std::array<T, Capacity>. T must be
//     default-constructible (array elements always exist) and assignable; the
//     producer stores into / the consumer reads from a live element with
//     ordinary assignment, never placement-new. Phase 2 benchmark message types
//     will be fixed-size, nothrow, allocation-free value types.
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
// Exceptions: the queue's own machinery makes no allocator calls and does not
// throw for nothrow-assignable T (the noexcept specifications below make that
// explicit). If T's assignment throws, the exception propagates out of
// try_push / try_pop and the cursor is NOT advanced, so the queue stays
// structurally consistent (no message is published, no slot is released).
// The VALUE guarantee is weaker and is deliberately not "strong":
//   * try_pop moves the stored payload into the caller's `out` as an rvalue.
//     If that move assignment throws, the source slot may already be partially
//     modified/moved-from. The message is still not popped (tail_ unchanged),
//     but the queue does NOT promise to recover the original stored value.
//   * try_push may likewise leave the target slot partially assigned.
// Therefore a throwing move is NOT recoverable here. Intended low-latency
// message types are nothrow copy/move-assignable fixed-size values, for which
// none of this arises. No rollback machinery is provided (and none is wanted
// on the hot path).
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
    // This experiment only supports platforms where the cursor atomic is
    // ALWAYS lock-free, so the cursor protocol's lock-free claim is checked at
    // compile time rather than assumed. There is intentionally no mutex
    // fallback: a silent fallback would hide exactly the property under study.
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "Experiment 02 requires a platform where "
                  "std::atomic<std::size_t> is always lock-free: the SPSC "
                  "cursor protocol (relaxed/acquire/release on head_/tail_) is "
                  "only lock-free where the cursor atomic is. No mutex "
                  "fallback is provided by design.");

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
    // The queue itself never blocks, spins, sleeps, or calls the allocator
    // (T's copy/move assignment may of course do any of those).
    //
    // Reuse gate: slot = local_head & (Capacity-1) was last written by the
    // producer when head_ was (local_head - Capacity). That occupant may only
    // be overwritten once the consumer has FINISHED reading it, which the
    // consumer announces by advancing tail_. The acquire load below therefore
    // observes the consumer's release store of tail_ and orders our payload
    // write after the consumer's read of the old occupant. A stale tail read is
    // conservative: it can only make the buffer look falsely full, never permit
    // overwriting a slot the consumer still owns.
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
    // The queue itself never blocks, spins, sleeps, or calls the allocator
    // (T's copy/move assignment may of course do any of those).
    //
    // Availability gate: slot = local_tail & (Capacity-1) may be read only
    // after the producer has published the payload. The acquire load below
    // observes the producer's release store of head_ and orders our payload
    // read after the producer's write of it (and of every earlier message). A
    // stale head read is conservative: it can only make the buffer look falsely
    // empty, never permit reading a payload that has not been published.
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

    // ADVISORY observation, not a synchronization primitive and NOT a coherent
    // pair-snapshot of the cursors: head_ and tail_ are read as two independent
    // relaxed loads, so even when called by the designated producer or consumer
    // the remote value may already be stale when the local one is loaded, and a
    // third (observer) thread is guaranteed nothing coherent at all. It is a
    // useful diagnostic (e.g. in single-threaded tests) and it is safe — a stale
    // remote read can only look falsely empty/full, never permit an unsafe
    // access. Cross-thread control flow must use the try_push / try_pop return
    // values, which perform the real acquire-gated checks.
    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

private:
    // Layout note: payload storage first, then the two cursors, deliberately
    // packed and unpadded. head_ (producer-owned) and tail_ (consumer-owned) are
    // adjacent members, which PERMITS and is likely to exhibit false sharing —
    // but adjacency does not by itself guarantee that the two objects land in
    // the same physical cache line for every object address and platform. That
    // unpadded layout is the intentional Phase-1 baseline. Phase 3 will run a
    // controlled experiment that explicitly verifies cursor addresses /
    // cache-line placement for a packed same-line control and a
    // separated/padded control. No padding here.
    std::array<T, Capacity> slots_{};
    std::atomic<std::size_t> head_{0}; // # produced; written ONLY by producer
    std::atomic<std::size_t> tail_{0}; // # consumed; written ONLY by consumer
};

} // namespace lltl
