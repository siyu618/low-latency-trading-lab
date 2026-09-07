#pragma once

#include "types.h"

#include <map>

namespace llob {

// ---------------------------------------------------------------------------
// MapOrderBook — baseline L2 book built on std::map.
//
// Bids live in one map ordered by descending price (so bid.rbegin() is the
// best bid); asks in another ordered by ascending price (ask.begin() is the
// best ask). This is the reference implementation used as a correctness
// oracle for FlatOrderBook and as the benchmark baseline.
//
// std::map is a node-based red-black tree: every inserted price level is a
// heap allocation, and reads/updates chase pointers. That is precisely the
// cost model we want to measure against a flat representation.
// ---------------------------------------------------------------------------
class MapOrderBook final {
public:
    // Tick-domain endpoints the map book will accept. Kept symmetric with
    // FlatOrderBook's bounds so the two books can be fed identical streams.
    static constexpr int64_t kMinTick = 1;
    static constexpr int64_t kMaxTick = 10'000'000'000;

    using Bids = std::map<int64_t, int64_t, std::greater<int64_t>>; // desc; begin() = best bid
    using Asks = std::map<int64_t, int64_t, std::less<int64_t>>;    // asc;  begin() = best ask

    ApplyResult apply(const L2Update& u) {
        if (u.seq <= seq_) {
            return ApplyResult::Stale; // replayed or older than applied state
        }
        if (!synced_) {
            // Already desynchronized by an earlier gap; every update is ignored
            // until load_snapshot() restores a consistent baseline.
            return ApplyResult::Stale;
        }
        if (u.seq != seq_ + 1) {
            synced_ = false;
            return ApplyResult::GapDetected;
        }

        if (u.price < kMinTick || u.price > kMaxTick) {
            return ApplyResult::Stale; // outside the domain the book can hold
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

    void load_snapshot(const BookSnapshot& s) {
        bids_.clear();
        asks_.clear();

        const auto& bp = s.bids.prices;
        const auto& bq = s.bids.qtys;
        for (size_t i = 0; i < bp.size() && i < bq.size(); ++i) {
            if (bp[i] >= kMinTick && bp[i] <= kMaxTick && bq[i] > 0) {
                bids_[bp[i]] = bq[i];
            }
        }
        const auto& ap = s.asks.prices;
        const auto& aq = s.asks.qtys;
        for (size_t i = 0; i < ap.size() && i < aq.size(); ++i) {
            if (ap[i] >= kMinTick && ap[i] <= kMaxTick && aq[i] > 0) {
                asks_[ap[i]] = aq[i];
            }
        }
        seq_   = s.seq;
        synced_ = true;
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
    uint64_t seq_    = 0; // last applied sequence number
    bool     synced_ = false;
};

} // namespace llob
