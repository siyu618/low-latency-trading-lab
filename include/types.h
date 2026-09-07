#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace llob {

// ---------------------------------------------------------------------------
// Shared market-data vocabulary.
//
// Prices are integral ticks everywhere in this code base. A "tick" is the
// minimum price increment of the instrument (for many venues the tick is
// itself an integer multiple of a price value; for others it maps onto a
// price via tick_size). Order books must never do arithmetic in double:
// tick integers compare, store and map to storage indexes exactly.
// ---------------------------------------------------------------------------

enum class Side : uint8_t { Bid = 0, Ask = 1 };

// True if `side` is a buy-side price level.
inline constexpr bool is_bid(Side side) noexcept { return side == Side::Bid; }

// Default price-domain bounds shared by MapOrderBook and FlatOrderBook.
// Both books are configured over the same [tick_min, tick_max] domain so that
// benchmark inputs are treated identically; the default is a banded domain
// (see the design discussion in the README). Construct any two books you
// intend to compare with the SAME domain.
inline constexpr int64_t kDefaultTickMin = 1;
inline constexpr int64_t kDefaultTickMax = 200'000;

// ---------------------------------------------------------------------------
// One aggregated L2 price-level update.
// ---------------------------------------------------------------------------
struct L2Update {
    uint64_t seq;   // stream sequence number; must be next_expected_seq() when synced
    int64_t  price; // in ticks, must lie in the book's [tick_min, tick_max] domain
    int64_t  qty;   // absolute size at `price`; 0 => delete the level; never negative
    Side     side;
};

// ---------------------------------------------------------------------------
// Outcome of feeding one update into an order book.
//
// The enum deliberately separates *stale sequencing* from *malformed content*:
// a Stale result means "we have already seen this or a later message", which
// is normal under replay; InvalidUpdate / OutOfRange mean the message is a
// brand-new sequence whose content we refused.
// ---------------------------------------------------------------------------
enum class ApplyResult : uint8_t {
    Applied,        // update applied; book contents changed accordingly
    Stale,          // seq <= last applied (replay/older) OR book is unsynced; ignored
    GapDetected,    // synced, seq skipped ahead => view untrustworthy, book unsynced
    InvalidUpdate,  // synced, in-order seq, but qty < 0 (corrupt) => book unsynced
    OutOfRange,     // synced, in-order seq, but price outside the configured domain
};                  //   => ignored (book intentionally does not cover that price)

// ---------------------------------------------------------------------------
// A price-ordered view of one side of the book (sorted per side convention:
// bids descending, asks ascending). Only for inspection / testing — the hot
// path never materializes these vectors.
// ---------------------------------------------------------------------------
struct SideSnapshot {
    std::vector<int64_t> prices; // book order (best first)
    std::vector<int64_t> qtys;
};

// ---------------------------------------------------------------------------
// Snapshot feed used for (re)synchronization: full-state replace, cold path.
// ---------------------------------------------------------------------------
struct BookSnapshot {
    uint64_t     seq; // sequence number the snapshot corresponds to
    SideSnapshot bids;
    SideSnapshot asks;
};

// ---------------------------------------------------------------------------
// Snapshot validation. Shared by every book implementation so that accept /
// reject behavior is identical across designs (a snapshot either fully loads
// or is rejected with no partial state).
//
// A snapshot is malformed and must be rejected if, on either side:
//   * prices.size() != qtys.size()              (never silently truncate)
//   * any price lies outside [tick_min, tick_max]
//   * any qty < 0
//   * a price is duplicated on the same side    (ambiguous state)
// A qty of 0 is allowed and treated as "no level at that price".
// ---------------------------------------------------------------------------
inline bool side_snapshot_valid(const SideSnapshot& sn,
                                int64_t tick_min,
                                int64_t tick_max) {
    const size_t n = sn.prices.size();
    if (sn.qtys.size() != n) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    std::vector<int64_t> sorted;
    sorted.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const int64_t p = sn.prices[i];
        const int64_t q = sn.qtys[i];
        if (q < 0) {
            return false;
        }
        if (p < tick_min || p > tick_max) {
            return false;
        }
        sorted.push_back(p);
    }
    std::sort(sorted.begin(), sorted.end());
    return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
}

inline bool validate_snapshot(const BookSnapshot& s,
                              int64_t tick_min,
                              int64_t tick_max) {
    return side_snapshot_valid(s.bids, tick_min, tick_max) &&
           side_snapshot_valid(s.asks, tick_min, tick_max);
}

// ---------------------------------------------------------------------------
// Apply / snapshot contract (identical for MapOrderBook and FlatOrderBook).
//
// apply() precedence — evaluated in this exact order by both books:
//   1. u.seq <= last applied      -> Stale. Nothing changes. (replay/older)
//   2. book unsynced()            -> Stale. Nothing changes. Only a snapshot
//                                    can restore a usable book.
//   3. u.seq > last applied + 1   -> GapDetected. Book becomes unsynced();
//                                    the view can no longer be maintained.
//   4. u.qty < 0                  -> InvalidUpdate. Book becomes unsynced();
//                                    corrupt content means the stream is not
//                                    trustworthy, so the book must be rebuilt
//                                    from a snapshot. Sequence NOT consumed.
//   5. price outside [tick_min,
//      tick_max]                  -> OutOfRange. Sequence IS consumed (the book
//                                    stays contiguous) but the level is not
//                                    stored; a banded book intentionally does
//                                    not cover every price. Book stays synced.
//   6. otherwise                  -> Applied. qty > 0 sets/creates the level;
//                                    qty == 0 removes it (idempotent if absent).
//                                    Sequence consumed.
//
// A qty of 0 removes the level. Removing a price that is not present is a
// no-op (idempotent delete) and still returns Applied.
//
// A level outside the domain (step 5) cannot represent a delete we have never
// stored, so it too is simply ignored.
//
// load_snapshot() validates the full snapshot first; if it is malformed the
// book is left completely unchanged (prior state and synced() flag intact) and
// load_snapshot returns false. On success the book is fully replaced and
// re-synchronized regardless of prior state.
//
// best_bid()/best_ask() return cached best prices in O(1) and report 0 for an
// empty side.
// ---------------------------------------------------------------------------
} // namespace llob
