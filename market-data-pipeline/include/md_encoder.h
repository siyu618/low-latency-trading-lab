#pragma once

#include "md_message.h"
#include "md_wire_protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// ---------------------------------------------------------------------------
// Experiment 03 Phase 1B — the wire encoder.
//
// This is a TEST AND FIXTURE encoder, not the hot path. It allocates, it takes
// no care to be fast, and it is deliberately written the straightforward way.
// Its one job is to produce the EXACT byte layout documented in
// md_wire_protocol.h, so that a decoder test can compare against literal
// constants.
//
// WHY IT EXISTS AT ALL, given that a test could hard-code every byte: most
// tests need a valid frame and only care about one field. Hard-coding 29 bytes
// per case would bury the field under test in noise. The exact-byte tests still
// use literal constants — see md_decoder_tests.cpp — so the encoder and the
// decoder are never the only two witnesses to the layout. If both agreed on a
// wrong endian convention, the literal tests would fail.
//
// `append_raw` takes every header field verbatim, which is how the malformed
// cases are built: a wrong version, an unknown type or a lying payload_length
// cannot be expressed by `append_message`, so they are expressed by assembling
// the header by hand.
// ---------------------------------------------------------------------------

namespace llmd::encode {

// Every field of a frame, set explicitly. Defaults describe a well-formed
// zero-length-payload bracket at sequence 0.
struct RawMessage {
    std::uint8_t  type = static_cast<std::uint8_t>(wire::MessageType::SnapshotBegin);
    std::uint8_t  version = wire::kVersion;
    std::uint16_t payload_length = 0;
    std::uint64_t sequence = 0;
    std::uint8_t  side = static_cast<std::uint8_t>(wire::WireSide::Bid);
    std::int64_t  price = 0;
    std::int64_t  quantity = 0;
};

// Append the 17-byte Level payload.
inline void append_level_payload(std::vector<std::byte>& out, std::uint8_t side,
                                 std::int64_t price, std::int64_t quantity) {
    const std::size_t base = out.size();
    out.resize(base + wire::kLevelPayloadSize);
    std::byte* lp = out.data() + base;
    lp[wire::kLevelOffsetSide] = static_cast<std::byte>(side);
    wire::write_i64_be(lp + wire::kLevelOffsetPrice, price);
    wire::write_i64_be(lp + wire::kLevelOffsetQuantity, quantity);
}

// Append a 12-byte header with every field taken verbatim, then the 17-byte
// Level payload if and only if the header declares a Level of exactly that
// length. A frame whose declared length and actual bytes disagree is built by
// calling the two halves separately.
inline void append_raw(std::vector<std::byte>& out, const RawMessage& r) {
    const std::size_t base = out.size();
    out.resize(base + wire::kHeaderSize);
    std::byte* p = out.data() + base;
    p[wire::kOffsetType] = static_cast<std::byte>(r.type);
    p[wire::kOffsetVersion] = static_cast<std::byte>(r.version);
    wire::write_u16_be(p + wire::kOffsetPayloadLength, r.payload_length);
    wire::write_u64_be(p + wire::kOffsetSequence, r.sequence);

    const bool well_formed_level =
        r.type == static_cast<std::uint8_t>(wire::MessageType::Level) &&
        r.payload_length == static_cast<std::uint16_t>(wire::kLevelPayloadSize);
    if (well_formed_level) {
        append_level_payload(out, r.side, r.price, r.quantity);
    }
}

// Map a value message onto the wire fields it encodes to.
[[nodiscard]] inline RawMessage raw_of(const MdMessage& m) {
    RawMessage r;
    r.sequence = m.seq;
    switch (m.kind) {
        case MdKind::SnapshotBegin:
            r.type = static_cast<std::uint8_t>(wire::MessageType::SnapshotBegin);
            r.payload_length = 0;
            break;
        case MdKind::SnapshotEnd:
            r.type = static_cast<std::uint8_t>(wire::MessageType::SnapshotEnd);
            r.payload_length = 0;
            break;
        case MdKind::Level:
            r.type = static_cast<std::uint8_t>(wire::MessageType::Level);
            r.payload_length = static_cast<std::uint16_t>(wire::kLevelPayloadSize);
            r.side = llob::is_bid(m.side)
                         ? static_cast<std::uint8_t>(wire::WireSide::Bid)
                         : static_cast<std::uint8_t>(wire::WireSide::Ask);
            r.price = m.price;
            r.quantity = m.qty;
            break;
    }
    return r;
}

// Append the well-formed encoding of one message.
inline void append_message(std::vector<std::byte>& out, const MdMessage& m) {
    append_raw(out, raw_of(m));
}

// A fresh buffer holding exactly one encoded message.
[[nodiscard]] inline std::vector<std::byte> encode(const MdMessage& m) {
    std::vector<std::byte> out;
    append_message(out, m);
    return out;
}

// A fresh buffer holding a contiguous run of encoded messages — the shape a
// caller actually reads off a socket.
[[nodiscard]] inline std::vector<std::byte> encode_all(
    std::span<const MdMessage> messages) {
    std::vector<std::byte> out;
    for (const MdMessage& m : messages) {
        append_message(out, m);
    }
    return out;
}

} // namespace llmd::encode
