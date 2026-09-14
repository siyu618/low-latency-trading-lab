// Experiment 03 Phase 1B — wire protocol decoder correctness tests.
//
// The decoder is the layer that turns bytes into `MdMessage`, and the thing
// most likely to be wrong in it is the one thing a round-trip test cannot see:
// the byte ORDER. An encoder and a decoder written by the same hand can agree
// on a wrong endian convention and round-trip perfectly, forever.
//
// So the layout is pinned three ways, weakest to strongest:
//
//   1. Round-trip tests (encode -> decode) catch disagreement between the two
//      halves, and nothing else.
//   2. LITERAL BYTE tests compare against byte constants written out by hand
//      from md_wire_protocol.h. These are the ones that catch a shared wrong
//      convention, because they do not consult the encoder at all.
//   3. SIGNED literal bytes (price = -2, quantity = -1) pin two's-complement
//      big-endian handling of the signed fields, which is where an
//      implementation that reached for a host-endian load would show up.
//
// Everything else here is boundary coverage: every truncation length of a
// Level, every malformed field, and the stream semantics that make this a byte
// decoder rather than a one-packet-per-call parser.
//
// A plain CHECK macro reports file/line. Failed CHECKs accumulate and main()
// returns non-zero, so CTest genuinely fails on a bad run.
//
// Exit-code self-test: LLDB_SELFTEST_FAIL=1 runs only a deliberately failing
// suite and exits through the normal path, so the caller can assert non-zero.

#include "flat_order_book.h"
#include "map_order_book.h"
#include "market_data_pipeline.h"
#include "md_decoder.h"
#include "md_encoder.h"
#include "md_message.h"
#include "md_wire_protocol.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using llob::FlatOrderBook;
using llob::MapOrderBook;
using llob::Side;

using llmd::DecodeOutcome;
using llmd::DecodeStatus;
using llmd::MdMessage;
using llmd::MdOutcome;
using llmd::MdState;
using llmd::MarketDataPipeline;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

int g_failures_total = 0;

void summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("[ok] %-42s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-42s (%d/%d checks FAILED)\n", suite, g_failures, g_checks);
    }
    g_failures_total += g_failures;
    g_checks = 0;
    g_failures = 0;
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

// Build a byte buffer from literal values, so expected wire content reads as
// the hex in the protocol doc rather than as a pile of casts.
std::vector<std::byte> bytes_of(std::initializer_list<unsigned> vals) {
    std::vector<std::byte> v;
    v.reserve(vals.size());
    for (unsigned x : vals) {
        v.push_back(static_cast<std::byte>(x));
    }
    return v;
}

void dump_bytes(const std::vector<std::byte>& v) {
    std::printf("       got (%zu):", v.size());
    for (const std::byte b : v) {
        std::printf(" %02x", static_cast<unsigned>(std::to_integer<std::uint8_t>(b)));
    }
    std::printf("\n");
}

// Compare against a literal, printing both sides on failure so a wrong byte is
// visible rather than merely reported.
void check_bytes(const char* label, const std::vector<std::byte>& got,
                 const std::vector<std::byte>& want) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::printf("FAIL %s\n", label);
    std::printf("       want (%zu):", want.size());
    for (const std::byte b : want) {
        std::printf(" %02x", static_cast<unsigned>(std::to_integer<std::uint8_t>(b)));
    }
    std::printf("\n");
    dump_bytes(got);
}

bool same_message(const MdMessage& a, const MdMessage& b) {
    return a.kind == b.kind && a.seq == b.seq && a.side == b.side &&
           a.price == b.price && a.qty == b.qty;
}

// A value no decoder output could accidentally equal, used to prove that a
// failed decode did not publish anything.
MdMessage sentinel() {
    return llmd::md_level(0xDEADBEEFULL, Side::Ask, 424242, 999999);
}

// Decode with the sentinel pre-installed and assert the failure contract:
// the status matches, `consumed` is 0, and `out` came back untouched.
void expect_reject(const char* label, const std::vector<std::byte>& input,
                   DecodeStatus want) {
    MdMessage out = sentinel();
    const DecodeOutcome oc = llmd::decode_one(input, out);
    ++g_checks;
    if (oc.status != want) {
        ++g_failures;
        std::printf("FAIL %s: status %s, want %s\n", label,
                    llmd::decode_status_name(oc.status), llmd::decode_status_name(want));
    }
    ++g_checks;
    if (oc.consumed != 0) {
        ++g_failures;
        std::printf("FAIL %s: consumed %zu, want 0\n", label, oc.consumed);
    }
    ++g_checks;
    if (!same_message(out, sentinel())) {
        ++g_failures;
        std::printf("FAIL %s: out was MODIFIED on a failed decode\n", label);
    }
}

std::uint8_t type_id(llmd::wire::MessageType t) {
    return static_cast<std::uint8_t>(t);
}

// A raw header with the fields the malformed cases vary.
llmd::encode::RawMessage raw(std::uint8_t type, std::uint16_t payload_length,
                             std::uint64_t seq) {
    llmd::encode::RawMessage r;
    r.type = type;
    r.payload_length = payload_length;
    r.sequence = seq;
    return r;
}

std::vector<std::byte> raw_bytes(const llmd::encode::RawMessage& r) {
    std::vector<std::byte> v;
    llmd::encode::append_raw(v, r);
    return v;
}

MdMessage B(std::uint64_t s) { return llmd::md_begin(s); }
MdMessage E(std::uint64_t s) { return llmd::md_end(s); }
MdMessage L(std::uint64_t s, Side side, std::int64_t px, std::int64_t qty) {
    return llmd::md_level(s, side, px, qty);
}

// ---------------------------------------------------------------------------
// Suite 1 — exact bytes, against literal constants.
// ---------------------------------------------------------------------------

void suite_exact_bytes() {
    using llmd::wire::MessageType;

    // SnapshotBegin seq=1
    //   type=01 version=01 payload_length=0000 sequence=0000000000000001
    check_bytes("SnapshotBegin seq=1 literal bytes",
                llmd::encode::encode(B(1)),
                bytes_of({0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x01}));

    // SnapshotEnd seq=4
    check_bytes("SnapshotEnd seq=4 literal bytes",
                llmd::encode::encode(E(4)),
                bytes_of({0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x04}));

    // Level seq=0x0102030405060708 Bid price=100 qty=20
    // The sequence is chosen specifically so that every byte differs: a decoder
    // that reversed the 8 sequence bytes, or read only the first four, cannot
    // produce 0x0102030405060708 by accident.
    const std::vector<std::byte> level_bid_literal =
        bytes_of({0x02, 0x01, 0x00, 0x11,                          // header
                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // sequence
                  0x00,                                            // side Bid
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64,  // price 100
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14}); // qty 20
    check_bytes("Level Bid seq=0x0102030405060708 price=100 qty=20 literal bytes",
                llmd::encode::encode(L(0x0102030405060708ULL, Side::Bid, 100, 20)),
                level_bid_literal);

    // Level seq=9 Ask price=101 qty=0
    check_bytes("Level Ask seq=9 price=101 qty=0 literal bytes",
                llmd::encode::encode(L(9, Side::Ask, 101, 0)),
                bytes_of({0x02, 0x01, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // The literal above must also DECODE to the message it depicts — the byte
    // check and the decode check are separate claims.
    {
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(level_bid_literal, out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(oc.consumed == 29);
        CHECK(out.kind == llmd::MdKind::Level);
        CHECK(out.seq == 0x0102030405060708ULL);
        CHECK(out.side == Side::Bid);
        CHECK(out.price == 100);
        CHECK(out.qty == 20);
    }

    // Signed fields, as literal bytes. A quantity of -1 is all ones, and a price
    // of -2 is all ones with a trailing FE. An implementation that assembled
    // the bytes correctly but converted to signed wrongly would fail here.
    check_bytes("quantity = -1 encodes as all ones",
                llmd::encode::encode(L(1, Side::Bid, 1, -1)),
                bytes_of({0x02, 0x01, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));

    check_bytes("price = -2 encodes as ...FE",
                llmd::encode::encode(L(1, Side::Bid, -2, 0)),
                bytes_of({0x02, 0x01, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                          0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // Both are structurally invalid, and the decoder says so by status rather
    // than by producing a negative price.
    expect_reject("quantity = -1 -> InvalidQuantity",
                  llmd::encode::encode(L(1, Side::Bid, 1, -1)),
                  DecodeStatus::InvalidQuantity);
    expect_reject("price = -2 -> InvalidPrice",
                  llmd::encode::encode(L(1, Side::Bid, -2, 0)),
                  DecodeStatus::InvalidPrice);

    // Message type IDs are pinned by number, not by enum order: renaming or
    // reordering `MessageType` must not silently move the wire value.
    CHECK(type_id(MessageType::SnapshotBegin) == 1);
    CHECK(type_id(MessageType::Level) == 2);
    CHECK(type_id(MessageType::SnapshotEnd) == 3);
    CHECK(llmd::wire::kVersion == 1);
    CHECK(llmd::wire::kHeaderSize == 12);
    CHECK(llmd::wire::kLevelPayloadSize == 17);
    CHECK(llmd::wire::kLevelMessageSize == 29);

    summary("exact wire bytes");
}

// ---------------------------------------------------------------------------
// Suite 2 — the endian helpers, directly.
// ---------------------------------------------------------------------------

void suite_endian_helpers() {
    using namespace llmd::wire;

    {
        std::byte p[2] = {};
        write_u16_be(p, 0x1234);
        CHECK(std::to_integer<std::uint8_t>(p[0]) == 0x12);
        CHECK(std::to_integer<std::uint8_t>(p[1]) == 0x34);
        CHECK(read_u16_be(p) == 0x1234);
    }
    {
        std::byte p[8] = {};
        write_u64_be(p, 0x0102030405060708ULL);
        CHECK(std::to_integer<std::uint8_t>(p[0]) == 0x01);
        CHECK(std::to_integer<std::uint8_t>(p[7]) == 0x08);
        CHECK(read_u64_be(p) == 0x0102030405060708ULL);
    }
    {
        std::byte p[8] = {};
        write_i64_be(p, -2);
        CHECK(std::to_integer<std::uint8_t>(p[6]) == 0xFF);
        CHECK(std::to_integer<std::uint8_t>(p[7]) == 0xFE);
        CHECK(read_i64_be(p) == -2);
    }
    // Extremes, which is where a shift or a mask goes wrong.
    {
        std::byte p[8] = {};
        write_u64_be(p, 0xFFFFFFFFFFFFFFFFULL);
        CHECK(read_u64_be(p) == 0xFFFFFFFFFFFFFFFFULL);
        write_u64_be(p, 0);
        CHECK(read_u64_be(p) == 0);
        write_i64_be(p, -1);
        CHECK(read_i64_be(p) == -1);
        write_i64_be(p, 0);
        CHECK(read_i64_be(p) == 0);
    }

    // A round trip over a spread of values, so the helpers are exercised on
    // more than the handful of constants above.
    for (std::int64_t v = -300; v <= 300; ++v) {
        std::byte p[8] = {};
        write_i64_be(p, v);
        if (read_i64_be(p) != v) {
            ++g_failures;
            std::printf("FAIL i64 round trip at %lld\n", static_cast<long long>(v));
        }
        ++g_checks;
    }

    summary("big-endian helpers");
}

// ---------------------------------------------------------------------------
// Suite 3 — stream semantics: many messages, one span.
// ---------------------------------------------------------------------------

void suite_stream() {
    const std::vector<MdMessage> stream = {B(1), L(2, Side::Bid, 100, 10),
                                           L(3, Side::Ask, 101, 20), E(4),
                                           L(5, Side::Bid, 100, 12)};
    const std::vector<std::byte> wire = llmd::encode::encode_all(stream);

    // 12 + 29 + 29 + 12 + 29
    CHECK(wire.size() == 111);

    const std::span<const std::byte> all(wire);
    std::size_t offset = 0;
    std::vector<MdMessage> decoded;
    std::vector<std::size_t> consumed;

    while (offset < wire.size()) {
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(all.subspan(offset), out);
        if (oc.status != DecodeStatus::Ok) {
            ++g_failures;
            ++g_checks;
            std::printf("FAIL stream decode at offset %zu: %s\n", offset,
                        llmd::decode_status_name(oc.status));
            break;
        }
        ++g_checks;
        consumed.push_back(oc.consumed);
        decoded.push_back(out);
        offset += oc.consumed;
    }

    CHECK(decoded.size() == 5);
    CHECK(offset == wire.size());
    if (decoded.size() == 5) {
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(same_message(decoded[i], stream[i]));
        }
    }

    const std::vector<std::size_t> want_consumed = {12, 29, 29, 12, 29};
    CHECK(consumed == want_consumed);

    // Extra trailing bytes are not an error: a decoder fed by a socket sees
    // partial reads constantly, and "the span holds more than one message" is
    // the ordinary case, not a length violation.
    {
        std::vector<std::byte> extra = wire;
        extra.push_back(static_cast<std::byte>(0xAB));
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(extra, out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(oc.consumed == 12);
        CHECK(same_message(out, B(1)));
    }

    // A Level followed by nothing still reports only its own 29 bytes.
    {
        std::vector<std::byte> two = llmd::encode::encode(L(7, Side::Ask, 5, 6));
        llmd::encode::append_message(two, E(8));
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(two, out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(oc.consumed == 29);
        CHECK(same_message(out, L(7, Side::Ask, 5, 6)));
    }

    summary("stream decoding");
}

// ---------------------------------------------------------------------------
// Suite 4 — truncation: every prefix of a message must ask for more.
// ---------------------------------------------------------------------------

void suite_truncation() {
    // An empty span is a prefix of everything.
    expect_reject("empty input", {}, DecodeStatus::NeedMoreData);

    // Brackets: 12 bytes, so every prefix 1..11 is a header truncation.
    const std::vector<std::byte> begin = llmd::encode::encode(B(1));
    CHECK(begin.size() == 12);
    for (std::size_t n = 1; n < 12; ++n) {
        expect_reject("SnapshotBegin header truncation",
                      std::vector<std::byte>(begin.begin(),
                                             begin.begin() + static_cast<std::ptrdiff_t>(n)),
                      DecodeStatus::NeedMoreData);
    }

    const std::vector<std::byte> end = llmd::encode::encode(E(4));
    CHECK(end.size() == 12);
    for (std::size_t n = 1; n < 12; ++n) {
        expect_reject("SnapshotEnd header truncation",
                      std::vector<std::byte>(end.begin(),
                                             end.begin() + static_cast<std::ptrdiff_t>(n)),
                      DecodeStatus::NeedMoreData);
    }

    // A Level is 29 bytes. 1..11 truncates the header; 12..28 truncates the
    // payload, and each of those has already read a VALID header declaring 17
    // payload bytes — so they are the cases where a decoder that published
    // whatever it had would produce a half-written level.
    const std::vector<std::byte> level =
        llmd::encode::encode(L(0x0102030405060708ULL, Side::Bid, 100, 20));
    CHECK(level.size() == 29);
    for (std::size_t n = 1; n < 29; ++n) {
        expect_reject("Level truncation",
                      std::vector<std::byte>(level.begin(),
                                             level.begin() + static_cast<std::ptrdiff_t>(n)),
                      DecodeStatus::NeedMoreData);
    }

    // And the full 29 bytes do decode, so the loop above is testing the
    // boundary rather than testing that Levels never decode.
    {
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(level, out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(oc.consumed == 29);
    }

    summary("truncation");
}

// ---------------------------------------------------------------------------
// Suite 5 — malformed frames.
// ---------------------------------------------------------------------------

void suite_malformed() {
    const std::uint8_t kBegin = type_id(llmd::wire::MessageType::SnapshotBegin);
    const std::uint8_t kLevel = type_id(llmd::wire::MessageType::Level);
    const std::uint8_t kEnd = type_id(llmd::wire::MessageType::SnapshotEnd);

    // Version. The field is checked before anything else in the header is
    // trusted, so a frame with a wrong version is rejected even when the rest
    // of it is impeccable.
    {
        llmd::encode::RawMessage r = raw(kBegin, 0, 1);
        r.version = 2;
        expect_reject("version 2 -> InvalidVersion", raw_bytes(r),
                      DecodeStatus::InvalidVersion);
        r.version = 0;
        expect_reject("version 0 -> InvalidVersion", raw_bytes(r),
                      DecodeStatus::InvalidVersion);
        r.version = 255;
        expect_reject("version 255 -> InvalidVersion", raw_bytes(r),
                      DecodeStatus::InvalidVersion);
    }

    // Type. 0 and 4..255 are all unassigned.
    expect_reject("type 0 -> InvalidType", raw_bytes(raw(0, 0, 1)),
                  DecodeStatus::InvalidType);
    expect_reject("type 4 -> InvalidType", raw_bytes(raw(4, 0, 1)),
                  DecodeStatus::InvalidType);
    expect_reject("type 255 -> InvalidType", raw_bytes(raw(255, 0, 1)),
                  DecodeStatus::InvalidType);

    // Declared length. Rejected on the header alone: no payload bytes are
    // present for these, and the decoder must not wait for a length it already
    // knows is wrong.
    expect_reject("SnapshotBegin payload_length=1 -> InvalidLength",
                  raw_bytes(raw(kBegin, 1, 1)), DecodeStatus::InvalidLength);
    expect_reject("SnapshotBegin payload_length=17 -> InvalidLength",
                  raw_bytes(raw(kBegin, 17, 1)), DecodeStatus::InvalidLength);
    expect_reject("SnapshotEnd payload_length=1 -> InvalidLength",
                  raw_bytes(raw(kEnd, 1, 1)), DecodeStatus::InvalidLength);
    expect_reject("Level payload_length=16 -> InvalidLength",
                  raw_bytes(raw(kLevel, 16, 5)), DecodeStatus::InvalidLength);
    expect_reject("Level payload_length=18 -> InvalidLength",
                  raw_bytes(raw(kLevel, 18, 5)), DecodeStatus::InvalidLength);
    expect_reject("Level payload_length=0 -> InvalidLength",
                  raw_bytes(raw(kLevel, 0, 5)), DecodeStatus::InvalidLength);

    // Field-level rejects, each a full 29-byte frame with one bad field.
    auto level_raw = [&](std::uint8_t side, std::int64_t price, std::int64_t qty) {
        llmd::encode::RawMessage r = raw(kLevel, 17, 5);
        r.side = side;
        r.price = price;
        r.quantity = qty;
        return raw_bytes(r);
    };

    expect_reject("side 2 -> InvalidSide", level_raw(2, 100, 1),
                  DecodeStatus::InvalidSide);
    expect_reject("side 255 -> InvalidSide", level_raw(255, 100, 1),
                  DecodeStatus::InvalidSide);
    expect_reject("price 0 -> InvalidPrice", level_raw(0, 0, 1),
                  DecodeStatus::InvalidPrice);
    expect_reject("price -1 -> InvalidPrice", level_raw(0, -1, 1),
                  DecodeStatus::InvalidPrice);
    expect_reject("quantity -1 -> InvalidQuantity", level_raw(0, 100, -1),
                  DecodeStatus::InvalidQuantity);

    // The two wire-valid side values are accepted, and qty == 0 is a delete,
    // not a malformed quantity.
    {
        MdMessage out = sentinel();
        DecodeOutcome oc = llmd::decode_one(level_raw(0, 100, 0), out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(out.side == Side::Bid);
        CHECK(out.qty == 0);
        oc = llmd::decode_one(level_raw(1, 100, 0), out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(out.side == Side::Ask);
    }

    // THE SEQUENCE DOMAIN. `sequence` is encoded as a uint64, so all 2^64 bit
    // patterns are representable ON THE WIRE — and the tests below are the
    // boundary of the narrower domain the protocol actually admits,
    // [1, UINT64_MAX - 1]. Both reserved values are written as literal bytes so
    // that the all-zero and all-ones headers are visibly the ones under test
    // rather than a value the encoder chose.
    //
    // WHY THE TOP IS RESERVED. The sequencer downstream computes `seq + 1` to
    // track the next expected sequence. A SnapshotBegin at UINT64_MAX would wrap
    // that to 0, leaving a pipeline in Snapshot state expecting a sequence that
    // is itself outside the domain — a watermark no message can ever meet. The
    // sequencer is frozen and documents this precondition; the decoder is where
    // it has to be enforced, because it is a property of the bytes.
    const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    static_assert(llmd::wire::kMaxSequence == kMax - 1);

    // seq = 0 -> rejected.
    expect_reject("SnapshotBegin seq=0 -> InvalidSequence",
                  bytes_of({0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00}),
                  DecodeStatus::InvalidSequence);
    expect_reject("Level seq=0 -> InvalidSequence",
                  bytes_of({0x02, 0x01, 0x00, 0x11,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14}),
                  DecodeStatus::InvalidSequence);

    // seq = UINT64_MAX -> rejected, on the header alone: no Level payload is
    // present, and the decoder must not answer NeedMoreData for a frame whose
    // sequence is already known to be unrepresentable.
    expect_reject("SnapshotBegin seq=UINT64_MAX -> InvalidSequence",
                  bytes_of({0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF}),
                  DecodeStatus::InvalidSequence);
    expect_reject("SnapshotEnd seq=UINT64_MAX -> InvalidSequence",
                  bytes_of({0x03, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF}),
                  DecodeStatus::InvalidSequence);
    expect_reject("Level seq=UINT64_MAX, header only -> InvalidSequence",
                  bytes_of({0x02, 0x01, 0x00, 0x11, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF}),
                  DecodeStatus::InvalidSequence);
    expect_reject("Level seq=UINT64_MAX, full frame -> InvalidSequence",
                  bytes_of({0x02, 0x01, 0x00, 0x11,
                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14}),
                  DecodeStatus::InvalidSequence);

    // The two INCLUSIVE ends of the domain decode, and the value survives the
    // round trip at full width — this is where a decoder that truncated the
    // sequence to 32 bits, or that used `>=` where it meant `>`, would fail.
    {
        MdMessage out = sentinel();
        const DecodeOutcome lo = llmd::decode_one(
            bytes_of({0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x01}), out);
        CHECK(lo.status == DecodeStatus::Ok);
        CHECK(lo.consumed == 12);
        CHECK(out.seq == 1);

        const DecodeOutcome hi = llmd::decode_one(
            bytes_of({0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                      0xFF, 0xFF, 0xFF, 0xFE}), out);
        CHECK(hi.status == DecodeStatus::Ok);
        CHECK(hi.consumed == 12);
        CHECK(out.seq == kMax - 1);

        const DecodeOutcome lvl = llmd::decode_one(
            bytes_of({0x02, 0x01, 0x00, 0x11,
                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14}), out);
        CHECK(lvl.status == DecodeStatus::Ok);
        CHECK(lvl.consumed == 29);
        CHECK(out.seq == kMax - 1);
        CHECK(out.side == Side::Bid);
        CHECK(out.price == 100);
        CHECK(out.qty == 20);
    }

    // The check is a HEADER rule, so it is decided before the type is
    // consulted: a frame that is wrong in both ways reports the sequence. This
    // is the same placement as `version`, and the same reasoning as the
    // payload_length check below it — the offending bytes are already present,
    // so nothing that arrives later can change the verdict.
    expect_reject("bad type AND reserved seq -> InvalidSequence",
                  raw_bytes(raw(99, 0, kMax)), DecodeStatus::InvalidSequence);
    expect_reject("bad length AND reserved seq -> InvalidSequence",
                  raw_bytes(raw(kLevel, 16, kMax)), DecodeStatus::InvalidSequence);

    // And the boundary is NOT a sequencing decision: a duplicate, a stale or an
    // out-of-order sequence is in-domain and decodes, because those are the
    // pipeline's questions. 7 is not "next" for any stream here, and the
    // decoder has no opinion about that.
    {
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(raw_bytes(raw(kBegin, 0, 7)), out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(out.seq == 7);
    }

    // THE LAYER BOUNDARY. A positive price far outside the default book tick
    // range is WIRE-VALID and decodes cleanly. The decoder does not know what
    // the book trades, and must not guess: this same message becomes
    // `ApplyResult::OutOfRange` one layer up, which is a different fact with a
    // different consequence — the sequence is still consumed and the book stays
    // synced.
    {
        const std::int64_t far_tick = 999999999;
        CHECK(far_tick > llob::kDefaultTickMax);
        MdMessage out = sentinel();
        const DecodeOutcome oc = llmd::decode_one(level_raw(0, far_tick, 1), out);
        CHECK(oc.status == DecodeStatus::Ok);
        CHECK(oc.consumed == 29);
        CHECK(out.price == far_tick);
    }

    summary("malformed frames");
}

// ---------------------------------------------------------------------------
// Suite 6 — the end-to-end integration fixture.
//
//   encoded bytes -> decode_one() -> MdMessage -> MarketDataPipeline<Book>
//
// ONE focused fixture, not the 1550-trace corpus. The corpus exists to exercise
// the sequencer, and running it through a byte encoder would test the encoder
// against the decoder rather than either against its contract. What this
// fixture establishes is that the three pieces COMPOSE: the bytes that reach
// the decoder are the bytes a feed would deliver, and the messages that fall
// out of it drive the frozen Phase-1A state machine to the state the sketch
// says they should.
// ---------------------------------------------------------------------------

struct Integration {
    std::vector<DecodeStatus> statuses;
    std::vector<std::size_t>  consumed;
    std::vector<MdOutcome>    outcomes;
    MdState                   state = MdState::NotSynced;
    std::uint64_t             last_applied = 0;
    std::uint64_t             expected = 0;
    std::int64_t              best_bid = 0;
    std::int64_t              best_ask = 0;
    std::size_t               level_count = 0;
};

template <class Book>
Integration run_integration(const std::vector<std::byte>& wire) {
    Integration r;
    Book book;
    MarketDataPipeline<Book> pipe{std::move(book), typename MarketDataPipeline<Book>::Config{}};

    const std::span<const std::byte> all(wire);
    std::size_t offset = 0;
    while (offset < wire.size()) {
        MdMessage m = sentinel();
        const DecodeOutcome oc = llmd::decode_one(all.subspan(offset), m);
        r.statuses.push_back(oc.status);
        r.consumed.push_back(oc.consumed);
        if (oc.status != DecodeStatus::Ok) break;
        const llmd::MdResult res = pipe.apply(m);
        r.outcomes.push_back(res.outcome);
        offset += oc.consumed;
    }

    r.state        = pipe.state();
    r.last_applied = pipe.book().last_applied_seq();
    r.expected     = pipe.expected();
    r.best_bid     = pipe.book().best_bid();
    r.best_ask     = pipe.book().best_ask();
    r.level_count  = pipe.book().level_count();
    return r;
}

void suite_integration() {
    // The deterministic fixture: a snapshot bracketed at sequences 1..4, then
    // one live update that REPLACES the resting bid at 100.
    const std::vector<MdMessage> stream = {B(1), L(2, Side::Bid, 100, 10),
                                           L(3, Side::Ask, 101, 20), E(4),
                                           L(5, Side::Bid, 100, 12)};
    const std::vector<std::byte> wire = llmd::encode::encode_all(stream);
    CHECK(wire.size() == 111);

    const Integration flat = run_integration<FlatOrderBook>(wire);
    const Integration map  = run_integration<MapOrderBook>(wire);

    const std::vector<DecodeStatus> want_status(5, DecodeStatus::Ok);
    const std::vector<std::size_t> want_consumed = {12, 29, 29, 12, 29};
    const std::vector<MdOutcome> want_outcomes = {
        MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
        MdOutcome::SnapshotCommitted, MdOutcome::Applied};

    // Every message decoded, and the byte accounting closes exactly.
    CHECK(flat.statuses == want_status);
    CHECK(flat.consumed == want_consumed);
    CHECK(flat.outcomes == want_outcomes);

    // The bracket committed, so the stream is live at the End's sequence and
    // the update at 5 applied on top of it.
    CHECK(flat.state == MdState::Live);
    CHECK(flat.last_applied == 5);
    CHECK(flat.expected == 6);

    // The snapshot set the bid to 10 and the ask to 20; the live update at 5
    // replaced the bid with 12 and left the ask alone.
    CHECK(flat.best_bid == 100);
    CHECK(flat.best_ask == 101);
    CHECK(flat.level_count == 2);

    // Both sinks must agree on the fixture, not merely on its top of book.
    CHECK(map.statuses == flat.statuses);
    CHECK(map.consumed == flat.consumed);
    CHECK(map.outcomes == flat.outcomes);
    CHECK(map.state == flat.state);
    CHECK(map.last_applied == flat.last_applied);
    CHECK(map.expected == flat.expected);
    CHECK(map.best_bid == flat.best_bid);
    CHECK(map.best_ask == flat.best_ask);
    CHECK(map.level_count == flat.level_count);

    summary("bytes -> decoder -> pipeline");
}

// ---------------------------------------------------------------------------
// Suite 7 — the composed invariant: decode failure mutates nothing.
//
//   decode failure -> no typed message publication -> no sequencer/book mutation
//
// Each half of that chain is already tested. The decoder suites prove a failed
// decode publishes nothing; the Phase-1A suites prove the sequencer behaves for
// every message it is given. Neither proves the COMPOSITION, and the
// composition is where the contract actually lives: a caller that ignored the
// status and applied `out` anyway would be applying a sentinel, and a decoder
// that wrote 8 of 29 bytes before failing would be handing over a real-looking
// half-message. The pipeline cannot defend against either — it has no way to
// know the message it was handed did not come from a successful decode.
//
// So the invariant is pinned at the seam instead: given a live pipeline, a
// failed decode must leave every observable piece of pipeline and book state
// byte-for-byte where it was.
// ---------------------------------------------------------------------------

template <class Book>
void check_no_mutation(const char* label, const std::vector<std::byte>& bad_frame) {
    // Establish a valid LIVE pipeline through the decoder, so the state under
    // test is one a real stream produced rather than one a fixture conjured.
    Book book;
    MarketDataPipeline<Book> pipe{std::move(book),
                                  typename MarketDataPipeline<Book>::Config{}};
    const std::vector<MdMessage> prefix = {B(1), L(2, Side::Bid, 100, 10),
                                          L(3, Side::Ask, 101, 20), E(4)};
    const std::vector<std::byte> prefix_wire = llmd::encode::encode_all(prefix);
    std::size_t offset = 0;
    while (offset < prefix_wire.size()) {
        MdMessage m{};
        const DecodeOutcome oc =
            llmd::decode_one(std::span<const std::byte>(prefix_wire).subspan(offset), m);
        CHECK(oc.status == DecodeStatus::Ok);
        pipe.apply(m);
        offset += oc.consumed;
    }

    // Record everything a caller could observe.
    const MdState      state0 = pipe.state();
    const std::uint64_t cursor0 = pipe.book().last_applied_seq();
    const std::uint64_t expect0 = pipe.expected();
    const std::int64_t  bid0 = pipe.book().best_bid();
    const std::int64_t  ask0 = pipe.book().best_ask();
    const std::size_t   levels0 = pipe.book().level_count();
    const llmd::MdCounters counters0 = pipe.counters();

    // Confirm the precondition is actually the interesting one: a live,
    // synced pipeline with a real book, not an empty one that would trivially
    // "not change".
    CHECK(state0 == MdState::Live);
    CHECK(cursor0 == 4);
    CHECK(expect0 == 5);
    CHECK(levels0 == 2);

    // The malformed frame. The decode is attempted and the status is honoured:
    // apply() is NOT called, because there is no message to apply.
    MdMessage out = sentinel();
    const DecodeOutcome bad = llmd::decode_one(bad_frame, out);
    CHECK(bad.status != DecodeStatus::Ok);
    CHECK(bad.consumed == 0);
    CHECK(same_message(out, sentinel()));

    // Nothing moved. Not the sequence position, not the book, not the counters.
    CHECK(pipe.state() == state0);
    CHECK(pipe.book().last_applied_seq() == cursor0);
    CHECK(pipe.expected() == expect0);
    CHECK(pipe.book().best_bid() == bid0);
    CHECK(pipe.book().best_ask() == ask0);
    CHECK(pipe.book().level_count() == levels0);
    CHECK(pipe.counters().messages == counters0.messages);
    CHECK(pipe.counters().applied == counters0.applied);
    CHECK(pipe.counters().snapshot_committed == counters0.snapshot_committed);
    CHECK(pipe.counters().staged == counters0.staged);
    CHECK(pipe.counters().malformed == counters0.malformed);

    if (g_failures == 0) {
        std::printf("       %s: live at seq=%llu, decode rejected, nothing moved\n",
                    label, static_cast<unsigned long long>(cursor0));
    }
}

void suite_composed_invariant() {
    // A frame that is structurally fine but carries an unusable side: a full,
    // well-formed 29-byte Level with side = 5. The frame must be COMPLETE —
    // a short one would return NeedMoreData and the test would pass for the
    // wrong reason, proving nothing about publication on a field rejection.
    const std::vector<std::byte> bad_side =
        bytes_of({0x02, 0x01, 0x00, 0x11,                          // type/ver/len
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,  // seq = 9
                  0x05,                                            // side = 5
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64,  // price 100
                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14}); // qty 20
    CHECK(bad_side.size() == 29);

    // A frame whose sequence is reserved. Note this one is otherwise a
    // perfectly good SnapshotBegin: if it were applied, the pipeline would
    // leave Live and the wrap would silently reset the watermark to 0.
    const std::vector<std::byte> bad_seq =
        bytes_of({0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
                  0xFF, 0xFF, 0xFF, 0xFF});

    check_no_mutation<MapOrderBook>("invalid side", bad_side);
    check_no_mutation<MapOrderBook>("invalid sequence", bad_seq);
    check_no_mutation<FlatOrderBook>("invalid side", bad_side);
    check_no_mutation<FlatOrderBook>("invalid sequence", bad_seq);

    summary("decode failure leaves the pipeline untouched");
}

// ---------------------------------------------------------------------------
// The deliberate failure, reached only through the self-test env var.
// ---------------------------------------------------------------------------
void suite_selftest_failure() {
    CHECK(1 == 1);
    CHECK(1 == 2); // deliberately false: proves the exit path is non-zero
    summary("deliberate failure (self-test)");
}

} // namespace

int main() {
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("LLDB_SELFTEST_FAIL set: running the deliberate-failure suite only\n");
        suite_selftest_failure();
        std::printf("selftest: %d failure(s) recorded; exit status must be non-zero\n",
                    g_failures_total);
        return g_failures_total == 0 ? 0 : 1;
    }

    std::printf("Experiment 03 Phase 1B — wire protocol decoder correctness\n\n");
    suite_exact_bytes();
    suite_endian_helpers();
    suite_stream();
    suite_truncation();
    suite_malformed();
    suite_integration();
    suite_composed_invariant();

    if (g_failures_total == 0) {
        std::printf("\nALL SUITES PASSED\n");
    } else {
        std::printf("\n%d CHECK FAILURE(S)\n", g_failures_total);
    }
    return g_failures_total == 0 ? 0 : 1;
}
