#pragma once

#include "types.h"

#include <map>
#include <stdexcept>

namespace llob {

// ---------------------------------------------------------------------------
// MapOrderBook — baseline L2 book built on std::map.
//
// Bids live in one map ordered by descending price; asks in another ordered by
// ascending price. With a descending comparator the largest bid is at
// begin(), so begin() is the best bid for both sides. This is the reference
// implementation used as a correctness oracle for FlatOrderBook and as the
// benchmark baseline.
//
// std::map is a node-based red-black tree: every inserted price level is a
// heap allocation, and reads/updates chase pointers. That is precisely the
// cost model we want to measure against a flat representation.
//
// Price-domain semantics are identical to FlatOrderBook: the map is configured
// over [tick_min, tick_max], accepts the same apply()/load_snapshot() results,
// and must be constructed with the SAME domain as any FlatOrderBook it is
// compared against.
// ---------------------------------------------------------------------------
class MapOrderBook final {
public:
    // Tick-domain endpoints the map book will accept. Defaults are shared with
    // FlatOrderBook (see types.h); construct with the same domain for parity.
    explicit MapOrderBook(int64_t tick_min = kDefaultTickMin,
                          int64_t tick_max = kDefaultTickMax)
        : tick_min_(tick_min), tick_max_(tick_max) {
        if (tick_max < tick_min) {
            throw std::invalid_argument("MapOrderBook: tick_max must be >= tick_min");
        }
    }

    using Bids = std::map<int64_t, int64_t, std::greater<int64_t>>; // desc; begin() = best bid
    using Asks = std::map<int64_t, int64_t, std::less<int64_t>>;    // asc;  begin() = best ask

    int64_t tick_min() const noexcept { return tick_min_; }
    int64_t tick_max() const noexcept { return tick_max_; }

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
        if (u.price < tick_min_ || u.price > tick_max_) {
            // Outside this book's configured domain. Ignored, but the sequence
            // IS consumed to keep the view contiguous.
            seq_ = u.seq;
            return ApplyResult::OutOfRange;
        }

        seq_ = u.seq;
        if (is_bid(u.side)) {
            if (u.qty == 0) {
                bids_.erase(u.price); // idempotent if absent
            } else {
                // std::map keeps its key order regardless of the value, so a
                // blind insert_or_assign is correct even when re-pricing the
                // current best.
                bids_.insert_or_assign(u.price, u.qty);
            }
        } else {
            if (u.qty == 0) {
                asks_.erase(u.price);
            } else {
                asks_.insert_or_assign(u.price, u.qty);
            }
        }
        return ApplyResult::Applied;
    }

    // Replaces book state from `s`. Returns false (leaving the book completely
    // unchanged) if the snapshot is malformed; cold path, correctness first.
    bool load_snapshot(const BookSnapshot& s) {
        if (!validate_snapshot(s, tick_min_, tick_max_)) {
            return false; // reject, do not partially load, do not touch synced()
        }

        bids_.clear();
        asks_.clear();
        for (size_t i = 0; i < s.bids.prices.size(); ++i) {
            const int64_t q = s.bids.qtys[i];
            if (q == 0) continue; // qty 0 == no level; validated >= 0 already
            bids_[s.bids.prices[i]] = q;
        }
        for (size_t i = 0; i < s.asks.prices.size(); ++i) {
            const int64_t q = s.asks.qtys[i];
            if (q == 0) continue;
            asks_[s.asks.prices[i]] = q;
        }
        seq_   = s.seq;
        synced_ = true;
        return true;
    }

    // Best prices, cached as map endpoints (O(1), no side effects). With the
    // descending bid comparator the best bid is the *first* element, so begin()
    // is correct for both sides.
    int64_t best_bid() const noexcept { return bids_.empty() ? 0 : bids_.begin()->first; }
    int64_t best_ask() const noexcept { return asks_.empty() ? 0 : asks_.begin()->first; }
    int64_t best_bid_qty() const noexcept { return bids_.empty() ? 0 : bids_.begin()->second; }
    int64_t best_ask_qty() const noexcept { return asks_.empty() ? 0 : asks_.begin()->second; }

    bool      synced() const noexcept { return synced_; }
    uint64_t  next_expected_seq() const noexcept { return seq_ + 1; }
    uint64_t  last_applied_seq() const noexcept { return seq_; }
    bool      empty() const noexcept { return bids_.empty() && asks_.empty(); }

    // Introspection / testing.
    const Bids& bids() const noexcept { return bids_; }
    const Asks& asks() const noexcept { return asks_; }
    size_t level_count() const noexcept { return bids_.size() + asks_.size(); }

private:
    Bids     bids_;
    Asks     asks_;
    int64_t  tick_min_;
    int64_t  tick_max_;
    uint64_t seq_    = 0; // last applied sequence number
    bool     synced_ = false;
};

} // namespace llob
