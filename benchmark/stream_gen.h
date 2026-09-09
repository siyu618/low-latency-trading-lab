// Deterministic workload-stream generator (Experiment 01).
//
// This is the SINGLE source of truth for the A/B/C/D/E workload semantics used
// by both the Phase 2 throughput benchmark (order_book_bench.cpp) and the
// Phase 4 tail-latency benchmark (order_book_tail_bench.cpp). Both executables
// include this header so that a Phase 4 stream for a given (workload, scale,
// seed, update count) is EXACTLY the stream Phase 2 would generate for the same
// (workload, scale, update count) under the default seed. The Phase 2 default
// seed is preserved verbatim: code that calls steady_ops(wl, n, updates)
// without a seed reproduces Phase 2's byte-for-byte op stream, so Phase 2
// results and semantics are unchanged.
//
// Price-domain model (shared with both benchmarks): domain is [1, 2N] where N
// is the requested scale in price levels. Each side starts with N live levels:
//   * bids occupy N+1 .. 2N   (best bid = 2N initially)
//   * asks occupy  1 .. N     (best ask = 1 initially)
// A candidate level `idx` on a side is idx steps from the touch (idx 0 == the
// best price), so both sides share one bookkeeping model. Workloads differ only
// in WHICH levels they touch and how often they delete the best. Live-level
// count over a run: A never changes the level set — it holds exactly N
// throughout. B/C/D are designed to RESTORE deleted levels and keep density near
// the starting N, but they do not pin the count to exactly N at every instant: a
// delete is undone only by a later refill, so any finite prefix may contain
// transient holes, and C in particular can finish with a small pending-hole
// deficit if the stream ends before the last vacated best is refilled (a finite
// run's exact end count is not hard-coded). E does NOT conserve levels — it
// deletes and adds at random, so occupancy may drift below the starting N; the
// exact finite-run value depends on scale, update count, the RNG stream, and
// the generator's retry/fallback behavior, and is NOT hard-coded anywhere (the
// Phase 2 --check pass prints the actual ending level counts of the generated
// stream). Every workload keeps the side far from empty, so each cell measures
// steady state, never a draining book.
//
//   A  update-only               every op re-quantifies a random present level;
//                                the level set never changes (exactly N)
//   B  10% deletes               each op picks a side; with 10% probability it
//                                deletes a random present level, otherwise it
//                                adds at a random ABSENT level (restoring the
//                                one that was deleted); density stays near N
//                                (each delete is later restored)
//   C  frequent best deletion    ~45% of ops delete the CURRENT best level
//                                (forcing the inward best re-scan); the rest
//                                refill the most recently vacated level, which
//                                restores it just below the current best, so
//                                the best churns across a few adjacent prices
//                                while density returns toward N (a finite
//                                stream may end with a few vacated-best holes
//                                still pending)
//   D  concentrated top-of-book  ops touch only a small window [0, N/128) at
//                                the best end; ~15% of ops delete a present
//                                window level, ~85% add at an absent window
//                                level (density inside the window is restored
//                                toward full; transient holes possible);
//                                levels below the window never move
//   E  uniformly random          fair side coin; price uniform over that side's
//                                whole region; 50% delete a present level, 50%
//                                add at an absent one; occupancy may drift
//                                below the starting N (not hard-coded)
//
// The stream is always generated BEFORE any clock starts (never inside a timed
// region), by a fixed-seed generator; the generator's own present_/best_ mirror
// is bookkeeping only and never reaches a timed book.

#pragma once

#include "types.h"

#include <cstdint>
#include <random>
#include <vector>

namespace llob_bench {

using llob::Side; // shorthand for the shared price-domain types

constexpr uint64_t kDefaultSeed = 0x5EED'C0FF'EEULL; // Phase 2 default seed

enum class Workload : int { A, B, C, D, E };
constexpr int kWorkloadCount = 5;

inline const char* workload_tag(Workload w) {
    switch (w) {
        case Workload::A: return "A";
        case Workload::B: return "B";
        case Workload::C: return "C";
        case Workload::D: return "D";
        case Workload::E: return "E";
    }
    return "?";
}

inline const char* workload_desc(Workload w) {
    switch (w) {
        case Workload::A: return "update-only";
        case Workload::B: return "10% deletes";
        case Workload::C: return "frequent best deletion";
        case Workload::D: return "concentrated top-of-book";
        case Workload::E: return "uniformly random";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Deterministic pre-stream generator.
//
// "Level idx" = a candidate price slot on one side, idx steps from the touch
// (idx 0 == the best price). present_[side][idx] records, in generator time
// only, whether that level is live in the book — so deletes can target levels
// that actually exist and adds can restore absent ones. Because the generator
// and the measured book receive the same ops, present_ is always an exact
// mirror of the book's live set. All of this is OFF the clock and never reaches
// a timed book.
//
// seed: pass the Phase 2 kDefaultSeed (the default) to reproduce the exact
// Phase 2 op stream for a (workload, scale, update-count) cell; any other seed
// yields a different-but-equally-deterministic stream of the SAME workload
// semantics. fill_snapshot() is RNG-free and identical for every seed.
// ---------------------------------------------------------------------------
class StreamGen {
public:
    // Cold-start snapshot: a full book with N live levels per side. The ONLY
    // way a fresh MapOrderBook/FlatOrderBook becomes usable is load_snapshot()
    // (both start unsynced and reject every apply() until then); an incremental
    // apply()-based fill is never valid on a fresh book. Deterministic and
    // identical for every impl/workload/seed.
    static llob::BookSnapshot fill_snapshot(int64_t n) {
        llob::BookSnapshot snap;
        snap.seq = static_cast<uint64_t>(2 * n); // steady stream continues at 2N+1
        snap.bids.prices.reserve(static_cast<size_t>(n));
        snap.bids.qtys.reserve(static_cast<size_t>(n));
        snap.asks.prices.reserve(static_cast<size_t>(n));
        snap.asks.qtys.reserve(static_cast<size_t>(n));
        for (int64_t idx = 0; idx < n; ++idx) {
            snap.bids.prices.push_back(price(Side::Bid, n, idx)); // 2N..N+1, best first
            snap.bids.qtys.push_back(qty_of(idx));
        }
        for (int64_t idx = 0; idx < n; ++idx) {
            snap.asks.prices.push_back(price(llob::Side::Ask, n, idx)); // 1..N, best first
            snap.asks.qtys.push_back(qty_of(idx));
        }
        return snap;
    }

    // Full steady-state op stream. Sequence numbers begin right after the fill
    // snapshot (at 2N+1), so the stream can be replayed immediately after
    // load_snapshot(fill_snapshot(n)). `seed` defaults to the Phase 2 seed; see
    // the class comment.
    static std::vector<llob::L2Update> steady_ops(Workload wl, int64_t n,
                                                  uint64_t updates,
                                                  uint64_t seed = kDefaultSeed) {
        StreamGen g(wl, n, seed);
        std::vector<llob::L2Update> ops;
        ops.reserve(static_cast<size_t>(updates));
        for (uint64_t i = 0; i < updates; ++i) ops.push_back(g.next());
        return ops;
    }

private:
    static int64_t qty_of(int64_t idx) { return 10 + (idx % 997); }

    // Candidate price for level `idx` on `side` under scale `n`.
    static int64_t price(llob::Side s, int64_t n, int64_t idx) {
        return llob::is_bid(s) ? 2 * n - idx : 1 + idx;
    }

    StreamGen(Workload wl, int64_t n, uint64_t seed)
        : wl_(wl)
        , n_(n)
        , seq_(static_cast<uint64_t>(2 * n)) // continues after the 2N fill ops
        , rng_(seed ^ static_cast<uint64_t>(n)) {
        // Top-of-book window (C refill radius, D touch band): a small slice of
        // N, but never tiny and never larger than N.
        win_ = n / 128;
        if (win_ < 16) win_ = 16;
        if (win_ > n) win_ = n;
        present_[0].assign(static_cast<size_t>(n), 1);
        present_[1].assign(static_cast<size_t>(n), 1);
        best_[0] = 0;
        best_[1] = 0;
    }

    static size_t side_i(Side s) { return llob::is_bid(s) ? 0u : 1u; }

    // Uniform integer in [lo, hi] (inclusive). Requires lo <= hi.
    int64_t rnd(int64_t lo, int64_t hi) {
        std::uniform_int_distribution<int64_t> d(lo, hi);
        return d(rng_);
    }

    // Uniform PRESENT level index on a side. present_ stays dense, so retries
    // are rare; the fallback only triggers on an effectively-full side.
    int64_t rnd_present(size_t si) {
        for (int tries = 0; tries < 4096; ++tries) {
            const int64_t idx = rnd(0, n_ - 1);
            if (present_[si][static_cast<size_t>(idx)]) return idx;
        }
        return 0;
    }

    // Uniform ABSENT level index on a side, or -1 when the side is full.
    int64_t rnd_absent(size_t si) {
        for (int tries = 0; tries < 4096; ++tries) {
            const int64_t idx = rnd(0, n_ - 1);
            if (!present_[si][static_cast<size_t>(idx)]) return idx;
        }
        return -1;
    }

    llob::L2Update make(Side s, int64_t idx, int64_t qty) {
        return llob::L2Update{++seq_, price(s, n_, idx), qty, s};
    }

    // Mirror operations on present_/best_. delete_level removes a present level
    // (caller guarantees present); add_level restores an absent one.
    void delete_level(size_t si, int64_t idx) {
        present_[si][static_cast<size_t>(idx)] = 0;
        if (idx == best_[si]) {
            // The best was removed: rescan upward for the next present level.
            int64_t j = idx + 1;
            while (j < n_ && !present_[si][static_cast<size_t>(j)]) ++j;
            best_[si] = (j < n_) ? j : -1;
        }
    }

    void add_level(size_t si, int64_t idx) {
        present_[si][static_cast<size_t>(idx)] = 1;
        if (best_[si] < 0 || idx < best_[si]) best_[si] = idx;
    }

    // One steady-state op per the workload's distribution.
    llob::L2Update next() {
        const llob::Side s = (rnd(0, 1) == 0) ? llob::Side::Bid : llob::Side::Ask;
        const size_t si = side_i(s);
        switch (wl_) {
            case Workload::A: {
                // Re-quantify a random present level; the level set never moves.
                return make(s, rnd_present(si), qty_of(rnd(0, n_ - 1)));
            }
            case Workload::B: {
                if (rnd(0, 9) < 1) { // 10% delete a present level
                    const int64_t idx = rnd_present(si);
                    delete_level(si, idx);
                    return make(s, idx, 0);
                }
                // 90% add: restore an absent level first so density stays ~N.
                if (const int64_t a = rnd_absent(si); a >= 0) {
                    add_level(si, a);
                    return make(s, a, qty_of(rnd(0, n_ - 1)));
                }
                return make(s, rnd_present(si), qty_of(rnd(0, n_ - 1)));
            }
            case Workload::C: {
                // Best-price churn. Deleting the best level of a FULL side only
                // keeps density near N if each delete is matched by a refill of
                // the vacated level (any other add at full density is a
                // re-quantify and the side would drain — see probes in the
                // commit notes). So the delete rate must not exceed the refill
                // rate:
                //   ~45% of ops delete the CURRENT best level (forces the
                //         inward best re-scan on the flat book);
                //   ~55% refill the most recently vacated best (LIFO hole), which
                //         restores it just below the current best, so the best
                //         churns between a few adjacent prices while density
                //         returns toward N. A finite stream can end with a few
                //         pending holes (deleted bests not yet refilled), so the
                //         ending live count need not be exactly N. A refill with
                //         no pending hole is a re-quantify of the best region.
                const int64_t best = best_[si]; // -1 only if the side emptied
                if (best >= 0 && rnd(0, 99) < 45) {
                    delete_level(si, best);
                    holes_[si].push_back(best);
                    return make(s, best, 0);
                }
                if (!holes_[si].empty()) {
                    const int64_t idx = holes_[si].back(); // newest hole (just below best)
                    holes_[si].pop_back();
                    add_level(si, idx);
                    return make(s, idx, qty_of(rnd(0, n_ - 1)));
                }
                // Book full, no pending hole: re-quantify a present near-best level.
                const int64_t a = best >= 0 ? best : 0;
                return make(s, a, qty_of(rnd(0, n_ - 1)));
            }
            case Workload::D: {
                // Touch band only: levels [0, win_).
                if (rnd(0, 99) < 15) { // 15% delete a present window level
                    for (int tries = 0; tries < 128; ++tries) {
                        const int64_t idx = rnd(0, win_ - 1);
                        if (present_[si][static_cast<size_t>(idx)]) {
                            delete_level(si, idx);
                            return make(s, idx, 0);
                        }
                    }
                    return make(s, rnd_present(si), qty_of(rnd(0, n_ - 1)));
                }
                // 85% add in the window; restore an absent window level first.
                for (int tries = 0; tries < 128; ++tries) {
                    const int64_t idx = rnd(0, win_ - 1);
                    if (!present_[si][static_cast<size_t>(idx)]) {
                        add_level(si, idx);
                        return make(s, idx, qty_of(rnd(0, n_ - 1)));
                    }
                }
                const int64_t idx = rnd(0, win_ - 1); // window is full: re-quantify
                return make(s, idx, qty_of(rnd(0, n_ - 1)));
            }
            case Workload::E: {
                // Uniformly random over the whole side region.
                if (rnd(0, 1) == 0) { // 50% delete a present level
                    const int64_t idx = rnd_present(si);
                    delete_level(si, idx);
                    return make(s, idx, 0);
                }
                if (const int64_t a = rnd_absent(si); a >= 0) { // 50% restore
                    add_level(si, a);
                    return make(s, a, qty_of(rnd(0, n_ - 1)));
                }
                return make(s, rnd_present(si), qty_of(rnd(0, n_ - 1)));
            }
        }
        return llob::L2Update{0, 0, 0, llob::Side::Bid}; // unreachable
    }

    Workload             wl_;
    int64_t              n_;
    int64_t              win_ = 0;
    int64_t              best_[2] = {0, 0};
    uint64_t             seq_;
    std::mt19937_64      rng_;
    std::vector<uint8_t> present_[2];
    std::vector<int64_t> holes_[2]; // workload C: vacated best slots awaiting refill
};

} // namespace llob_bench
