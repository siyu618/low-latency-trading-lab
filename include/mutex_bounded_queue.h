#pragma once

// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency.
//
// MutexBoundedQueue: a bounded FIFO guarded by a single std::mutex. It is the
// CORRECTNESS / REFERENCE baseline for Experiment 02 Phase 1: the SPSC ring
// buffer (spsc_ring_buffer.h) must agree with it on every observable FIFO
// behavior, so this type exists to be compared against, not to be fast.
//
// Deliberate non-goals (this is a reference queue, not a candidate):
//   * No condition_variable — the API is non-blocking (try_*). A condvar is
//     pointless without blocking wait/pop, and Phase 1 never blocks.
//   * Not optimized — one mutex per call, modulo indexing, no batching.
//   * No virtual interface. MutexBoundedQueue and SpscRingBuffer share a
//     common conceptual API (try_push/try_pop/empty/capacity) by convention
//     only, so the differential test drives both with the same code.
//
// Concurrency: this queue is thread-safe for ANY number of producers and
// consumers (the mutex serializes everyone). It exists precisely to show the
// SPSC protocol does not need that generality — see SPSC_MEMORY_MODEL.md.
//
// Storage is a fixed preallocated std::array<T, Capacity>. As a Phase-1
// simplification T must be default-constructible (so the array elements can
// exist) and assignable from the pushed value / into the popped out-parameter.
// No allocation happens inside try_push / try_pop (or anywhere in this queue).
//
// Exception behavior: the mutex machinery itself never throws. If T's copy or
// move assignment throws, the exception propagates out of try_push / try_pop;
// the queue is left internally consistent because the cursor (count_/head_) is
// only advanced after the element assignment succeeds. See the same note in
// spsc_ring_buffer.h for the SPSC variant's (slightly different) guarantee.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <mutex>
#include <type_traits>
#include <utility>

namespace lltl {

template <typename T, std::size_t Capacity>
class MutexBoundedQueue {
    static_assert(Capacity > 0, "MutexBoundedQueue requires Capacity > 0");
    static_assert(std::is_default_constructible_v<T>,
                  "Phase 1 reference queue stores T in a preallocated "
                  "std::array; T must be default-constructible");

public:
    MutexBoundedQueue() = default;

    // Non-copyable / non-movable: a mutex is not copyable, and allowing the
    // buffer to be moved would make the guarded region ambiguous.
    MutexBoundedQueue(const MutexBoundedQueue&)            = delete;
    MutexBoundedQueue& operator=(const MutexBoundedQueue&) = delete;
    MutexBoundedQueue(MutexBoundedQueue&&)                 = delete;
    MutexBoundedQueue& operator=(MutexBoundedQueue&&)      = delete;

    // Maximum number of elements the queue can hold. Compile-time constant.
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Enqueue a copy of `v`. Returns false (and leaves the queue unchanged)
    // if the queue is full. True if the element was enqueued.
    //
    // noexcept is true exactly when T's copy assignment cannot throw; a
    // throwing copy assignment would propagate before count_ is advanced.
    bool try_push(const T& v) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == Capacity) {
            return false; // full; queue unchanged
        }
        slots_[(head_ + count_) % Capacity] = v; // may throw for a throwing T
        ++count_;
        return true;
    }

    // Enqueue by moving from `v`. Returns false (queue unchanged) if full.
    bool try_push(T&& v) noexcept(std::is_nothrow_move_assignable_v<T>) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == Capacity) {
            return false; // full; queue unchanged
        }
        slots_[(head_ + count_) % Capacity] = std::move(v);
        ++count_;
        return true;
    }

    // Dequeue the front element into `out` by move assignment. Returns false
    // (and leaves `out` untouched) if the queue is empty. True if an element
    // was removed.
    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == 0) {
            return false; // empty; out untouched
        }
        out = std::move(slots_[head_]); // may throw for a throwing T
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }

    // True if the queue holds no elements. Exact (serialized by the mutex).
    bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return count_ == 0;
    }

private:
    mutable std::mutex mu_;
    std::array<T, Capacity> slots_{};
    std::size_t head_  = 0; // index of the front element (oldest)
    std::size_t count_ = 0; // number of elements currently held (0..Capacity)
};

} // namespace lltl
