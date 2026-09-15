#pragma once

#include "md_decoder.h"
#include "md_message.h"
#include "md_wire_protocol.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

// ---------------------------------------------------------------------------
// Experiment 03 Phase 2 — stream framing.
//
//   arbitrary byte chunks -> zero or more complete MdMessage values
//                         + at most one retained partial frame
//
// Phase 1B's `decode_one` answers a narrower question than a socket asks. It
// reads one message from the FRONT of a span and returns `NeedMoreData` for a
// prefix — but it holds no state, so it cannot itself remember that prefix
// across calls. A caller looping over socket reads therefore needs one more
// thing: somewhere to keep the tail of a frame that got cut in half.
//
// This header is that thing and nothing else. It does the framing; it does NOT
// parse. Every complete frame it assembles is handed to the frozen
// `decode_one()`, so there is exactly one implementation of the binary layout in
// the repository and no second opinion about byte order, field widths or the
// sequence domain.
//
// WHY A FIXED CARRY. The largest message the protocol defines is
// `kLevelMessageSize` (29 bytes), so a partial frame is at most
// `kLevelMessageSize - 1` bytes and one `std::array<std::byte, 29>` holds every
// prefix that can exist. A growing receive buffer would allocate on the steady
// path and would put an unbounded, remotely-driven size in the hot loop for no
// benefit; the frame ceiling makes it unnecessary. The steady state — a chunk
// that begins and ends on frame boundaries — copies nothing at all.
//
// THE TWO PATHS, and why both exist:
//
//   * carry empty  -> decode_directly from the chunk at the current offset. The
//     common case, and the one that must not copy.
//   * carry non-empty -> top the carry up from the chunk and decode that. The
//     carry is only refilled, never grown past the frame ceiling, and whatever
//     a successful decode does not consume is shifted down for the next frame.
//
// ERROR POLICY. `feed` returns `Ok`, or the terminal status that ended the
// session. It NEVER returns `NeedMoreData`: a partial frame is not a failure
// here, it is the retained carry, which is the entire reason this component
// exists. A terminal status means the bytes are malformed and the caller must
// resynchronise — which this phase deliberately does not implement, because
// `consumed` is 0 on malformed input and skipping an arbitrary byte without a
// framing-resynchronisation protocol would be guessing. See the Phase-2 note in
// docs/MARKET_DATA_PIPELINE.md.
//
// FINALIZATION, and why `feed` alone cannot report truncation. Because a
// retained prefix is `Ok` from `feed`'s point of view, a stream that simply
// STOPS mid-frame would otherwise look exactly like a clean one: the caller
// stops calling, the carry still holds bytes, and nothing in the return value
// ever said so. `feed` cannot raise this itself — it has no way to know whether
// more bytes are coming, and inventing an error for a partial frame would break
// the ordinary chunk-boundary case, which is the common one on a socket.
//
// So the finalization step is EXPLICIT and belongs to whoever knows the session
// is over. `finish()` answers one question: is the decoder at a frame boundary?
// The caller asks it when the byte source is exhausted, and only then.
//
//   mid-stream, carry non-empty   -> ordinary, means "send me the rest"
//   at EOF,      carry non-empty   -> the session ended mid-frame: TRUNCATED
//
// Both are `NeedMoreData` from this component, because both are literally the
// same state — retained bytes with no complete frame. The meaning differs only
// by what the caller knows about the byte source, which is exactly why the
// caller, not this class, decides. See `finish()` below.
// ---------------------------------------------------------------------------

namespace llmd {

class StreamDecoder {
public:
    // The largest frame the protocol defines, and therefore the smallest buffer
    // that can hold any partial one.
    static constexpr std::size_t kCarryCapacity = wire::kLevelMessageSize;

    StreamDecoder() noexcept = default;

    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    // Consume one chunk, invoking `sink(message)` for each COMPLETE message in
    // order. `sink` is a forwarding reference so that a caller can pass a lambda
    // expression directly, which is the natural call shape at every use site;
    // it is called only from this thread, and on the hot path it must be
    // noexcept and allocation-free, because `feed` is.
    //
    // Returns `DecodeStatus::Ok` when the chunk was consumed — note that this
    // includes the case where a partial frame is now retained — or the terminal
    // status that ended the session, in which case the carry is cleared and the
    // StreamDecoder must not be fed again.
    //
    // Allocation-free, bounds-checked, noexcept, no state but the carry.
    template <class Sink>
    [[nodiscard]] DecodeStatus feed(std::span<const std::byte> chunk,
                                    Sink&& sink) noexcept {
        std::size_t off = 0;
        for (;;) {
            if (carry_size_ == 0) {
                if (off == chunk.size()) {
                    return DecodeStatus::Ok; // chunk exhausted, nothing retained
                }

                // Steady-state path: decode straight out of the chunk, no copy.
                MdMessage m{};
                const DecodeOutcome oc = decode_one(chunk.subspan(off), m);
                if (oc.status == DecodeStatus::Ok) {
                    sink(m);
                    off += oc.consumed;
                    continue;
                }
                if (oc.status != DecodeStatus::NeedMoreData) {
                    return oc.status; // terminal, nothing retained
                }

                // A strict prefix. `decode_one` only answers NeedMoreData for a
                // span shorter than a header, or a Level shorter than a whole
                // one, so this is at most kLevelMessageSize - 1 bytes and the
                // carry always fits. Bounds-checked above, stated here.
                const std::size_t rem = chunk.size() - off;
                std::memcpy(carry_.data(), chunk.data() + off, rem);
                carry_size_ = rem;
                return DecodeStatus::Ok;
            }

            // The carry holds a partial frame. Nothing to add means nothing to
            // do: the rest of this frame is in a chunk that has not arrived.
            if (off == chunk.size()) {
                return DecodeStatus::Ok;
            }

            const std::size_t want = kCarryCapacity - carry_size_;
            const std::size_t avail = chunk.size() - off;
            const std::size_t take = want < avail ? want : avail;
            std::memcpy(carry_.data() + carry_size_, chunk.data() + off, take);
            carry_size_ += take;
            off += take;

            MdMessage m{};
            const DecodeOutcome oc =
                decode_one(std::span<const std::byte>(carry_.data(), carry_size_), m);
            if (oc.status == DecodeStatus::Ok) {
                sink(m);
                // A bracket is 12 bytes and the carry may hold up to 29, so a
                // successful decode can leave the head of the NEXT frame behind.
                // That residue is already-in-hand stream data, not waste: shift
                // it down and continue from it.
                const std::size_t leftover = carry_size_ - oc.consumed;
                if (leftover != 0) {
                    std::memmove(carry_.data(), carry_.data() + oc.consumed, leftover);
                }
                carry_size_ = leftover;
                continue;
            }
            if (oc.status != DecodeStatus::NeedMoreData) {
                carry_size_ = 0; // terminal: drop the partial frame
                return oc.status;
            }
            // Still a prefix. Loop: if the chunk has more bytes, top up again
            // (this is how a bracket followed by a partial Level is handled);
            // otherwise the `off == chunk.size()` guard above returns.
        }
    }

    // Finalize a stream: report whether the decoder ended on a frame boundary.
    //
    // Call this when — and only when — the byte source is exhausted. It is the
    // caller's statement "no more bytes are coming", and it is what turns a
    // retained partial frame from an ordinary wait into evidence of TRUNCATED
    // INPUT. See the finalization note at the top of this header.
    //
    //   carry_size() == 0  ->  Ok            the stream ended cleanly
    //   carry_size() != 0  ->  NeedMoreData  the stream ended mid-frame
    //
    // `NeedMoreData` is the honest answer rather than a new status: those bytes
    // ARE a strict prefix of a message, and the only thing that makes them an
    // error is that nothing more will ever arrive. `decode_one` answers the same
    // way for the same bytes, so the two agree.
    //
    // It does NOT clear the carry and it does NOT parse anything. A
    // finalization that silently dropped the partial frame and returned `Ok`
    // would be the exact bug this exists to prevent: it would convert a
    // truncated session into a clean one, and the discarded bytes would be
    // invisible in every count and every log.
    //
    // const, noexcept, allocation-free. Calling it before the source is
    // exhausted, or calling it twice, changes nothing — it reads one field.
    [[nodiscard]] DecodeStatus finish() const noexcept {
        return carry_size_ == 0 ? DecodeStatus::Ok : DecodeStatus::NeedMoreData;
    }

    // Bytes retained from an incomplete frame. Always < kCarryCapacity at rest,
    // because kCarryCapacity bytes can only ever be a complete frame or a
    // terminal error, both of which leave the carry empty.
    [[nodiscard]] std::size_t carry_size() const noexcept { return carry_size_; }
    [[nodiscard]] bool has_partial_frame() const noexcept { return carry_size_ != 0; }

private:
    std::array<std::byte, kCarryCapacity> carry_{};
    std::size_t carry_size_ = 0;
};

} // namespace llmd
