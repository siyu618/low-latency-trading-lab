#pragma once

#include "types.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace llob {

// ---------------------------------------------------------------------------
// TransitionAwareFlatOrderBook — EXPERIMENTAL CONTROL for the Experiment 01
// Optimization Study (NOT a production book, NOT a replacement for
// FlatOrderBook).
//
// It isolates ONE variable: the control flow of the update hot path. The
// question the study asks of BitsetFlatOrderBook ("does the occupancy bitmap
// make best-price discovery cheaper?") is confounded because the bitmap book
// also changed ordinary positive->positive bookkeeping relative to the frozen
// FlatOrderBook:
//
//   * FlatOrderBook computes and checks the cached-best state on EVERY positive
//     write, even when the level being written is already present. For a
//     positive->positive re-quantify of a non-best level that bookkeeping is
//     dead work: a level that is present and not the best cannot BECOME the best
//     by having its quantity change, because the best is a property of the
//     occupied PRICE set, which a present->present write does not change.
//
//   * BitsetFlatOrderBook instead branches on the occupancy TRANSITION and only
//     runs best bookkeeping when a level appears (0 -> positive); a present->
//     present write is a pure quantity store that touches neither the bitmap
//     nor the cached-best index.
//
// This control is a copy of FlatOrderBook whose ONLY change is that same
// transition-aware positive path:
//
//   positive write:
//     if the slot is empty (0 -> positive): store qty, then promote the cached
//         best if the new level beats it (the only positive write that can move
//         the best);
//     else (positive -> positive): store qty; do NO cached-best bookkeeping.
//
// Deletion keeps FlatOrderBook's exact behavior: an adjacent LINEAR re-scan
// (scan_best_from) when the deleted level is the cached best. There is NO
// occupancy bitmap anywhere in this class.
//
// Consequences for the study's attribution:
//   * Flat vs TransitionAwareFlat      = the control-flow optimization alone.
//   * TransitionAwareFlat vs Bitset    = the occupancy bitmap alone, with the
//                                        non-bitmap hot-path structure held
//                                        constant (same contiguous quantity
//                                        array, same linear best re-scan on
//                                        delete, same transition-aware positive
//                                        path).
//   * Flat vs Bitset                   = the end-to-end candidate comparison
//                                        (control flow + bitmap together); it
//                                        must NOT be read as "the bitmap".
//
// This class exists only as a benchmark/test fixture and is deliberately NOT
// used by any production code path. It preserves the same externally visible
// semantics as FlatOrderBook (same domain, sequence/snapshot/gap/invalid/out-of-
// range behavior, same best-read O(1) cache), and apply() performs no dynamic
// allocation after construction.
// ---------------------------------------------------------------------------
class TransitionAwareFlatOrderBook final {
public:
    TransitionAwareFlatOrderBook(int64_t tick_min = kDefaultTickMin,
                                 int64_t tick_max = kDefaultTickMax)
        : tick_min_(tick_min)
        , tick_span_(tick_max - tick_min + 1) {
        if (tick_span_ <= 0) {
            throw std::invalid_argument(
                "TransitionAwareFlatOrderBook: tick_max must be >= tick_min");
        }
        allocate();
    }

    int64_t tick_min() const noexcept { return tick_min_; }
    int64_t tick_max() const noexcept { return tick_min_ + tick_span_ - 1; }

    // Number of addressable tick slots per side.
    int64_t capacity() const noexcept { return tick_span_; }

    // ---- Hot path ----------------------------------------------------------

    ApplyResult apply(const L2Update& u) {
        if (u.seq <= seq_) {
            return ApplyResult::Stale; // replay/older, or unsynced: nothing changes
        }
        if (!synced_) {
            return ApplyResult::Stale; // only a snapshot can restore a usable book
        }
        if (u.seq != seq_ + 1) {
            synced_ = false;
            return ApplyResult::GapDetected;
        }

        // Brand-new, in-order sequence: validate content before touching state.
        if (u.qty < 0) {
            // Corrupt content => the stream is not trustworthy; refuse the
            // update and require a snapshot rebuild. Sequence NOT consumed.
            synced_ = false;
            return ApplyResult::InvalidUpdate;
        }
        const int64_t idx = u.price - tick_min_;
        if (idx < 0 || idx >= tick_span_) {
            // Outside this book's configured domain. The book intentionally
            // does not cover this price (banded design), so the update is
            // ignored but the sequence IS consumed to keep the view contiguous.
            seq_ = u.seq;
            return ApplyResult::OutOfRange;
        }

        const size_t s  = side_of(u.side);
        // idx is in [0, tick_span_) here (validated just above), so the
        // narrowing to size_t is safe.
        auto& qty       = qty_[s][static_cast<size_t>(idx)];
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
            if (idx == best_idx_[s]) {
                // Removed the level the cache points at. Find the next best by
                // scanning inward from the adjacent slot (FlatOrderBook's exact
                // linear re-scan; NO bitmap here), so best reads stay O(1) and
                // the cost is on the delete itself.
                best_idx_[s] = rescan_after_delete(u.side, idx);
            }
        } else {
            if (qty == 0) {
                // 0 -> positive: a level appears. This is the ONLY positive
                // write that can move the cached best (a new occupied price may
                // beat the current one), so promote if needed.
                qty = u.qty;
                update_best_if_needed(u.side, u.price);
            } else {
                // positive -> positive: pure quantity store. The occupied price
                // set did not change, so the cached best cannot move; no
                // cached-best bookkeeping is performed (the control-flow change
                // under study — see the class comment).
                qty = u.qty;
            }
        }

        seq_ = u.seq;
        return ApplyResult::Applied;
    }

    // ---- Cold path ---------------------------------------------------------

    // Replaces book state from `s`. Returns false (leaving the book completely
    // unchanged) if the snapshot is malformed; cold path, correctness first.
    bool load_snapshot(const BookSnapshot& s) {
        if (!validate_snapshot(s, tick_min_, tick_min_ + tick_span_ - 1)) {
            return false; // reject, do not partially load, do not touch synced()
        }

        clear_storage(); // zero both sides

        for (size_t i = 0; i < s.bids.prices.size(); ++i) {
            const int64_t q = s.bids.qtys[i];
            if (q == 0) continue; // qty 0 == no level; validated >= 0 already
            qty_[0][static_cast<size_t>(s.bids.prices[i] - tick_min_)] = q;
        }
        for (size_t i = 0; i < s.asks.prices.size(); ++i) {
            const int64_t q = s.asks.qtys[i];
            if (q == 0) continue;
            qty_[1][static_cast<size_t>(s.asks.prices[i] - tick_min_)] = q;
        }

        seq_        = s.seq;
        synced_     = true;
        best_idx_[0] = find_best(Side::Bid);
        best_idx_[1] = find_best(Side::Ask);
        return true;
    }

    // ---- Accessors ---------------------------------------------------------

    int64_t best_bid() const noexcept {
        return best_idx_[0] < 0 ? 0 : tick_min_ + best_idx_[0];
    }
    int64_t best_ask() const noexcept {
        return best_idx_[1] < 0 ? 0 : tick_min_ + best_idx_[1];
    }
    int64_t best_bid_qty() const noexcept {
        return best_idx_[0] < 0 ? 0
                                : qty_[0][static_cast<size_t>(best_idx_[0])];
    }
    int64_t best_ask_qty() const noexcept {
        return best_idx_[1] < 0 ? 0
                                : qty_[1][static_cast<size_t>(best_idx_[1])];
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

    void clear_storage() { // zero both sides
        std::memset(qty_[0].data(), 0, qty_[0].size() * sizeof(int64_t));
        std::memset(qty_[1].data(), 0, qty_[1].size() * sizeof(int64_t));
    }

    // Scan one side inward for the best level, starting at `start` (a slot
    // index). Bids scan downward (toward lower prices / lower indexes); asks
    // scan upward. Returns the slot index of the best filled level, or -1 if
    // the side is empty from `start` onward.
    //
    // Both rescan_after_delete() and find_best() are thin wrappers over this
    // single search primitive; they differ only in the starting slot:
    //   * rescan_after_delete starts adjacent to a just-deleted best (hot path,
    //     the common next-best is close by).
    //   * find_best starts at the far edge of the domain (cold path — after a
    //     snapshot wipes the book there is no prior best to resume from, so the
    //     whole domain must be re-examined).
    // `start` is the first slot considered. It may be exactly one past the
    // valid range (-1 for an empty bid side, tick_span_ for an empty ask side);
    // the loop condition handles those and the scan simply returns -1.
    int64_t scan_best_from(Side side, int64_t start) const noexcept {
        const size_t s = side_of(side);
        if (is_bid(side)) {
            for (int64_t i = start; i >= 0; --i) {
                if (qty_[s][static_cast<size_t>(i)] != 0) return i;
            }
        } else {
            for (int64_t i = start; i < tick_span_; ++i) {
                if (qty_[s][static_cast<size_t>(i)] != 0) return i;
            }
        }
        return -1;
    }

    // Next best level after the just-deleted best at slot `deleted`. Bids start
    // at deleted-1 (a lower price), asks at deleted+1 (a higher price).
    // Caller guarantees slot `deleted` is now zero.
    int64_t rescan_after_delete(Side side, int64_t deleted) const noexcept {
        return is_bid(side) ? scan_best_from(side, deleted - 1)
                            : scan_best_from(side, deleted + 1);
    }

    // Best level on `side` by a full-domain scan. Used ONLY on the cold path
    // (load_snapshot), where the whole book was just rebuilt and there is no
    // prior best to seed a localized scan: start at the far edge of the domain.
    int64_t find_best(Side side) const noexcept {
        return is_bid(side) ? scan_best_from(side, tick_span_ - 1)
                            : scan_best_from(side, 0);
    }

    // After a 0 -> positive write, promote the cache if the new level is better
    // than the current best (or the side has no cached best). Never called on a
    // positive -> positive write: a present level that is not the best cannot
    // become the best by re-quantification, so the call would be dead work.
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
