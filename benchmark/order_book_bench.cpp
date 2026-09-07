// Deterministic order-book benchmark (Experiment 01, Phase 2).
//
// What this measures: steady-state apply() throughput of ONE L2 book
// implementation over pre-recorded, well-formed update streams. It does NOT
// measure RNG cost, stream construction, book cold-start, or best-price reads —
// all of that happens before the timed region or is excluded by construction
// (the full update stream is generated up front, once, off the clock, by a
// fixed-seed generator).
//
// Methodology (deliberate — matches the Phase 2 contract):
//   * The whole stream for (impl, workload, scale) is generated BEFORE any
//     timer starts. The timed path does not allocate, and the generator is not
//     on the clock.
//   * Each timed block has two phases on a FRESH book:
//       - cold start: load_snapshot() of a full N-levels-per-side snapshot,
//                     untimed. (A fresh book is unsynced and rejects every
//                     apply() until a snapshot, so an incremental warm-up is
//                     impossible.) This is the state each measurement starts from.
//       - timed:      replay the pre-built steady op stream. The stream's
//                     sequence numbers continue exactly after the snapshot's
//                     (seq 2N), so every timed op is Applied (validated by
//                     --check).
//   * One block = (impl, workload, scale, rep). The identical op stream is
//     replayed for every rep and for BOTH books, so any time difference is
//     purely the book implementation. Reported time is the best (min) of the
//     reps blocks — a common low-latency summary that discounts scheduling
//     noise (which only ever adds latency).
//   * The stream never contains a negative qty or an out-of-domain price, and
//     seq advances by exactly 1, so it is legal for both books.
//
// Price-domain model: domain is [1, 2N] where N is the requested scale in price
// levels. Each side carries ~N live levels:
//   * bids occupy N+1 .. 2N   (best bid = 2N initially)
//   * asks occupy  1 .. N     (best ask = 1 initially)
// A candidate level `idx` on a side is idx steps from the touch (idx 0 == the
// best price), so both sides share one bookkeeping model. Workloads differ only
// in WHICH levels they touch and how often they delete the best; every workload
// keeps its side dense (~N live levels), so each cell measures steady state,
// never a draining book.
//
//   A  update-only               every op re-quantifies a random present level
//                                (no level ever appears or disappears)
//   B  10% deletes               every 10th op deletes a random present level;
//                                the rest restore a random absent level, so
//                                density stays at ~N
//   C  frequent best deletion    ~45% of ops delete the CURRENT best level
//                                (forces the inward best re-scan); the rest
//                                refill the vacated level just below the new
//                                best, so the best churns across a few adjacent
//                                prices while the side stays at ~N levels
//   D  concentrated top-of-book  ops touch only a small window [0, N/128) at
//                                the best end; ~15% deletes inside the window
//   E  uniformly random          fair side coin; price uniform over that side's
//                                whole region; half delete a present level /
//                                half restore an absent one
//
// Usage:
//   orderbook_bench [impl] [workload] [scale] [updates=N] [reps=N] [--check]
//     impl      map | flat | both      (default both)
//     workload  A B C D E | all        (default all)
//     scale     1000 | 10000 | 100000 | 1000000 | all   (default all)
//     updates=N   steady ops per timed block            (default 2000000)
//     reps=N      timed blocks per cell; best is kept   (default 3)
//     --check     validate stream shapes only; no timing
//
// Run ONE implementation per process for profile runs (Phase 3): perf counter
// attribution for two book designs must never share a run.
//
// Release config is set in CMakeLists: -O3 -DNDEBUG plus (optionally)
// -march=native.

#include "flat_order_book.h"
#include "map_order_book.h"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using llob::ApplyResult;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::MapOrderBook;
using llob::Side;

namespace {

constexpr uint64_t kDefaultUpdates = 2'000'000;
constexpr int      kDefaultReps    = 3;
constexpr uint64_t kDefaultSeed    = 0x5EED'C0FF'EEULL;

constexpr int64_t kScaleLevels[4] = {1'000, 10'000, 100'000, 1'000'000};

enum class Workload : int { A, B, C, D, E };
constexpr int kWorkloadCount = 5;

const char* workload_tag(Workload w) {
    switch (w) {
        case Workload::A: return "A";
        case Workload::B: return "B";
        case Workload::C: return "C";
        case Workload::D: return "D";
        case Workload::E: return "E";
    }
    return "?";
}

const char* workload_desc(Workload w) {
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
// mirror of the book's live set (validated by --check). All of this is OFF the
// clock and never reaches the timed book.
// ---------------------------------------------------------------------------
class StreamGen {
public:
    // Cold-start snapshot: a full book with N live levels per side. The ONLY
    // way a fresh MapOrderBook/FlatOrderBook becomes usable is load_snapshot()
    // (both start unsynced and reject every apply() until then); an incremental
    // apply()-based fill is never valid on a fresh book. Deterministic and
    // identical for every impl/workload.
    static BookSnapshot fill_snapshot(int64_t n) {
        BookSnapshot snap;
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
            snap.asks.prices.push_back(price(Side::Ask, n, idx)); // 1..N, best first
            snap.asks.qtys.push_back(qty_of(idx));
        }
        return snap;
    }

    // Full steady-state op stream. Sequence numbers begin right after the fill
    // snapshot (at 2N+1), so the stream can be replayed immediately after
    // load_snapshot(fill_snapshot(n)).
    static std::vector<L2Update> steady_ops(Workload wl, int64_t n,
                                            uint64_t updates) {
        StreamGen g(wl, n);
        std::vector<L2Update> ops;
        ops.reserve(static_cast<size_t>(updates));
        for (uint64_t i = 0; i < updates; ++i) ops.push_back(g.next());
        return ops;
    }

private:
    static int64_t qty_of(int64_t idx) { return 10 + (idx % 997); }

    // Candidate price for level `idx` on `side` under scale `n`.
    static int64_t price(Side s, int64_t n, int64_t idx) {
        return llob::is_bid(s) ? 2 * n - idx : 1 + idx;
    }

    StreamGen(Workload wl, int64_t n)
        : wl_(wl)
        , n_(n)
        , seq_(static_cast<uint64_t>(2 * n)) // continues after the 2N fill ops
        , rng_(kDefaultSeed ^ static_cast<uint64_t>(n)) {
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

    L2Update make(Side s, int64_t idx, int64_t qty) {
        return L2Update{++seq_, price(s, n_, idx), qty, s};
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
    L2Update next() {
        const Side s    = (rnd(0, 1) == 0) ? Side::Bid : Side::Ask;
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
                // conserves density if each delete is matched by a refill of the
                // vacated level (any other add at full density is a re-quantify
                // and the side would drain — see probes in the commit notes). So
                // the delete rate must not exceed the refill rate:
                //   ~45% of ops delete the CURRENT best level (forces the
                //         inward best re-scan on the flat book);
                //   ~55% refill the most recently vacated best (LIFO hole), which
                //         restores it just below the current best, so the best
                //         churns between a few adjacent prices while the side
                //         stays at exactly N live levels. A refill with no
                //         pending hole is a re-quantify of the best region.
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
        return L2Update{0, 0, 0, Side::Bid}; // unreachable
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

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

// Compiler memory barrier: an opaque no-op at runtime that the optimizer cannot
// see through. In the timed loop it forces every apply() to be treated as an
// opaque memory-touching operation (its loads and stores cannot be reordered,
// CSE'd, or eliminated across the barrier). This matters because both books are
// header-only: without the barrier the compiler could legally prove parts of
// the flat book's stores dead and remove them, and could otherwise optimize
// more aggressively across apply() calls than a real caller in another
// translation unit would allow. Zero runtime cost — it only constrains the
// optimizer (register-allocation pressure).
inline void memory_barrier() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#else
    (void)0;
#endif
}

struct TimedResult {
    double best_ns_total      = 0.0; // best-of-reps wall time of the timed loop
    double best_ns_per_update = 0.0; // best_ns_total / updates
};

// Time `ops` steady updates against one Book over `reps` blocks. A FRESH book is
// cold-started from `snap` (a load_snapshot, untimed) at the start of every
// block so each rep is independent and begins from the same full, synced state.
// `tick_max` is the shared [1, tick_max] domain of the books under test (for our
// scale-N snapshots it is 2N, covering bids up to 2N and asks up to N). Returns
// the best-of-reps ns/update over the timed portion only.
template <typename Book>
TimedResult time_book(const BookSnapshot& snap,
                      const std::vector<L2Update>& ops,
                      int64_t tick_max, int reps) {
    TimedResult best;
    best.best_ns_per_update = 1e300;

    uint64_t sink = 0;

    for (int rep = 0; rep < reps; ++rep) {
        Book book(1, tick_max);
        if (!book.load_snapshot(snap)) { // cold start; untimed, like the old fill
            std::fprintf(stderr, "time_book: snapshot rejected (N=%lld) — aborting\n",
                         static_cast<long long>(tick_max));
            std::exit(2);
        }

        const auto t0 = Clock::now();
        for (const L2Update& u : ops) {
            sink ^= static_cast<uint64_t>(book.apply(u));
            memory_barrier(); // see above; zero runtime cost, keeps the loop honest
        }
        const auto t1 = Clock::now();

        // End-state reads (off the clock). Common API across both books; pin
        // best_bid/best_ask (which reflect cache writes made during the timed
        // loop) and the live-level count so the final state is observable.
        sink ^= static_cast<uint64_t>(book.best_bid());
        sink ^= static_cast<uint64_t>(book.best_ask());
        sink ^= static_cast<uint64_t>(book.level_count());

        const double ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (rep == 0 || ns < best.best_ns_total) {
            best.best_ns_total      = ns;
            best.best_ns_per_update = ns / static_cast<double>(ops.size());
        }
    }

    // Final observable read of the accumulator: the branch is never taken on any
    // real stream, but it makes every apply() result and end-state read above
    // genuinely influence program behavior, so the optimizer cannot drop them.
    if (sink == 0x9E37'79B9'7F4A'7C15ULL) {
        best.best_ns_per_update = -1.0; // unreachable; marks the writes as live
    }
    return best;
}

// ---------------------------------------------------------------------------
// Validation pass (--check): replay every stream against a reference book and
// print the resulting shape. Guarantees no degenerate stream is ever measured:
// density must stay ~N per side and every steady op must be Applied.
// ---------------------------------------------------------------------------
void check_streams(uint64_t updates) {
    std::printf("--check: replaying streams against MapOrderBook (reference)\n");
    for (int64_t n : kScaleLevels) {
        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const auto ops = StreamGen::steady_ops(wl, n, updates);
            MapOrderBook book(1, 2 * n);
            const bool loaded = book.load_snapshot(snap);
            uint64_t applied = 0, other = 0;
            for (const L2Update& u : ops) {
                if (book.apply(u) == ApplyResult::Applied) ++applied;
                else ++other;
            }
            std::printf("  N=%-10" PRId64 " wl=%s %-28s snap_ok=%d"
                        " steady_applied=%" PRIu64 " non_applied=%" PRIu64
                        " end_levels(bid/ask)=%zu/%zu best=%" PRId64 "/%" PRId64 "\n",
                        n, workload_tag(wl), workload_desc(wl), loaded ? 1 : 0,
                        applied, other,
                        book.bids().size(), book.asks().size(),
                        book.best_bid(), book.best_ask());
        }
    }
    std::printf("--check done (all steady ops should be Applied; end levels ~2N)\n");
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void print_header(uint64_t updates, int reps) {
    std::printf("# orderbook_bench - deterministic steady-state apply() throughput\n");
    std::printf("# domain [1, 2N]; N live levels/side; fill untimed; "
                "best of %d reps; %" PRIu64 " steady ops per block\n",
                reps, updates);
    std::printf("# impl,wl,scale_n,updates,best_ms,best_ns_per_update,best_updates_per_s\n");
}

template <typename Book>
void run_impl(const char* impl_name, const char* wl_sel, const char* scale_sel,
              uint64_t updates, int reps) {
    for (int si = 0; si < 4; ++si) {
        const int64_t n = kScaleLevels[si];
        const bool want_scale = std::strcmp(scale_sel, "all") == 0 ||
                                std::strtoll(scale_sel, nullptr, 10) == n;
        if (!want_scale) continue;

        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const bool want_wl = std::strcmp(wl_sel, "all") == 0 ||
                                 workload_tag(wl)[0] == wl_sel[0];
            if (!want_wl) continue;

            const auto ops       = StreamGen::steady_ops(wl, n, updates);
            const TimedResult r  = time_book<Book>(snap, ops, 2 * n, reps);
            std::printf("%s,%s,%.0f,%" PRIu64 ",%.3f,%.3f,%.0f\n",
                        impl_name, workload_tag(wl), static_cast<double>(n),
                        updates, r.best_ns_total / 1e6, r.best_ns_per_update,
                        1e9 / r.best_ns_per_update);
            std::fflush(stdout);
        }
    }
    std::printf("# done %s\n", impl_name);
}

void usage(const char* argv0) {
    std::printf(
        "usage: %s [impl] [workload] [scale] [updates=N] [reps=N] [--check]\n"
        "  impl      map | flat | both        (default both)\n"
        "  workload  A B C D E | all          (default all)\n"
        "  scale     1000 | 10000 | 100000 | 1000000 | all   (default all)\n"
        "  updates=N   steady ops per timed block            (default %" PRIu64 ")\n"
        "  reps=N      timed blocks per cell; best is kept   (default %d)\n"
        "  --check     validate stream shapes only; no timing\n",
        argv0, kDefaultUpdates, kDefaultReps);
}

} // namespace

int main(int argc, char** argv) {
    const char* impl_sel  = "both";
    const char* wl_sel    = "all";
    const char* scale_sel = "all";
    uint64_t    updates   = kDefaultUpdates;
    int         reps      = kDefaultReps;
    bool        check     = false;

    // First three positional args are impl, workload, scale, in that order.
    int pos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--check") == 0) {
            check = true;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (std::strncmp(a, "updates=", 8) == 0) {
            updates = std::strtoull(a + 8, nullptr, 10);
        } else if (std::strncmp(a, "reps=", 5) == 0) {
            reps = static_cast<int>(std::strtoul(a + 5, nullptr, 10));
        } else if (pos == 0) {
            impl_sel = a;
        } else if (pos == 1) {
            wl_sel = a;
        } else if (pos == 2) {
            scale_sel = a;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
        if (pos < 3) ++pos;
    }

    if (check) {
        check_streams(updates);
        return 0;
    }

    const bool want_map  = std::strcmp(impl_sel, "map") == 0 ||
                          std::strcmp(impl_sel, "both") == 0;
    const bool want_flat = std::strcmp(impl_sel, "flat") == 0 ||
                           std::strcmp(impl_sel, "both") == 0;
    if (!want_map && !want_flat) {
        std::fprintf(stderr, "unknown impl '%s' (want map|flat|both)\n", impl_sel);
        usage(argv[0]);
        return 2;
    }

    print_header(updates, reps);
    if (want_map) run_impl<MapOrderBook>("map", wl_sel, scale_sel, updates, reps);
    if (want_flat) run_impl<FlatOrderBook>("flat", wl_sel, scale_sel, updates, reps);
    return 0;
}
