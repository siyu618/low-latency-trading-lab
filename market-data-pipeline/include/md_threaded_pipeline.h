#pragma once

#include "flat_order_book.h"
#include "market_data_pipeline.h"
#include "md_decoder.h"
#include "md_message.h"
#include "md_stream_decoder.h"
#include "spsc_remote_cursor_ring_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

// ---------------------------------------------------------------------------
// Experiment 03 Phase 2 — the threaded pipeline.
//
//   byte chunks -> Decoder Thread -> SPSC -> Book Thread -> MarketDataPipeline
//
// This header INTEGRATES four frozen components and reimplements none of them:
// Phase-1B's `decode_one` (via the `StreamDecoder` framer), Experiment 02's
// selected SPSC queue, Phase-1A's `MarketDataPipeline`, and Experiment 01's
// `FlatOrderBook`. The only new code is the two thread bodies and the
// completion protocol that joins them.
//
// The queue is `lltl::SpscSeparatedBaselineRingBuffer` — the FINAL frozen
// Experiment-02 selection, the uncached separated-cursor baseline. Not the
// cached-remote-cursor variant, not the cursor-layout variants, not an
// instrumented build, and not a copy: this file includes the frozen header and
// instantiates it. Phase 2 changes no memory order, no cursor placement and no
// capacity semantic of that queue.
//
// ---------------------------------------------------------------------------
// OWNERSHIP CONTRACT (see docs/MARKET_DATA_PIPELINE.md § Phase 2)
// ---------------------------------------------------------------------------
//
// The decoder thread owns, for the whole run:
//
//     the byte chunk cursor          (the caller's ChunkSource)
//     the StreamDecoder carry buffer
//     all framing/decoding state
//     the SPSC PRODUCER side
//
// The book thread owns, for the whole run:
//
//     MarketDataPipeline<FlatOrderBook>
//     the FlatOrderBook inside it
//     the SPSC CONSUMER side
//
// The object's own thread (`run`'s caller) touches none of that while the
// workers are alive, and inspects the result only AFTER joining both. That is
// what preserves Experiment 01's single-writer OrderBook design: the book has
// exactly one writing thread for the entire run, so it needs no lock, no
// atomic and no per-field synchronization — the same argument as the
// single-threaded case, with a different thread in the role.
//
// THE BOOK IS ALWAYS `FlatOrderBook`. It is deliberately NOT a template
// parameter. Phase 2's subject is the threaded composition, and pushing every
// message into both a flat and a map book from the consumer thread would change
// the work being exercised into a dual-book validation pipeline. The reference
// for correctness is computed separately, single-threaded, by the tests.
//
// ---------------------------------------------------------------------------
// COMPLETION: `producer_done_`, and what it actually buys
// ---------------------------------------------------------------------------
//
// End-of-input is a runtime fact, not a market-data message, so it is NOT a
// wire message and NOT a new `MdKind`: adding `EndOfStream` to the protocol
// would put a control-plane concept into the typed vocabulary every layer
// shares, and would make the wire format depend on how a particular process
// happens to be structured. Instead the producer publishes one atomic flag
// after its last successful push:
//
//     producer:  ... all pushes done ...
//                producer_done_.store(true, std::memory_order_release);
//
//     consumer:  ... try_pop fails ...
//                if (producer_done_.load(std::memory_order_acquire)) {
//                    final try_pop; if that fails too, exit
//                }
//
// THE HAPPENS-BEFORE ARGUMENT. The release store and the acquire load form a
// release/acquire pair on the same atomic, so everything sequenced-before the
// store in the producer thread happens-before everything sequenced-after the
// load in the consumer thread. Every `try_push` the producer will ever perform
// is sequenced before that store, therefore every one of them happens-before
// the consumer's subsequent `try_pop`. Two consequences follow, and they are
// the whole protocol:
//
//   * The consumer's final `try_pop` observes the queue as of AFTER the
//     producer's last push. A failure there is therefore not a race that more
//     yielding could win — it means the queue is empty and no further push can
//     ever make it non-empty.
//   * The `producer_done_` flag is what makes that emptiness FINAL. The queue's
//     own release-store/acquire-load on its cursors is what makes pushed PAYLOAD
//     bytes visible; the flag is orthogonal to that and does not duplicate it.
//
// What a push does NOT need is to be ordered with respect to the flag by the
// queue — the flag is a separate atomic and the ordering comes from the
// release/acquire pair above, not from the queue's cursors.
//
// `queue.empty()` is deliberately NOT the mechanism. A queue that is empty now
// may be non-empty a microsecond later, so emptiness alone can never terminate
// a consumer; only "empty AND the producer has stopped" can. Using emptiness as
// the sole signal is the classic lost-final-message bug, and § termination of
// the Phase-2 tests pins it: a stream whose final message is pushed immediately
// before the flag must still be consumed.
//
// ---------------------------------------------------------------------------
// TWO WAYS A SESSION ENDS BADLY, and why they are not the same thing
// ---------------------------------------------------------------------------
//
// The decoder thread stops for exactly two reasons, and conflating them would
// lose real information:
//
//   MALFORMED. `feed` returns one of the `Invalid*` statuses: the bytes present
//   PROVE the frame is not a message of this protocol. Terminal immediately —
//   the verdict does not depend on any byte that has not arrived yet. No
//   resynchronisation is attempted, because `consumed` is 0 on malformed input
//   and any resync point would be a guess.
//
//   TRUNCATED. `feed` returned `Ok` for every chunk, then the source reported
//   EOF while the framer still held a partial frame. Nothing is malformed — the
//   session simply ended mid-frame. This is reported by `framer.finish()`, and
//   ONLY at EOF, because mid-stream a retained prefix is the ordinary state of a
//   framer waiting for the rest of a message.
//
// `NeedMoreData` therefore means two different things depending on where it is
// observed, and the distinction is the whole point of the finalization call:
//
//     during streaming   ordinary. Retain the prefix, await the next chunk.
//                        NOT an error, and the common case on a socket.
//     at declared EOF    the session ended mid-frame. `terminal_error` is TRUE
//                        and the status is `NeedMoreData`.
//
// The valid prefix is unaffected either way. Everything decoded before the
// truncated frame was already pushed and is drained normally; the partial frame
// itself is never published, so it appears in no count and in no book state.
//
// ---------------------------------------------------------------------------
// BACKPRESSURE: no message is ever dropped
// ---------------------------------------------------------------------------
//
// Every decoded message is pushed with retry-until-success. A full queue makes
// the producer spin, never discard, because a dropped inbound message is not a
// lost update — it is a lost SEQUENCE NUMBER, which the pipeline downstream can
// only read as a gap, forcing a full snapshot recovery the feed never asked
// for. The retry policy is Experiment 02's, unchanged:
//
//     on failed push:  ++producer_full_retries; ++consecutive
//                      after 1024 consecutive failures: yield, reset
//     on success:      reset consecutive
//
// and the consumer mirrors it with `consumer_empty_retries`. Those counters are
// DIAGNOSTIC CORRECTNESS CONTEXT, not a performance result: nothing here is
// timed, and no retry count in this phase is a throughput or latency claim.
// ---------------------------------------------------------------------------

namespace llmd {

// Experiment 02's yield threshold, reused verbatim. Consecutive, not cumulative:
// a successful operation ends the run, so a producer that is keeping up never
// yields however many times it retried over the course of a session.
inline constexpr std::uint64_t kYieldAfterMisses = 1024;

// Statistics owned by the DECODER thread. Plain integers, not atomics: they are
// written by exactly one thread and read by the caller only after `run` has
// joined it, where the join itself is the synchronization edge.
struct MdProducerStats {
    std::uint64_t decoded_messages = 0;  // complete messages the framer produced
    std::uint64_t enqueued_messages = 0; // ...of which these reached the queue
    std::uint64_t producer_full_retries = 0;
    DecodeStatus  terminal_decode_status = DecodeStatus::Ok;
    bool          terminal_error = false; // the session ended badly: malformed
                                          // bytes, or truncated input at EOF
};

// Statistics owned by the BOOK thread. Same reasoning.
struct MdConsumerStats {
    std::uint64_t consumed_messages = 0;
    std::uint64_t consumer_empty_retries = 0;
};

template <std::size_t Capacity>
class ThreadedMdPipeline {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "the SPSC ring buffer requires a power-of-two capacity");

public:
    using Queue    = lltl::SpscSeparatedBaselineRingBuffer<MdMessage, Capacity>;
    using Book     = llob::FlatOrderBook;
    using Pipeline = MarketDataPipeline<Book>;

    ThreadedMdPipeline() = default;

    ThreadedMdPipeline(const ThreadedMdPipeline&) = delete;
    ThreadedMdPipeline& operator=(const ThreadedMdPipeline&) = delete;

    // Run one session to completion: spawn both threads, join both.
    //
    // `src` is the chunk source, called only from the decoder thread, and must
    // expose `next() -> std::span<const std::byte>` returning an empty span when
    // the session is exhausted. The source owns the byte storage and the chunk
    // boundaries; this class owns neither, so a chunk plan is a property of the
    // caller rather than of the pipeline.
    //
    // Call once per object.
    template <class ChunkSource>
    void run(ChunkSource& src) {
        std::thread decoder([this, &src] { decoder_thread(src); });
        std::thread book([this] { book_thread(); });
        decoder.join();
        book.join();
    }

    // ---- results, valid only AFTER run() has returned -----------------------

    [[nodiscard]] const MdProducerStats& producer_stats() const noexcept { return producer_; }
    [[nodiscard]] const MdConsumerStats& consumer_stats() const noexcept { return consumer_; }

    [[nodiscard]] const Pipeline& pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] MdState state() const noexcept { return pipeline_.state(); }
    [[nodiscard]] std::uint64_t expected() const noexcept { return pipeline_.expected(); }
    [[nodiscard]] std::uint64_t last_applied_seq() const noexcept {
        return pipeline_.book().last_applied_seq();
    }
    [[nodiscard]] std::int64_t best_bid() const noexcept { return pipeline_.book().best_bid(); }
    [[nodiscard]] std::int64_t best_ask() const noexcept { return pipeline_.book().best_ask(); }
    [[nodiscard]] std::size_t level_count() const noexcept {
        return pipeline_.book().level_count();
    }
    [[nodiscard]] const MdCounters& counters() const noexcept { return pipeline_.counters(); }

    // ---- TEST-ONLY harness control -----------------------------------------
    //
    // These two exist so a test can make backpressure deterministic instead of
    // sleeping and hoping. Neither is part of the production algorithm: with no
    // gate set and no reader, the pipeline runs identically without them.

    // When set, the book thread spins on `*gate` before its first pop, so a test
    // can hold the consumer still while the producer fills a small ring. Pass
    // nullptr (the default) for normal operation.
    void set_consumer_gate(const std::atomic<bool>* gate) noexcept {
        consumer_gate_ = gate;
    }

    // Set by the producer the first time a push finds the ring full — i.e. the
    // gate above has done its job and the ring is genuinely at capacity. A test
    // waits on THIS rather than on a timer before releasing the gate.
    [[nodiscard]] bool producer_saw_full() const noexcept {
        return producer_saw_full_.load(std::memory_order_acquire);
    }

private:
    template <class ChunkSource>
    void decoder_thread(ChunkSource& src) {
        StreamDecoder framer;
        std::uint64_t consecutive = 0;

        auto sink = [&](const MdMessage& m) noexcept {
            ++producer_.decoded_messages;
            while (!queue_.try_push(m)) {
                ++producer_.producer_full_retries;
                if (!producer_saw_full_.load(std::memory_order_relaxed)) {
                    producer_saw_full_.store(true, std::memory_order_release);
                }
                if (++consecutive >= kYieldAfterMisses) {
                    consecutive = 0;
                    std::this_thread::yield();
                }
            }
            consecutive = 0; // success ends the consecutive-failure run
            ++producer_.enqueued_messages;
        };

        DecodeStatus status = DecodeStatus::Ok;
        for (;;) {
            const std::span<const std::byte> chunk = src.next();
            if (chunk.empty()) {
                // Session exhausted. The framer may still be holding a partial
                // frame, and `feed` has no way to say so — a retained prefix is
                // `Ok` from its point of view, because mid-stream it is ordinary.
                // Only here, where the source is known to be finished, does the
                // carry mean anything: a non-empty one is TRUNCATED INPUT.
                //
                // Without this call the retained bytes are dropped on the floor
                // and the session reports a clean success, which is exactly the
                // defect this finalization closes.
                status = framer.finish();
                break;
            }
            status = framer.feed(chunk, sink);
            if (status != DecodeStatus::Ok) {
                break; // terminal: malformed bytes, stop accepting input
            }
        }

        producer_.terminal_decode_status = status;
        producer_.terminal_error = (status != DecodeStatus::Ok);

        // The completion publication. Everything this thread will ever push is
        // sequenced before this store. See the happens-before note above.
        producer_done_.store(true, std::memory_order_release);
    }

    void book_thread() {
        if (consumer_gate_ != nullptr) {
            // TEST-ONLY: hold the consumer still until the test releases it.
            while (!consumer_gate_->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }

        std::uint64_t consecutive = 0;
        MdMessage m{};
        for (;;) {
            if (queue_.try_pop(m)) {
                consecutive = 0; // success ends the consecutive-miss run
                pipeline_.apply(m);
                ++consumer_.consumed_messages;
                continue;
            }

            ++consumer_.consumer_empty_retries;
            if (++consecutive >= kYieldAfterMisses) {
                consecutive = 0;
                std::this_thread::yield();
            }

            if (producer_done_.load(std::memory_order_acquire)) {
                // The producer has stopped and will never enqueue again, so one
                // final pop is decisive: it either takes the last message or
                // proves there is none. `try_pop` failing here is not a race
                // that more spinning could win.
                if (queue_.try_pop(m)) {
                    pipeline_.apply(m);
                    ++consumer_.consumed_messages;
                    continue;
                }
                break;
            }
            // Producer still running: the ring being empty is transient.
        }
    }

    Queue queue_{};

    // The pipeline is constructed here, on the caller's thread, and then written
    // ONLY by the book thread until join. It is not touched by the caller during
    // the run, which is what keeps the book single-writer.
    Pipeline pipeline_{};

    MdProducerStats producer_{};
    MdConsumerStats consumer_{};

    std::atomic<bool> producer_done_{false};

    const std::atomic<bool>* consumer_gate_ = nullptr;
    std::atomic<bool> producer_saw_full_{false};
};

} // namespace llmd
