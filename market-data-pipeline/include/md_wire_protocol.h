#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Experiment 03 Phase 1B — the synthetic wire protocol.
//
// This is a DELIBERATELY SIMPLE, synthetic protocol. It is not FIX, not ITCH,
// not SBE, and not modelled on any venue. It exists so that the decoder has a
// byte-level contract that is written down exactly, byte for byte, and can be
// tested against literal constants rather than against itself.
//
// LAYOUT. Every message is a fixed 12-byte header followed by an optional
// payload. All multi-byte integers are BIG ENDIAN, two's complement for signed
// fields. Nothing here is host-endian dependent, and no field is read through a
// pointer cast.
//
//   header, 12 bytes, byte offsets from the start of the message
//   ---------------------------------------------------------------
//     0        uint8   message_type          1 = SnapshotBegin
//                                           2 = Level
//                                           3 = SnapshotEnd
//     1        uint8   version               exactly 1
//     2..3     uint16  payload_length        big endian
//     4..11    uint64  sequence              big endian
//
//   Level payload, exactly 17 bytes, byte offsets from the start of the PAYLOAD
//   ---------------------------------------------------------------
//     0        uint8   side                  0 = Bid, 1 = Ask
//     1..8     int64   price_tick            big endian, must be > 0
//     9..16    int64   quantity              big endian, must be >= 0
//
//   message total sizes
//   ---------------------------------------------------------------
//     SnapshotBegin   12 bytes   payload_length must be 0
//     SnapshotEnd     12 bytes   payload_length must be 0
//     Level           29 bytes   payload_length must be 17
//
// WHY THE LAYOUT IS EXPLICIT RATHER THAN A STRUCT. The wire format is an
// external contract, so it is written as byte offsets and assembled by hand.
// A `struct` would have made the wire ABI a property of the compiler: padding,
// alignment and member order are implementation details that no protocol should
// inherit, and a packed struct would trade that for unaligned loads that are
// undefined behaviour on strict-alignment targets. The header is the contract;
// the struct is not.
//
// THREE LAYERS OF VALIDITY, and this file is only the first:
//
//   wire validity      the bytes are a well-formed message of this protocol
//                      (this file and md_decoder.h). A positive price OUTSIDE
//                      the order book's tick range is still wire-valid.
//   sequence validity  the message is in order for the stream it belongs to
//                      (market_data_pipeline.h). A wire-valid message can be
//                      Stale, or open a gap.
//   book validity      the message is applicable to the book's price domain
//                      and content rules (types.h). A sequence-valid message
//                      can still be OutOfRange or InvalidUpdate.
//
// Each layer rejects things the others accept. A decoder that also enforced the
// tick range would be reaching across the seam, and the pipeline would lose the
// ability to distinguish "the venue sent me a price I do not trade" from "the
// venue sent me garbage".
// ---------------------------------------------------------------------------

namespace llmd::wire {

// ---------------------------------------------------------------------------
// The single protocol version this decoder accepts. A frame carrying anything
// else is InvalidVersion, not an unknown-message-type problem: the version
// field tells us how to read the REST of the header, so it is checked before
// any other field is trusted.
// ---------------------------------------------------------------------------
inline constexpr std::uint8_t kVersion = 1;

// ---- field widths and offsets ---------------------------------------------

inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kLevelPayloadSize = 17;
inline constexpr std::size_t kLevelMessageSize = kHeaderSize + kLevelPayloadSize;

inline constexpr std::size_t kOffsetType = 0;
inline constexpr std::size_t kOffsetVersion = 1;
inline constexpr std::size_t kOffsetPayloadLength = 2; // 2..3
inline constexpr std::size_t kOffsetSequence = 4;      // 4..11

inline constexpr std::size_t kLevelOffsetSide = 0;
inline constexpr std::size_t kLevelOffsetPrice = 1;     // 1..8
inline constexpr std::size_t kLevelOffsetQuantity = 9;  // 9..16

// Message type IDs, as they appear on the wire. Deliberately a separate enum
// from `llmd::MdKind`: the wire ID is an external number that must not move if
// someone reorders the internal enumeration.
enum class MessageType : std::uint8_t {
    SnapshotBegin = 1,
    Level = 2,
    SnapshotEnd = 3,
};

// Side IDs, as they appear on the wire. Same reasoning as MessageType.
enum class WireSide : std::uint8_t {
    Bid = 0,
    Ask = 1,
};

// ---------------------------------------------------------------------------
// Big-endian field access.
//
// Every helper takes a pointer the CALLER has already bounds-checked. They do
// no bounds checking of their own, by design: the decoder checks once, up
// front, and then reads; a second redundant check per field would be noise on
// the one path that has to be cheap.
//
// Bytes are assembled explicitly. There is no `reinterpret_cast` to an integer
// pointer, no packed struct, and no unaligned load — all three are either
// undefined or implementation-defined, and all three would silently make the
// wire format a property of the host.
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::uint16_t read_u16_be(const std::byte* p) noexcept {
    const std::uint32_t hi = std::to_integer<std::uint8_t>(p[0]);
    const std::uint32_t lo = std::to_integer<std::uint8_t>(p[1]);
    return static_cast<std::uint16_t>((hi << 8) | lo);
}

[[nodiscard]] inline std::uint64_t read_u64_be(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
    }
    return v;
}

// C++20 guarantees two's complement, so reinterpreting the 64-bit pattern as a
// signed value is well defined. This is a conversion, not a memcpy, and it does
// not assume anything about the host: the bytes were already assembled in
// big-endian order by read_u64_be.
[[nodiscard]] inline std::int64_t read_i64_be(const std::byte* p) noexcept {
    return static_cast<std::int64_t>(read_u64_be(p));
}

inline void write_u16_be(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    p[1] = static_cast<std::byte>(static_cast<std::uint8_t>(v & 0xFFu));
}

inline void write_u64_be(std::byte* p, std::uint64_t v) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t shift = (7 - i) * 8;
        p[i] = static_cast<std::byte>(
            static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

inline void write_i64_be(std::byte* p, std::int64_t v) noexcept {
    write_u64_be(p, static_cast<std::uint64_t>(v));
}

} // namespace llmd::wire
