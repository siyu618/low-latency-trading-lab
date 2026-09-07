#pragma once

#include "types.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace llob {

// ---------------------------------------------------------------------------
// FlatOrderBook — dense, preallocated, tick-addressed L2 book.
//
// Design:
//   * Prices are integer ticks in [tick_min, tick_max]. Each tick maps to one
//     array slot via  price_index = price - tick_min.  A slot holds the
//     absolute quantity for that price level (0 == no level), so an update is
//     a direct array write with no search, no node allocation and no pointer
//     chasing.
//   * Dense addressing means the array is sized by the *span* of the domain,
//     not the number of live levels. A 1..1e9-tick instrument costs 8 GB per
//     side (2x int64 per tick), which is why realistic configurations keep the
//     tick span modest (e.g. a 100k-tick band around the last price).
//   * best_bid/best_ask are cached integers. Deleting the level that holds the
//     current best triggers an immediate (eager) linear scan of that side to
//     find the new best, so the cache is always valid and reads are pure O(1).
//     The eager scan keeps updates deterministic — the cost lands on the delete
//     itself, never on a later read. This makes "update a non-best level" O(1),
//     and "delete the best price" O(span) — the documented worst case.
//
// Hot path: apply() performs no dynamic allocation after construction, and
// load_snapshot() performs none either once the storage is preallocated.
//
// Memory layout: the two sides are two separate int64 vectors so each side's
// scan touches a single contiguous cache line run.
// ---------------------------------------------------------------------------
class FlatOrderBook final {
public:
    FlatOrderBook(int64_t tick_min, int64_t tick_max)
        : tick_min_(tick_min)
        , tick_span_(tick_max - tick_min + 1) {
        if (tick_span_ <= 0) {
            throw std::invalid_argument("FlatOrderBook: tick_max must be >= tick_min");
        }
        allocate();
    }

    // Number of addressable tick slots per side.
    int64_t capacity() const noexcept { return tick_span_; }

    // ---- Hot path ----------------------------------------------------------

    ApplyResult apply(const L2Update& u) {
        if (u.seq <= seq_) {
            return ApplyResult::Stale;
        }
        if (!synced_) {
            // Already desynchronized; ignore until a snapshot restores state.
            return ApplyResult::Stale;
        }
        if (u.seq != seq_ + 1) {
            synced_ = false;
            return ApplyResult::GapDetected;
        }

        const int64_t idx = u.price - tick_min_;
        if (idx < 0 || idx >= tick_span_) {
            return ApplyResult::Stale; // outside this book's domain
        }

        const size_t s  = side_of(u.side);
        auto& qty       = qty_[s][idx];
        const bool is_best = (idx == best_idx_[s]);
        if (u.qty == 0) {
            if (qty == 0) {
                // Deleting a level that is not present. If it were the cached
                // best the cache would already be consistent (nothing present
                // to evict, and the cached best slot is always present while
                // >= 0). Just advance the sequence.
                seq_ = u.seq;
                return ApplyResult::Applied;
            }
            qty = 0;
            if (is_best) {
                // Removed the level the cache points at. Rescan NOW so that
                // best_bid()/best_ask() stay pure O(1) reads with no hidden
                // invalidation work. This is the documented O(span) worst
                // case; it happens exactly once per best-level deletion.
                best_idx_[s] = find_best(u.side);
            }
        } else {
            qty = u.qty;
            if (!is_best) {
                update_best_if_needed(u.side, u.price);
            }
            // If it *is* the best we keep the cache: same price, new quantity.
        }

        seq_ = u.seq;
        return ApplyResult::Applied;
    }

    // ---- Cold path ---------------------------------------------------------

    void load_snapshot(const BookSnapshot& s) {
        clear_storage(); // zero both sides

        const auto& bp = s.bids.prices;
        const auto& bq = s.bids.qtys;
        for (size_t i = 0; i < bp.size() && i < bq.size(); ++i) {
            if (bq[i] > 0) {
                const int64_t idx = bp[i] - tick_min_;
                if (idx >= 0 && idx < tick_span_) {
                    qty_[0][idx] = bq[i];
                }
            }
        }
        const auto& ap = s.asks.prices;
        const auto& aq = s.asks.qtys;
        for (size_t i = 0; i < ap.size() && i < aq.size(); ++i) {
            if (aq[i] > 0) {
                const int64_t idx = ap[i] - tick_min_;
                if (idx >= 0 && idx < tick_span_) {
                    qty_[1][idx] = aq[i];
                }
            }
        }

        seq_        = s.seq;
        synced_     = true;
        best_idx_[0] = find_best(Side::Bid);
        best_idx_[1] = find_best(Side::Ask);
    }

    // ---- Accessors ---------------------------------------------------------

    int64_t best_bid() const noexcept {
        return best_idx_[0] < 0 ? 0 : tick_min_ + best_idx_[0];
    }
    int64_t best_ask() const noexcept {
        return best_idx_[1] < 0 ? 0 : tick_min_ + best_idx_[1];
    }
    int64_t best_bid_qty() const noexcept {
        return best_idx_[0] < 0 ? 0 : qty_[0][best_idx_[0]];
    }
    int64_t best_ask_qty() const noexcept {
        return best_idx_[1] < 0 ? 0 : qty_[1][best_idx_[1]];
    }

    bool     synced() const noexcept { return synced_; }
    uint64_t next_expected_seq() const noexcept { return seq_ + 1; }
    uint64_t last_applied_seq() const noexcept { return seq_; }
    bool     empty() const noexcept { return best_idx_[0] < 0 && best_idx_[1] < 0; }

    // Number of live levels across both sides (test/debug helper; scans).
    size_t level_count() const noexcept {
        size_t n = 0;
        for (int64_t v : qty_[0]) n += (v != 0);
        for (int64_t v : qty_[1]) n += (v != 0);
        return n;
    }

    // The current best (highest filled) price on a side, or 0 when empty.
    int64_t side_best(Side side) const noexcept {
        const int64_t i = best_idx_[side_of(side)];
        return i < 0 ? 0 : tick_min_ + i;
    }

    // Quantity at an exact price level (0 == absent/empty). Test/debug helper.
    int64_t level_qty(int64_t price, Side side) const noexcept {
        const int64_t idx = price - tick_min_;
        if (idx < 0 || idx >= tick_span_) return 0;
        return qty_[side_of(side)][static_cast<size_t>(idx)];
    }

private:
    static constexpr size_t side_of(Side s) noexcept {
        return s == Side::Bid ? 0u : 1u;
    }

    void allocate() {
        const size_t n = static_cast<size_t>(tick_span_);
        qty_[0].assign(n, 0);
        qty_[1].assign(n, 0);
    }

    void clear_storage() {
        std::memset(qty_[0].data(), 0, qty_[0].size() * sizeof(int64_t));
        std::memset(qty_[1].data(), 0, qty_[1].size() * sizeof(int64_t));
    }

    // Recompute the cached best for `side` by scanning its slots.
    int64_t find_best(Side side) const noexcept {
        const size_t s = side_of(side);
        if (is_bid(side)) { // bids: highest filled index
            for (int64_t i = tick_span_ - 1; i >= 0; --i) {
                if (qty_[s][static_cast<size_t>(i)] != 0) return i;
            }
        } else { // asks: lowest filled index
            for (int64_t i = 0; i < tick_span_; ++i) {
                if (qty_[s][static_cast<size_t>(i)] != 0) return i;
            }
        }
        return -1;
    }

    // After writing a non-best level, promote the cache if the new level is
    // better than the current best (or the side has no cached best).
    void update_best_if_needed(Side side, int64_t price) noexcept {
        const size_t s = side_of(side);
        const int64_t cur = best_idx_[s];
        if (cur < 0) {
            best_idx_[s] = price - tick_min_;
            return;
        }
        if (is_bid(side)) {
            if (price > tick_min_ + cur) best_idx_[s] = price - tick_min_;
        } else {
            if (price < tick_min_ + cur) best_idx_[s] = price - tick_min_;
        }
    }

    int64_t      tick_min_;
    int64_t      tick_span_;                       // addressable slots per side
    std::vector<int64_t> qty_[2];                  // [side][price - tick_min_] = level qty
    int64_t      best_idx_[2] = {-1, -1};          // cached best slot per side
    uint64_t     seq_    = 0;
    bool         synced_ = false;
};

} // namespace llob
