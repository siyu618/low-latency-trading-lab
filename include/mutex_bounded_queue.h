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
//   * No condition_variable — the try_* API never waits for the QUEUE STATE to
//     change. (Acquiring the mutex can still block; see the contract below.)
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
// The queue's own storage is preallocated and the implementation performs no
// allocator calls in the hot path; T's copy/move assignment is arbitrary user
// code and may itself allocate, throw, or block.
//
// BLOCKING / EXCEPTION CONTRACT (a mutex queue, and honest about it):
//   * try_push / try_pop / empty never wait for the QUEUE STATE to change —
//     they report full/empty immediately once they hold the mutex.
//   * However, obtaining the mutex itself may BLOCK under contention, and
//     std::mutex::lock() may throw std::system_error. So these methods are
//     neither non-blocking nor noexcept, and MutexBoundedQueue is not claimed
//     to be either. That is expected and acceptable for a reference baseline;
//     no attempt is made to optimize it away.
//   * If T's copy/move assignment throws, the exception propagates out of
//     try_push / try_pop. The queue stays internally consistent because
//     count_ / head_ advance only after the element assignment succeeds. The
//     VALUE guarantee is the same weak one documented in spsc_ring_buffer.h:
//     a throwing move may leave the source slot partially moved-from, and no
//     rollback is attempted.
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

    // Enqueue a copy of `v`. Returns false (and leaves the queue unchanged) if
    // the queue is full; true if the element was enqueued. Does not wait for
    // the queue to drain, but acquiring the mutex may block under contention.
    // Not noexcept: std::mutex::lock() may throw std::system_error, and T's
    // copy assignment may throw.
    bool try_push(const T& v) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == Capacity) {
            return false; // full; queue unchanged
        }
        slots_[(head_ + count_) % Capacity] = v; // may throw for a throwing T
        ++count_;
        return true;
    }

    // Enqueue by moving from `v`. Returns false (queue unchanged) if full.
    // Same blocking/exception contract as the copy overload.
    bool try_push(T&& v) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == Capacity) {
            return false; // full; queue unchanged
        }
        slots_[(head_ + count_) % Capacity] = std::move(v);
        ++count_;
        return true;
    }

    // Dequeue the front element into `out` by move assignment. Returns false
    // (and leaves `out` untouched) if the queue is empty; true if an element was
    // removed. Does not wait for an element to arrive, but acquiring the mutex
    // may block under contention. Not noexcept, as above.
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ == 0) {
            return false; // empty; out untouched
        }
        out = std::move(slots_[head_]); // may throw for a throwing T
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }

    // True if the queue holds no elements. Exact (serialized by the mutex), but
    // unlike SpscRingBuffer::empty() it is not observational-only — it takes the
    // lock, so it may block under contention and is not noexcept. The result is
    // still only valid at the instant it is returned.
    bool empty() const {
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
