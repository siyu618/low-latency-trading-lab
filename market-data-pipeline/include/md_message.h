#pragma once

#include "types.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// Experiment 03 — Market Data Pipeline. Shared message vocabulary.
//
// A market-data feed is a SEQUENCED stream of messages. This header defines the
// three kinds the pipeline recognises:
//
//   SnapshotBegin  a full-state run starts here
//   SnapshotEnd    ...and ends here; the book is published at this sequence
//   Level          one aggregated L2 price-level update
//
// THE SNAPSHOT IS SEQUENCED. SnapshotBegin/Level.../Level/SnapshotEnd is a
// contiguous run that CONSUMES sequence numbers exactly like the incremental
// stream does, and SnapshotEnd's own seq becomes the sequence the book is
// published at, so the next incremental message is `end.seq + 1`. There is no
// side channel and no out-of-band snapshot.
//
// The level messages inside a snapshot run are ordinary `Level` messages — the
// same kind that carries the live stream. The ONLY difference is the framing
// around them. This is deliberate: the snapshot path is not a second message
// type with its own decoding rules, it is the same message under a bracket.
//
// Deliberately ABSENT: any timestamp, ingress stamp or arrival field. Phase 1
// is a correctness phase and measures no time; a stamp would be dead weight on
// this path, and adding one later is a separate, separately-attributed change.
// ---------------------------------------------------------------------------

namespace llmd {

enum class MdKind : std::uint8_t {
    SnapshotBegin, // bracket open; the accompanying seq is the run's first seq
    SnapshotEnd,   // bracket close; the accompanying seq becomes last_applied
    Level,         // one L2 price-level update (live stream or snapshot content)
};

inline const char* md_kind_name(MdKind k) noexcept {
    switch (k) {
        case MdKind::SnapshotBegin: return "SnapshotBegin";
        case MdKind::SnapshotEnd:   return "SnapshotEnd";
        case MdKind::Level:         return "Level";
    }
    return "?";
}

// One message on the wire, already decoded.
//
// `side`, `price` and `qty` are meaningful ONLY for MdKind::Level. The factories
// below zero them for the bracket kinds so that a hand-written trace cannot
// carry a stale field into a message where it means nothing.
//
// `qty < 0` is representable on purpose: it is a malformed message the pipeline
// must classify, not a value it may assume away. `qty == 0` is a well-formed
// delete (or, inside a snapshot, "no level at this price").
struct MdMessage {
    MdKind       kind;
    std::uint64_t seq;
    llob::Side   side;
    std::int64_t price;
    std::int64_t qty;
};

inline MdMessage md_begin(std::uint64_t seq) noexcept {
    return MdMessage{MdKind::SnapshotBegin, seq, llob::Side::Bid, 0, 0};
}

inline MdMessage md_end(std::uint64_t seq) noexcept {
    return MdMessage{MdKind::SnapshotEnd, seq, llob::Side::Bid, 0, 0};
}

inline MdMessage md_level(std::uint64_t seq, llob::Side side, std::int64_t price,
                          std::int64_t qty) noexcept {
    return MdMessage{MdKind::Level, seq, side, price, qty};
}

} // namespace llmd
