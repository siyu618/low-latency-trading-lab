#pragma once

#include "md_message.h"
#include "md_wire_protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>

// ---------------------------------------------------------------------------
// Experiment 03 Phase 1B — the byte-stream decoder.
//
//   raw bytes -> decode_one() -> llmd::MdMessage -> MarketDataPipeline
//
// `decode_one` reads AT MOST ONE message from the FRONT of a byte span. The
// caller advances by `outcome.consumed` and calls again. It is a byte-stream
// decoder, NOT a one-packet-per-call parser:
//
//     decode_one([message1][message2][message3]) -> message1, consumed=12
//     decode_one([message2][message3])           -> message2, consumed=29
//     decode_one([message3])                     -> message3, consumed=12
//
// Extra bytes after a complete first message are therefore NOT `InvalidLength`.
// A decoder that demanded the span hold exactly one message could not be fed by
// a socket read, which is the entire point of having one.
//
// IT DOES NOT DECIDE SEQUENCING. This decoder answers "are these bytes a
// well-formed message of this protocol"; it has no opinion about whether the
// message is in order, whether it is a duplicate, or whether its price is one
// the book trades. Those are the pipeline's question and the book's question
// respectively. See the three-layer note at the top of md_wire_protocol.h.
//
// The one sequence-shaped thing it DOES enforce is the FIELD DOMAIN: 0 and
// UINT64_MAX are reserved and rejected as `InvalidSequence`, because the
// sequencer downstream computes `seq + 1` and a reserved value would wrap it.
// That is a statement about which numbers are representable at all, not about
// whether a given number is next — the same kind of check as `side`, and it is
// the reason the frozen sequencer's documented precondition holds for every
// message this decoder can produce.
//
// HOT-PATH CONTRACT. `decode_one` is:
//
//   * allocation-free — it writes into a caller-provided `MdMessage` and
//     constructs no string, vector or other owning object;
//   * bounds-checked — every read is behind an explicit size test, and the
//     documented `consumed` is the ONLY thing the caller may trust;
//   * exception-free on the normal path — `noexcept`, and failures are returned
//     as statuses rather than thrown.
//
// `MdMessage` stays a value type: it is assigned once, at the end, and only on
// success.
// ---------------------------------------------------------------------------

namespace llmd {

// ---------------------------------------------------------------------------
// Why a decode attempt did not produce a message.
//
// `NeedMoreData` is NOT an error. It is the ordinary answer for a partial read,
// and it is the only status a caller should treat as "call me again with more
// bytes". Every other non-Ok status is a terminal property of the bytes
// themselves: re-reading them will not help, and the caller must resynchronise.
//
// The rejected alternatives each need stating because they look simpler:
//
//   * ONE `Invalid` status would collapse "wait for more" into "give up", which
//     is the difference between a decoder that works on a socket and one that
//     discards valid data whenever a read lands mid-message.
//   * Folding `InvalidSide` / `InvalidPrice` / `InvalidQuantity` into one
//     `Malformed` would make a venue-side bug indistinguishable from corruption
//     in the counters an operator reads. They are cheap to separate and the
//     separation is the whole reason this decoder reports statuses at all.
// ---------------------------------------------------------------------------
enum class DecodeStatus : std::uint8_t {
    Ok,               // exactly one message decoded; `consumed` is its size
    NeedMoreData,     // the span is a strict prefix of a message; read again
    InvalidVersion,   // header version is not kVersion
    InvalidType,      // message_type is not one of the three known IDs
    InvalidLength,    // payload_length disagrees with the message_type
    InvalidSequence,  // header sequence is outside [kMinSequence, kMaxSequence]
    InvalidSide,      // Level side is neither Bid nor Ask
    InvalidPrice,     // Level price_tick <= 0
    InvalidQuantity,  // Level quantity < 0
};

inline const char* decode_status_name(DecodeStatus s) noexcept {
    switch (s) {
        case DecodeStatus::Ok:              return "Ok";
        case DecodeStatus::NeedMoreData:    return "NeedMoreData";
        case DecodeStatus::InvalidVersion:  return "InvalidVersion";
        case DecodeStatus::InvalidType:     return "InvalidType";
        case DecodeStatus::InvalidLength:   return "InvalidLength";
        case DecodeStatus::InvalidSequence: return "InvalidSequence";
        case DecodeStatus::InvalidSide:     return "InvalidSide";
        case DecodeStatus::InvalidPrice:    return "InvalidPrice";
        case DecodeStatus::InvalidQuantity: return "InvalidQuantity";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The result of one decode attempt.
//
// THE INVARIANT, and the reason this is a struct rather than a bare status:
//
//     consumed != 0   if and only if   status == Ok
//
// On `Ok`, `consumed` is EXACTLY the encoded size of the message that was
// decoded (12 for a bracket, 29 for a Level) — never more, so the caller can
// advance without re-deriving the length, and never less, so a caller that
// advances by it cannot stall on a message it already consumed.
//
// On EVERY failure — `NeedMoreData` included — `consumed` is 0, and the `out`
// parameter passed to `decode_one` is untouched. A partial message is never
// published. This matters more than it looks: a caller that trusted a partially
// written `MdMessage` would apply half a level update to a live book, and the
// `NeedMoreData` case is the one that happens constantly on a real socket.
// ---------------------------------------------------------------------------
struct DecodeOutcome {
    DecodeStatus status = DecodeStatus::NeedMoreData;
    std::size_t  consumed = 0;
};

// ---------------------------------------------------------------------------
// Decode at most one message from the front of `input` into `out`.
//
// `out` is written ONLY on success. On any non-Ok return it is left exactly as
// the caller passed it, so a caller that loops on a reused `MdMessage` cannot
// pick up a stale or half-written value.
//
// Allocation-free, bounds-checked, noexcept.
// ---------------------------------------------------------------------------
[[nodiscard]] inline DecodeOutcome decode_one(std::span<const std::byte> input,
                                              MdMessage& out) noexcept {
    using namespace llmd::wire;

    // A header is the smallest thing that can carry a status. Anything shorter
    // is a prefix, and a prefix is not evidence of a malformed message.
    if (input.size() < kHeaderSize) {
        return DecodeOutcome{DecodeStatus::NeedMoreData, 0};
    }

    const std::byte* p = input.data();

    const std::uint8_t type = std::to_integer<std::uint8_t>(p[kOffsetType]);
    const std::uint8_t version = std::to_integer<std::uint8_t>(p[kOffsetVersion]);
    const std::uint16_t payload_length = read_u16_be(p + kOffsetPayloadLength);
    const std::uint64_t sequence = read_u64_be(p + kOffsetSequence);

    // Version first. It is the field that says how to read everything else, so
    // nothing after it is trusted until it is known to be ours.
    if (version != kVersion) {
        return DecodeOutcome{DecodeStatus::InvalidVersion, 0};
    }

    // The sequence domain, checked here because it is a HEADER field common to
    // every message type — like `version`, it is validated before the type is
    // consulted, so a frame with both a bad sequence and a bad type reports the
    // sequence. Like the payload_length check below, it is decided on the header
    // ALONE: those 8 bytes are already fully present, so a frame carrying a
    // reserved sequence is definitively malformed whatever arrives next, and
    // answering `NeedMoreData` would ask the caller to wait for bytes that can
    // only ever produce this same rejection.
    //
    // This is a FIELD-DOMAIN rule, not a sequencing rule. It says a sequence is
    // representable in a stream, never that it is the right one for this stream.
    // Stale, duplicate, gap and snapshot-continuity remain the pipeline's.
    if (sequence < kMinSequence || sequence > kMaxSequence) {
        return DecodeOutcome{DecodeStatus::InvalidSequence, 0};
    }

    switch (static_cast<MessageType>(type)) {
        case MessageType::SnapshotBegin:
            if (payload_length != 0) {
                return DecodeOutcome{DecodeStatus::InvalidLength, 0};
            }
            out = md_begin(sequence);
            return DecodeOutcome{DecodeStatus::Ok, kHeaderSize};

        case MessageType::SnapshotEnd:
            if (payload_length != 0) {
                return DecodeOutcome{DecodeStatus::InvalidLength, 0};
            }
            out = md_end(sequence);
            return DecodeOutcome{DecodeStatus::Ok, kHeaderSize};

        case MessageType::Level: {
            // The declared length is checked BEFORE waiting for bytes. A frame
            // that claims the wrong length is malformed whatever arrives next,
            // and waiting for a length that will never be satisfiable would let
            // a single corrupt header stall the decoder indefinitely.
            if (payload_length != kLevelPayloadSize) {
                return DecodeOutcome{DecodeStatus::InvalidLength, 0};
            }
            if (input.size() < kLevelMessageSize) {
                return DecodeOutcome{DecodeStatus::NeedMoreData, 0};
            }

            const std::byte* lp = p + kHeaderSize;
            // Compared as its raw byte so that an out-of-range value cannot be
            // read back out of a scoped enum first.
            const std::uint8_t raw_side =
                std::to_integer<std::uint8_t>(lp[kLevelOffsetSide]);
            const std::int64_t price = read_i64_be(lp + kLevelOffsetPrice);
            const std::int64_t quantity = read_i64_be(lp + kLevelOffsetQuantity);

            if (raw_side > static_cast<std::uint8_t>(WireSide::Ask)) {
                return DecodeOutcome{DecodeStatus::InvalidSide, 0};
            }
            // price_tick <= 0 is structurally invalid: a tick index is a
            // POSITION in the book's price domain, not a price, so zero and
            // negatives are not "a very cheap instrument" — they are not
            // positions at all. Note what is NOT checked here: whether the
            // positive tick is inside the book's configured range. That is the
            // book's question, and a positive out-of-range tick is wire-valid.
            if (price <= 0) {
                return DecodeOutcome{DecodeStatus::InvalidPrice, 0};
            }
            // quantity == 0 is a delete, which is well formed. Negative is not.
            if (quantity < 0) {
                return DecodeOutcome{DecodeStatus::InvalidQuantity, 0};
            }

            const llob::Side side =
                raw_side == static_cast<std::uint8_t>(WireSide::Bid) ? llob::Side::Bid
                                                                     : llob::Side::Ask;
            out = md_level(sequence, side, price, quantity);
            return DecodeOutcome{DecodeStatus::Ok, kLevelMessageSize};
        }
    }

    // Every declared case returned above, so reaching here means `type` held a
    // value no case matched. Casting a raw byte to the enum is what makes that
    // possible, and it is deliberate: the wire can carry any of 256 values.
    return DecodeOutcome{DecodeStatus::InvalidType, 0};
}

} // namespace llmd
