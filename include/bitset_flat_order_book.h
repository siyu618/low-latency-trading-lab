#pragma once

#include "types.h"

#include <bit>       // std::countl_zero / std::countr_zero (C++20)
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace llob {

// ---------------------------------------------------------------------------
// BitsetFlatOrderBook — dense, preallocated, tick-addressed L2 book whose
// occupancy is ALSO tracked in a hierarchical bitmap (Experiment 01
// Optimization Study: "Hierarchical Occupancy Bitmap for Flat best-price
// discovery").
//
// It has the SAME externally visible semantics as FlatOrderBook (this is a
// drop-in alternative, NOT a subclass — there is no inheritance or
// polymorphism anywhere in this code base). Same price domain, same integer
// tick addressing, same absolute-quantity storage with qty == 0 deleting the
// level, same best bid/ask caching, same sequence validation / gap /
// invalid-update / out-of-range behavior, same snapshot loading and syncing
// rules, same single-writer apply contract. Quantity storage is the same
// preallocated contiguous int64 array as FlatOrderBook.
//
// WHAT the bitmap adds: FlatOrderBook discovers the next best price after
// deleting the current best by LINEARLY scanning empty slots inward from the
// adjacent slot (scan_best_from). That scan is O(gap) — cheap when the next
// level sits in the adjacent slot (workload C's common case), expensive when
// the side has large holes (sparse books). This book instead keeps, per side,
// a three-level occupancy summary over the SAME slot space as the qty array:
//
//   L0   one bit per price level (slot).  A bit is set iff qty[slot] != 0.
//   L1   one bit per non-empty L0 uint64 word (i.e. per 64 consecutive slots).
//   L2   one bit per non-empty L1 uint64 word.
//
//   Worked word counts for a ~1,000,000-level per-side span
//   (span / 64 = 15,625 L0 words; L1 = ceil(15625/64) = 245; L2 =
//   ceil(245/64) = 4). The counts below are computed from the actual
//   configured span at construction (the benchmark cells construct the book
//   over [1, 2N], so at scale N the per-side slot span is 2N — twice the
//   live-level count — and the word counts double accordingly).
//
// The bitmap is a PURE OCCUPANCY index — quantities are never encoded in it.
// A level's quantity lives only in qty_[]. The bitmap answers exactly one
// question: "what is the highest (bid) / lowest (ask) occupied slot
// below/above a given slot?" It never linearly inspects the empty PRICE SLOTS
// after leaving the current L0 word (that is the per-slot scan being avoided).
// The descent is a bounded few word steps in the common case (same L0 word, or
// one L1/L2 step). Worst case it scans a small number of SUMMARY words: the
// L2-level fallback in prev_occ2()/next_occ2() linearly walks adjacent occ2
// words, and occ2 is tiny by construction — ceil(L1words/64) words per side,
// e.g. 8 words at a 2M-slot-per-side domain (see those functions).
//
// MAINTENANCE DISCIPLINE (the whole point): the bitmap is updated ONLY on an
// occupancy transition:
//   * 0 -> positive : set the L0 bit; if that L0 word went zero -> nonzero,
//                     set the L1 bit; propagate upward only if that L1 word
//                     went zero -> nonzero.
//   * positive -> 0 : clear the L0 bit; if that L0 word went nonzero -> zero,
//                     clear the L1 bit; propagate upward only as needed.
//   * positive -> positive : the bitmap is NOT touched at all. Re-quantifying
//                     an already-present level (workload A's only operation)
//                     therefore costs nothing in the bitmap, keeping ordinary
//                     updates a clean A-vs-A comparison against FlatOrderBook.
//
// Best-price discovery (hot path, best deletion only):
//   * Bid: deleting the current best bid (the highest occupied slot) searches
//     the LOWER bits of the same L0 word first; if none, it walks the
//     hierarchy (L0 -> L1 -> L2) to the preceding non-empty L0 word and
//     descends to the exact occupied bit. No linear slot scan.
//   * Ask: mirrored toward HIGHER slots.
//   * Word boundaries, hierarchy boundaries, the first/last domain level, and
//     "no occupied level remains" (side empties -> best index -1) are handled
//     explicitly.
//
// WHY this exists (the research question): for a FlatOrderBook whose next-best
// price is adjacent, deleting the best is ~free (the linear scan is one slot).
// The bitmap can only pay for itself when best deletion has to skip a gap —
// and it always adds a little work on occupancy-changing updates (two extra
// stores + possible upward propagation). Whether there is a gap at which the
// hierarchical lookup beats the adjacent linear scan is a MEASUREMENT question
// answered by the dedicated gap benchmark and docs/ORDERBOOK_BITMAP_OPTIMIZATION.md;
// this header makes no universal "faster" claim.
//
// Allocation: like FlatOrderBook, apply() performs no dynamic allocation after
// construction; load_snapshot() is the cold path (it validates first, which
// sorts prices into a temporary vector, then rebuilds qty + occupancy from
// scratch).
// ---------------------------------------------------------------------------
class BitsetFlatOrderBook final {
public:
    BitsetFlatOrderBook(int64_t tick_min = kDefaultTickMin,
                        int64_t tick_max = kDefaultTickMax)
        : tick_min_(tick_min)
        , tick_span_(tick_max - tick_min + 1) {
        if (tick_span_ <= 0) {
            throw std::invalid_argument(
                "BitsetFlatOrderBook: tick_max must be >= tick_min");
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

        const size_t s = side_of(u.side);
        // idx is in [0, tick_span_) here (validated just above).
        auto& qty = qty_[s][static_cast<size_t>(idx)];
        if (u.qty == 0) {
            if (qty == 0) {
                // Deleting a level that is not present: nothing to remove from
                // the bitmap (its bit is already clear). Just advance seq.
                seq_ = u.seq;
                return ApplyResult::Applied;
            }
            qty = 0;
            const bool is_best = (idx == best_idx_[s]);
            clear_occ(s, idx);
            if (is_best) {
                // Removed the level the cache points at. Locate the next best
                // with the occupancy hierarchy (bid: highest occupied slot
                // below; ask: lowest occupied slot above), so best reads stay
                // O(1) and the delete pays only the hierarchy descent.
                best_idx_[s] = is_bid(u.side) ? prev_occupied_slot(s, idx)
                                              : next_occupied_slot(s, idx);
            }
        } else {
            if (qty == 0) {
                qty = u.qty;
                set_occ(s, idx); // 0 -> positive: bitmap transition
                if (!is_best_slot(s, idx)) {
                    update_best_if_needed(s, idx);
                }
                // If it *is* the best we keep the cache: same price, new qty
                // (a present best re-quantified as positive never changes the
                // price); positive->positive never touches the bitmap.
            } else {
                qty = u.qty; // positive -> positive: bitmap untouched
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

        clear_storage(); // zero qty AND all occupancy words, both sides

        for (size_t i = 0; i < s.bids.prices.size(); ++i) {
            const int64_t q = s.bids.qtys[i];
            if (q == 0) continue; // qty 0 == no level; validated >= 0 already
            const size_t idx = static_cast<size_t>(s.bids.prices[i] - tick_min_);
            qty_[0][idx]     = q;
            set_occ(0, static_cast<int64_t>(idx));
        }
        for (size_t i = 0; i < s.asks.prices.size(); ++i) {
            const int64_t q = s.asks.qtys[i];
            if (q == 0) continue;
            const size_t idx = static_cast<size_t>(s.asks.prices[i] - tick_min_);
            qty_[1][idx]     = q;
            set_occ(1, static_cast<int64_t>(idx));
        }

        seq_          = s.seq;
        synced_       = true;
        best_idx_[0]  = prev_occupied_slot(0, tick_span_); // highest bid slot
        best_idx_[1]  = next_occupied_slot(1, -1);         // lowest ask slot
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

    // ---- Memory accounting (Optimization Study: item 11) -------------------
    // The occupancy hierarchy is never free; these helpers report exactly how
    // much it costs so the doc can compare it to the qty-array bytes instead of
    // calling it zero. Occupancy bytes = sum over both sides of
    // (L0 + L1 + L2) uint64 words.
    size_t quantity_bytes() const noexcept {
        return qty_[0].size() * sizeof(int64_t) + qty_[1].size() * sizeof(int64_t);
    }
    size_t occupancy_bytes() const noexcept {
        size_t n = 0;
        for (int s = 0; s < 2; ++s) {
            n += occ0_[s].size() * sizeof(uint64_t); // L0
            n += occ1_[s].size() * sizeof(uint64_t); // L1
            n += occ2_[s].size() * sizeof(uint64_t); // L2
        }
        return n;
    }
    // Per-side hierarchy word counts (L0 / L1 / L2), for reporting.
    void hierarchy_words(size_t out[3]) const noexcept {
        out[0] = occ0_[0].size();
        out[1] = occ1_[0].size();
        out[2] = occ2_[0].size();
    }

private:
    static constexpr size_t side_of(Side s) noexcept {
        return s == Side::Bid ? 0u : 1u;
    }

    // Is slot index `i` the currently cached best on side s?
    bool is_best_slot(size_t s, int64_t i) const noexcept {
        return i == best_idx_[s];
    }

    // msb / lsb positions of a NONZERO uint64 (0..63).
    static constexpr int msb_index(uint64_t x) noexcept {
        return 63 - std::countl_zero(x);
    }
    static constexpr int lsb_index(uint64_t x) noexcept {
        return std::countr_zero(x);
    }

    // Masks over the low/high bits of one uint64, avoiding UB shifts.
    // low_mask(p):  bits p..0 set (p in [0,63]);  p==63 -> all ones.
    // high_mask(p): bits 63..p set (p in [0,63]); p==0  -> all ones.
    static constexpr uint64_t low_mask(unsigned p) noexcept {
        return (p == 63) ? ~uint64_t{0} : ((uint64_t{1} << (p + 1)) - 1);
    }
    static constexpr uint64_t high_mask(unsigned p) noexcept {
        return (p == 0) ? ~uint64_t{0} : ~((uint64_t{1} << p) - 1);
    }

    // ---- Occupancy maintenance (transition-only; see class comment) --------

    void set_occ(size_t s, int64_t slot) noexcept {
        const size_t w  = static_cast<size_t>(slot >> 6);
        const unsigned p = static_cast<unsigned>(slot) & 63u;
        const uint64_t old = occ0_[s][w];
        const uint64_t bit = uint64_t{1} << p;
        if (old & bit) return; // already occupied (defensive; never on hot path)
        occ0_[s][w] = old | bit;
        if (old == 0) {
            // This L0 word went zero -> nonzero: set its L1 summary bit.
            const size_t w1  = w >> 6;
            const unsigned p1 = static_cast<unsigned>(w) & 63u;
            const uint64_t old1 = occ1_[s][w1];
            occ1_[s][w1] = old1 | (uint64_t{1} << p1);
            if (old1 == 0) {
                // This L1 word went zero -> nonzero: set its L2 summary bit.
                occ2_[s][w1 >> 6] |= (uint64_t{1} << (static_cast<unsigned>(w1) & 63u));
            }
        }
    }

    void clear_occ(size_t s, int64_t slot) noexcept {
        const size_t w  = static_cast<size_t>(slot >> 6);
        const unsigned p = static_cast<unsigned>(slot) & 63u;
        occ0_[s][w] &= ~(uint64_t{1} << p);
        if (occ0_[s][w] == 0) {
            // This L0 word went nonzero -> zero: clear its L1 summary bit.
            const size_t w1  = w >> 6;
            const unsigned p1 = static_cast<unsigned>(w) & 63u;
            occ1_[s][w1] &= ~(uint64_t{1} << p1);
            if (occ1_[s][w1] == 0) {
                // This L1 word went nonzero -> zero: clear its L2 summary bit.
                occ2_[s][w1 >> 6] &= ~(uint64_t{1} << (static_cast<unsigned>(w1) & 63u));
            }
        }
    }

    // ---- Hierarchical search primitives (one side s) ----------------------
    // All bounds are exclusive/inclusive as documented; "occupied" always means
    // "the level's qty != 0", which the bitmap mirrors exactly.

    // Largest L2 bit < b  (L2 bit j set <=> L1 word j non-empty).
    int64_t prev_occ2(size_t s, int64_t b) const noexcept {
        if (b <= 0) return -1;
        const int64_t w = (b - 1) >> 6;
        const unsigned p = static_cast<unsigned>((b - 1) & 63);
        const uint64_t x = occ2_[s][static_cast<size_t>(w)] & low_mask(p);
        if (x) return (w << 6) + msb_index(x);
        // Previous non-empty occ2 word. occ2 has ceil(L1words/64) words per
        // side (e.g. 8 at a 2M-slot-per-side domain), so this fallback is a
        // LINEAR scan over a handful of SUMMARY words — not the per-slot linear
        // scan over empty price levels that the hierarchy exists to avoid. It
        // is bounded and tiny; there is deliberately no fourth (L3) level.
        for (int64_t ww = w - 1; ww >= 0; --ww) {
            const uint64_t y = occ2_[s][static_cast<size_t>(ww)];
            if (y) return (ww << 6) + msb_index(y);
        }
        return -1;
    }

    // Largest L1 bit < b  (L1 bit j set <=> L0 word j non-empty).
    int64_t prev_occ1(size_t s, int64_t b) const noexcept {
        if (b <= 0) return -1;
        const int64_t w = (b - 1) >> 6;
        const unsigned p = static_cast<unsigned>((b - 1) & 63);
        const uint64_t x = occ1_[s][static_cast<size_t>(w)] & low_mask(p);
        if (x) return (w << 6) + msb_index(x);
        if (w == 0) return -1;
        const int64_t j = prev_occ2(s, w); // largest non-empty L1 word index < w
        if (j < 0) return -1;
        return (j << 6) + msb_index(occ1_[s][static_cast<size_t>(j)]);
    }

    // Largest L0 bit (occupied slot) strictly below `b` on side s, or -1.
    int64_t prev_occupied_slot(size_t s, int64_t b) const noexcept {
        if (b <= 0) return -1;
        const int64_t w = (b - 1) >> 6;
        const unsigned p = static_cast<unsigned>((b - 1) & 63);
        const uint64_t x = occ0_[s][static_cast<size_t>(w)] & low_mask(p);
        if (x) return (w << 6) + msb_index(x);
        if (w == 0) return -1;
        const int64_t j = prev_occ1(s, w); // largest non-empty L0 word index < w
        if (j < 0) return -1;
        return (j << 6) + msb_index(occ0_[s][static_cast<size_t>(j)]);
    }

    // Smallest L2 bit >= lb, or -1. occ2 has one meaningful bit per L1 word,
    // i.e. occ1_[s].size() meaningful bits (higher bits are never set).
    int64_t next_occ2(size_t s, int64_t lb) const noexcept {
        const int64_t limit = static_cast<int64_t>(occ1_[s].size());
        if (lb >= limit) return -1;
        const int64_t w = lb >> 6;
        const unsigned p = static_cast<unsigned>(lb & 63);
        const uint64_t x = occ2_[s][static_cast<size_t>(w)] & high_mask(p);
        if (x) return (w << 6) + lsb_index(x);
        for (int64_t ww = w + 1; ww < static_cast<int64_t>(occ2_[s].size()); ++ww) {
            const uint64_t y = occ2_[s][static_cast<size_t>(ww)];
            if (y) return (ww << 6) + lsb_index(y);
        }
        return -1;
    }

    // Smallest L1 bit >= lb, or -1.
    int64_t next_occ1(size_t s, int64_t lb) const noexcept {
        const int64_t l0w = static_cast<int64_t>(occ0_[s].size());
        if (lb >= l0w) return -1;
        const int64_t w = lb >> 6;
        const unsigned p = static_cast<unsigned>(lb & 63);
        const uint64_t x = occ1_[s][static_cast<size_t>(w)] & high_mask(p);
        if (x) return (w << 6) + lsb_index(x);
        const int64_t j = next_occ2(s, w + 1); // first non-empty L1 word index >= w+1
        if (j < 0) return -1;
        return (j << 6) + lsb_index(occ1_[s][static_cast<size_t>(j)]);
    }

    // Smallest occupied slot strictly above `a` on side s, or -1.
    int64_t next_occupied_slot(size_t s, int64_t a) const noexcept {
        if (a + 1 >= tick_span_) return -1;
        const int64_t start = a + 1;
        const int64_t w = start >> 6;
        const unsigned p = static_cast<unsigned>(start & 63);
        const uint64_t x = occ0_[s][static_cast<size_t>(w)] & high_mask(p);
        if (x) return (w << 6) + lsb_index(x);
        const int64_t j = next_occ1(s, w + 1); // first non-empty L0 word >= w+1
        if (j < 0) return -1;
        return (j << 6) + lsb_index(occ0_[s][static_cast<size_t>(j)]);
    }

    // After a 0 -> positive write at a NON-best slot, promote the cache if the
    // new level is better than the current best (or the side had no best).
    void update_best_if_needed(size_t s, int64_t idx) noexcept {
        const int64_t cur = best_idx_[s];
        if (cur < 0) {
            best_idx_[s] = idx;
            return;
        }
        if (s == 0) { // bids: higher price (higher slot) is better
            if (idx > cur) best_idx_[s] = idx;
        } else { // asks: lower price (lower slot) is better
            if (idx < cur) best_idx_[s] = idx;
        }
    }

    void allocate() {
        const int64_t span = tick_span_;
        const size_t n     = static_cast<size_t>(span);
        const size_t w0    = (n + 63) / 64; // L0 words (ceil bits/64)
        const size_t w1    = (w0 + 63) / 64; // L1 words
        const size_t w2    = (w1 + 63) / 64; // L2 words
        qty_[0].assign(n, 0);
        qty_[1].assign(n, 0);
        occ0_[0].assign(w0, 0);
        occ0_[1].assign(w0, 0);
        occ1_[0].assign(w1, 0);
        occ1_[1].assign(w1, 0);
        occ2_[0].assign(w2, 0);
        occ2_[1].assign(w2, 0);
    }

    void clear_storage() { // zero qty and all occupancy words, both sides
        std::memset(qty_[0].data(), 0, qty_[0].size() * sizeof(int64_t));
        std::memset(qty_[1].data(), 0, qty_[1].size() * sizeof(int64_t));
        std::memset(occ0_[0].data(), 0, occ0_[0].size() * sizeof(uint64_t));
        std::memset(occ0_[1].data(), 0, occ0_[1].size() * sizeof(uint64_t));
        std::memset(occ1_[0].data(), 0, occ1_[0].size() * sizeof(uint64_t));
        std::memset(occ1_[1].data(), 0, occ1_[1].size() * sizeof(uint64_t));
        std::memset(occ2_[0].data(), 0, occ2_[0].size() * sizeof(uint64_t));
        std::memset(occ2_[1].data(), 0, occ2_[1].size() * sizeof(uint64_t));
    }

    int64_t tick_min_;
    int64_t tick_span_;      // addressable slots per side
    std::vector<int64_t> qty_[2];    // [side][price - tick_min_] = level qty
    std::vector<uint64_t> occ0_[2];  // L0: 1 bit per slot (set iff qty != 0)
    std::vector<uint64_t> occ1_[2];  // L1: 1 bit per non-empty L0 word
    std::vector<uint64_t> occ2_[2];  // L2: 1 bit per non-empty L1 word
    int64_t best_idx_[2] = {-1, -1}; // cached best slot per side
    uint64_t seq_    = 0;
    bool     synced_ = false;
};

} // namespace llob
