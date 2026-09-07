#pragma once

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

// ---------------------------------------------------------------------------
// One aggregated L2 price-level update.
// ---------------------------------------------------------------------------
struct L2Update {
    uint64_t seq;   // stream sequence number; must be next_expected_seq() when synced
    int64_t  price; // in ticks, always > 0 for a well-formed update
    int64_t  qty;   // absolute size at `price`; 0 => delete the whole level
    Side     side;
};

// ---------------------------------------------------------------------------
// Per-price-level quantities held by the books.
// ---------------------------------------------------------------------------
struct Level {
    int64_t qty = 0;
};

// ---------------------------------------------------------------------------
// Outcome of feeding one update into an order book.
// ---------------------------------------------------------------------------
enum class ApplyResult : uint8_t {
    Applied,        // update applied; book contents changed accordingly
    Stale,          // seq <= already-seen; ignored (no-op)
    GapDetected,    // seq jumped ahead; book marked unsynced, needs a snapshot
};

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
    uint64_t          seq;      // sequence number the snapshot corresponds to
    SideSnapshot      bids;
    SideSnapshot      asks;
};

// ---------------------------------------------------------------------------
// Interface shared by every order-book implementation.
//
// The core contract (identical for MapOrderBook and FlatOrderBook):
//
//   * apply() returns Applied  when the update is accepted and reflected.
//   * A qty of 0 removes the level. Removing a price that is not present is a
//     no-op (idempotent delete).
//   * apply() returns GapDetected and marks the book unsynced() the first time
//     an incoming seq skips ahead of next_expected_seq(). This is NOT an error
//     in the caller's data — it is the book reporting that it can no longer
//     maintain a consistent view, so it must be reloaded from a snapshot.
//   * Once unsynced, apply() returns Stale for every further update (including
//     further future sequences) until load_snapshot() restores a baseline:
//     nothing can repair the sequence tracker except a fresh snapshot.
//   * apply() returns Stale for an already-seen (<=) seq; replay is ignored
//     and never desynchronizes the book.
//   * After synced, seq must advance by exactly +1 per applied update.
//   * load_snapshot() installs a full state and re-synchronizes the sequence
//     tracker regardless of prior state.
//
// A snapshot only carries prices and quantities for levels with qty > 0.
// best_bid()/best_ask() return 0 when the corresponding side is empty, and an
// empty side never participates in best-price crossing (bids < asks when both
// present). best_bid() and best_ask() return cached values — reading them is
// O(1) and side-effect free on both designs.
// ---------------------------------------------------------------------------
struct OrderBookApi {
    virtual ~OrderBookApi() = default;
    virtual ApplyResult   apply(const L2Update& u) = 0;
    virtual void          load_snapshot(const BookSnapshot& s) = 0;
    virtual int64_t       best_bid() const noexcept = 0;
    virtual int64_t       best_ask() const noexcept = 0;
    virtual bool          synced() const noexcept = 0;
    virtual uint64_t      next_expected_seq() const noexcept = 0;
    virtual uint64_t      last_applied_seq() const noexcept = 0;
    virtual bool          empty() const noexcept = 0;
};

} // namespace llob
