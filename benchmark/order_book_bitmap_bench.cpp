// Deterministic steady-state benchmark for the Experiment 01 Optimization Study:
// FlatOrderBook vs BitsetFlatOrderBook on the FROZEN A/B/C/D/E workloads.
//
// This benchmark is the OPTIMIZATION-STUDY counterpart of the Phase 2
// throughput benchmark (order_book_bench.cpp). It deliberately REUSES the
// Experiment 01 methodology — the same deterministic stream generator
// (benchmark/stream_gen.h), the same [1, 2N] price-domain model, the same
// fresh-book cold start per rep, the same compiler barrier, the same
// best-of-reps summary, the same observable end state — and applies it to the
// two FLAT implementations so the study's questions are answered on a directly
// comparable basis:
//
//   Q4 (does the occupancy bitmap add overhead to ordinary positive->positive
//       updates?)  -> workload A, where the bitmap must NOT be touched.
//   Q5 (on the existing workloads, are best-delete gaps large enough for the
//       bitmap to help?) -> workload C (and the --gaps analysis below).
//
// CONTROL ISOLATION: BitsetFlatOrderBook's positive->positive path is also
// control-flow-different from the frozen FlatOrderBook (Flat eagerly checks the
// cached-best state on EVERY positive write; the bitmap book only maintains the
// cache on occupancy TRANSITIONS). To keep "control flow" and "occupancy
// bitmap" separable, a third implementation — TransitionAwareFlatOrderBook, a
// copy of FlatOrderBook with only the transition-aware positive path, NO
// bitmap — can be selected as `tuned`. Then:
//     flat  vs tuned  isolates the control-flow change alone;
//     tuned vs bits   isolates the bitmap alone (hot-path structure held equal);
//     flat  vs bits   is the end-to-end candidate delta and must NOT be read as
//                     "the bitmap". impl=both keeps the legacy flat+bits pair;
//                     impl=all selects flat+tuned+bits.
//
// Canonical runs are ONE implementation per process (`--impl flat`,
// `--impl tuned`, `--impl bits` separately), exactly as Phase 2 ran map and
// flat in separate processes. The benchmark itself never decides "who is
// faster": it reports ns/update and leaves the comparison to
// docs/ORDERBOOK_BITMAP_OPTIMIZATION.md.
//
// The bitmap book is constructed over the SAME domain as FlatOrderBook
// ([1, 2N]); its occupancy hierarchy (L0/L1/L2) covers the same per-side slot
// span, so memory accounting is directly comparable (--memory).
//
// Modes:
//   * default: steady-state best-of-reps throughput for the selected impl(s),
//     workload(s), scale(s). Output is the Phase 2 CSV shape.
//   * --check: replay the same stream through FlatOrderBook,
//     TransitionAwareFlatOrderBook, and BitsetFlatOrderBook and require
//     byte-identical observable state; no timing.
//   * --gaps: do NOT time; replay the selected workload/scale stream through an
//     off-clock level mirror and report the DISTRIBUTION of best-delete rescan
//     distances (how many empty price levels FlatOrderBook's inward scan would
//     cross when the current best is deleted). This is the measurement behind
//     question Q5: whether the gaps the workload actually produces are large
//     enough that a hierarchical lookup could beat an adjacent linear scan.
//     Requires a single workload and a single scale.
//   * --memory: do NOT time; print the quantity-array and occupancy-hierarchy
//     byte cost for the selected scale(s). The occupancy is never free; this
//     prints exactly what it costs (see BitsetFlatOrderBook::occupancy_bytes).
//
// Usage (positional args like the Phase 2 benchmark, flags after):
//   orderbook_bitmap_bench [impl] [workload] [scale] [updates=N] [reps=N]
//                          [--check] [--gaps] [--memory]
//     impl      flat | bits | both          (default both)
//     workload  A B C D E | all             (default all)
//     scale     1000 | 10000 | 100000 | 1000000 | all   (default all)
//     updates=N  steady ops per timed block            (default 2000000)
//     reps=N     timed blocks per cell; best is kept   (default 3)
//
// Release config is set in CMakeLists: -O3 -DNDEBUG.

#include "bitset_flat_order_book.h"
#include "flat_order_book.h"
#include "stream_gen.h"
#include "transition_aware_flat_order_book.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using llob::ApplyResult;
using llob::BitsetFlatOrderBook;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::Side;
using llob::TransitionAwareFlatOrderBook;

using llob_bench::StreamGen;
using llob_bench::Workload;
using llob_bench::kDefaultSeed;
using llob_bench::kWorkloadCount;
using llob_bench::workload_desc;
using llob_bench::workload_tag;

namespace {

constexpr uint64_t kDefaultUpdates = 2'000'000;
constexpr int      kDefaultReps    = 3;
constexpr int64_t  kScaleLevels[4] = {1'000, 10'000, 100'000, 1'000'000};

using Clock = std::chrono::steady_clock;

// Same compiler barrier as the Phase 2 benchmark: forces each apply() to be an
// opaque, memory-touching operation and is byte-identical in both timed loops.
inline void memory_barrier() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#else
    (void)0;
#endif
}

struct TimedResult {
    double best_ns_total      = 0.0;
    double best_ns_per_update = 0.0;
};

// Phase 2 methodology, unchanged: a FRESH book cold-starts from `snap` (a
// load_snapshot, untimed) at the start of every rep so each block is
// independent and starts from the same full, synced state. `tick_max` is the
// shared [1, tick_max] domain (2N for a scale-N snapshot). Best-of-reps of the
// timed apply() loop only; end-state reads are off the clock and make the
// timed writes observable.
template <typename Book>
TimedResult time_book(const BookSnapshot& snap,
                      const std::vector<L2Update>& ops,
                      int64_t tick_max, int reps) {
    TimedResult best;
    best.best_ns_per_update = 1e300;
    uint64_t sink = 0;

    for (int rep = 0; rep < reps; ++rep) {
        Book book(1, tick_max);
        if (!book.load_snapshot(snap)) { // cold start; untimed
            std::fprintf(stderr, "time_book: snapshot rejected (N=%lld) — aborting\n",
                         static_cast<long long>(tick_max));
            std::exit(2);
        }

        const auto t0 = Clock::now();
        for (const L2Update& u : ops) {
            sink ^= static_cast<uint64_t>(book.apply(u));
            memory_barrier();
        }
        const auto t1 = Clock::now();

        // Off-clock end-state reads: make the timed writes observable.
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
    if (sink == 0x9E37'79B9'7F4A'7C15ULL) {
        best.best_ns_per_update = -1.0; // unreachable; keeps the writes live
    }
    return best;
}

// ---------------------------------------------------------------------------
// --gaps: best-delete rescan-distance analysis (OFF the clock).
//
// Replays the deterministic (workload, n, updates) stream against a light level
// mirror that reconstructs, for every op that deletes the CURRENT best, how far
// below the deleted best the next present level sits — i.e. how many empty
// price levels FlatOrderBook's inward re-scan has to cross. Adjacent levels
// give distance 1. The generator's own level-index model is used: idx 0 is the
// best price on both sides, price moves by 1 per idx, so the idx distance IS
// the price distance in ticks and IS the flat rescan length.
// ---------------------------------------------------------------------------
void report_gaps(Workload wl, int64_t n, uint64_t updates) {
    const auto snap = StreamGen::fill_snapshot(n);
    const auto ops  = StreamGen::steady_ops(wl, n, updates, kDefaultSeed);

    // Mirror over level idx [0, n): present_[side][idx] == live in the book.
    std::vector<uint8_t> present[2];
    present[0].assign(static_cast<size_t>(n), 1);
    present[1].assign(static_cast<size_t>(n), 1);
    int64_t best[2] = {0, 0};

    std::vector<uint64_t> hist;          // hist[d-1] = best-deletes with rescan d
    uint64_t best_deletes = 0;
    uint64_t side_emptied = 0;           // best delete with no level left below

    auto idx_of = [n](Side s, int64_t price) -> int64_t {
        return llob::is_bid(s) ? 2 * n - price : price - 1;
    };

    for (const L2Update& u : ops) {
        const int si = llob::is_bid(u.side) ? 0 : 1;
        const int64_t idx = idx_of(u.side, u.price);
        if (u.qty == 0) {
            // Delete of a present level (the generator only deletes present).
            if (idx == best[si]) {
                // Best delete: how far to the next present level (higher idx)?
                int64_t j = idx + 1;
                while (j < n && !present[si][static_cast<size_t>(j)]) ++j;
                if (j < n) {
                    const int64_t d = j - idx; // flat rescan length
                    if (hist.size() <= static_cast<size_t>(d - 1))
                        hist.resize(static_cast<size_t>(d), 0);
                    ++hist[static_cast<size_t>(d - 1)];
                    best[si] = j;
                } else {
                    ++side_emptied;
                    best[si] = -1;
                }
                ++best_deletes;
            }
            present[si][static_cast<size_t>(idx)] = 0;
        } else {
            const bool was_absent =
                !present[si][static_cast<size_t>(idx)];
            present[si][static_cast<size_t>(idx)] = 1;
            if (best[si] < 0 || idx < best[si]) best[si] = idx;
            (void)was_absent;
        }
    }

    std::printf("# gaps: wl=%s N=%.0f updates=%" PRIu64
                "  best_deletes=%" PRIu64 " (side_emptied=%" PRIu64 ")\n",
                workload_tag(wl), static_cast<double>(n), updates,
                best_deletes, side_emptied);
    if (best_deletes == 0) {
        std::printf("# gaps: no best-deletes in this stream\n");
        return;
    }
    // Cumulative share of best-deletes whose rescan is <= each distance.
    uint64_t cum = 0;
    const int max_print = 16;
    std::printf("# gaps: rescan-distance cumulative distribution (of best deletes):\n");
    for (size_t i = 0; i < hist.size() && i < static_cast<size_t>(max_print); ++i) {
        if (hist[i] == 0) continue;
        cum += hist[i];
        std::printf("# gaps:   distance<=%zu : %6.2f%%  (n=%" PRIu64 ")\n",
                    i + 1, 100.0 * static_cast<double>(cum) /
                                static_cast<double>(best_deletes),
                    hist[i]);
    }
    if (cum < best_deletes) {
        std::printf("# gaps:   distance>%d : %6.2f%%\n", max_print,
                    100.0 * static_cast<double>(best_deletes - cum) /
                        static_cast<double>(best_deletes));
    }
}

// ---------------------------------------------------------------------------
// --memory: quantity-array vs occupancy-hierarchy byte cost at scale n.
// The benchmark domain is [1, 2N], so each side's slot span is 2N.
// ---------------------------------------------------------------------------
void report_memory(int64_t n) {
    const int64_t span = 2 * n;
    BitsetFlatOrderBook probe(1, span); // same domain the timed books use
    size_t words[3] = {0, 0, 0};
    probe.hierarchy_words(words);
    const double MiB = 1024.0 * 1024.0;
    std::printf("# memory N=%.0f domain=[1,%.0f] per-side slot span=%.0f\n",
                static_cast<double>(n), static_cast<double>(span),
                static_cast<double>(span));
    // Explicit size_t -> double casts (per the strict-warning discipline): the
    // byte counts are size_t, so every conversion into the %g computations is
    // spelled out rather than left to implicit usual-arithmetic conversions.
    std::printf("# memory quantity_arrays_bytes=%zu  (%g MiB; %g MiB/side)\n",
                probe.quantity_bytes(),
                static_cast<double>(probe.quantity_bytes()) / MiB,
                static_cast<double>(probe.quantity_bytes()) / 2.0 / MiB);
    std::printf("# memory occupancy_L0_words_per_side=%zu L1=%zu L2=%zu\n",
                words[0], words[1], words[2]);
    std::printf("# memory occupancy_bytes=%zu  (%g MiB; per side L0=%g KiB "
                "L1=%g KiB L2=%g B)\n",
                probe.occupancy_bytes(),
                static_cast<double>(probe.occupancy_bytes()) / MiB,
                (static_cast<double>(words[0]) * 8.0) / 1024.0,
                (static_cast<double>(words[1]) * 8.0) / 1024.0,
                static_cast<double>(words[2]) * 8.0);
    std::printf("# memory occupancy_is_fraction_of_quantity=%.4f%%\n",
                100.0 * static_cast<double>(probe.occupancy_bytes()) /
                    static_cast<double>(probe.quantity_bytes()));
}

// ---------------------------------------------------------------------------
// --check: FlatOrderBook vs TransitionAwareFlatOrderBook vs BitsetFlatOrderBook
// differential validation over every (scale, workload) stream. Divergence FAILS
// loudly (non-zero exit).
// ---------------------------------------------------------------------------
int check_streams(uint64_t updates) {
    int failures = 0;
    std::printf("--check: differential FlatOrderBook vs TransitionAwareFlatOrderBook "
                "vs BitsetFlatOrderBook (identical stream, identical domain)\n");
    for (int64_t n : kScaleLevels) {
        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const auto ops = StreamGen::steady_ops(wl, n, updates);

            FlatOrderBook               fbook(1, 2 * n);
            TransitionAwareFlatOrderBook tbook(1, 2 * n);
            BitsetFlatOrderBook          bbook(1, 2 * n);
            const bool loaded_f = fbook.load_snapshot(snap);
            const bool loaded_t = tbook.load_snapshot(snap);
            const bool loaded_b = bbook.load_snapshot(snap);
            if (!(loaded_f && loaded_t && loaded_b)) {
                ++failures;
                std::printf("  [FAIL] N=%-9" PRId64 " wl=%s snapshot rejected "
                            "(flat=%d tuned=%d bits=%d)\n",
                            n, workload_tag(wl), loaded_f ? 1 : 0,
                            loaded_t ? 1 : 0, loaded_b ? 1 : 0);
                continue;
            }

            uint64_t applied = 0, other = 0;
            for (const L2Update& u : ops) {
                const ApplyResult rf = fbook.apply(u);
                const ApplyResult rt = tbook.apply(u);
                const ApplyResult rb = bbook.apply(u);
                const bool same = rf == rt && rf == rb &&
                                  fbook.synced() == tbook.synced() &&
                                  fbook.synced() == bbook.synced() &&
                                  fbook.last_applied_seq() == tbook.last_applied_seq() &&
                                  fbook.last_applied_seq() == bbook.last_applied_seq() &&
                                  fbook.best_bid() == tbook.best_bid() &&
                                  fbook.best_bid() == bbook.best_bid() &&
                                  fbook.best_ask() == tbook.best_ask() &&
                                  fbook.best_ask() == bbook.best_ask() &&
                                  fbook.best_bid_qty() == tbook.best_bid_qty() &&
                                  fbook.best_bid_qty() == bbook.best_bid_qty() &&
                                  fbook.best_ask_qty() == tbook.best_ask_qty() &&
                                  fbook.best_ask_qty() == bbook.best_ask_qty();
                if (!same) {
                    if (failures < 10) {
                        std::printf("  [DIVERGE] N=%-9" PRId64 " wl=%s seq=%" PRIu64
                                    " flat(rs=%d sync=%d bb=%" PRId64 "/%" PRId64
                                    " ba=%" PRId64 "/%" PRId64 ") tuned(rs=%d bb=%"
                                    PRId64 "/%" PRId64 " ba=%" PRId64 "/%" PRId64
                                    ") bits(rs=%d sync=%d bb=%" PRId64 "/%" PRId64
                                    " ba=%" PRId64 "/%" PRId64 ")\n",
                                    n, workload_tag(wl), u.seq,
                                    static_cast<int>(rf), fbook.synced() ? 1 : 0,
                                    fbook.best_bid(), fbook.best_bid_qty(),
                                    fbook.best_ask(), fbook.best_ask_qty(),
                                    static_cast<int>(rt), tbook.best_bid(),
                                    tbook.best_bid_qty(), tbook.best_ask(),
                                    tbook.best_ask_qty(),
                                    static_cast<int>(rb), bbook.synced() ? 1 : 0,
                                    bbook.best_bid(), bbook.best_bid_qty(),
                                    bbook.best_ask(), bbook.best_ask_qty());
                    }
                    ++failures;
                    break;
                }
                if (rf == ApplyResult::Applied) ++applied;
                else ++other;
            }
            std::printf("  N=%-9" PRId64 " wl=%s %-28s ok  applied=%" PRIu64
                        " non_applied=%" PRIu64 " end_best=%" PRId64 "/%" PRId64 "\n",
                        n, workload_tag(wl), workload_desc(wl), applied, other,
                        fbook.best_bid(), fbook.best_ask());
        }
    }
    std::printf(failures == 0
                    ? "--check PASSED: Flat, Tuned, and Bits agree on every stream\n"
                    : "--check FAILED: %d stream(s) diverged\n",
                failures);
    return failures == 0 ? 0 : 1;
}

void print_header(uint64_t updates, int reps) {
    std::printf("# orderbook_bitmap_bench - optimization-study steady throughput "
                "(flat / transition-aware control / hierarchical-occupancy bitmap)\n");
    std::printf("# domain [1, 2N]; N live levels/side; fill untimed; "
                "best of %d reps; %" PRIu64 " steady ops per block\n",
                reps, updates);
    std::printf("# impl,wl,scale_n,updates,best_ms,best_ns_per_update,best_updates_per_s\n");
}

template <typename Book>
void run_impl(const char* impl_name, const char* wl_sel, const char* scale_sel,
              uint64_t updates, int reps) {
    for (int64_t n : kScaleLevels) {
        const bool want_scale = std::strcmp(scale_sel, "all") == 0 ||
                                std::strtoll(scale_sel, nullptr, 10) == n;
        if (!want_scale) continue;

        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const bool want_wl = std::strcmp(wl_sel, "all") == 0 ||
                                 workload_tag(wl)[0] == wl_sel[0];
            if (!want_wl) continue;

            const auto ops      = StreamGen::steady_ops(wl, n, updates);
            const TimedResult r = time_book<Book>(snap, ops, 2 * n, reps);
            std::printf("%s,%s,%.0f,%" PRIu64 ",%.3f,%.3f,%.0f\n",
                        impl_name, workload_tag(wl), static_cast<double>(n),
                        updates, r.best_ns_total / 1e6, r.best_ns_per_update,
                        1e9 / r.best_ns_per_update);
            std::fflush(stdout);
        }
    }
    std::printf("# done %s\n", impl_name);
}

// ---- Interleaved in-process mode -------------------------------------------
// Phase 2 runs one impl per process; that is authoritative for an impl's OWN
// absolute throughput (best-of-reps, comparable to the frozen numbers). But a
// cross-impl DELTA measured across two separate processes on this host is
// drowned by process-to-process turbo/frequency drift (a 0.1 s process samples
// one DVFS state). For the DELTA only, this mode times the selected
// implementations block-by-block INSIDE one process, rotating who goes first
// each block, on identical books and op streams, so every pair shares the same
// clock. Each block is still a fresh snapshot cold start. impl=both selects
// flat+bits (legacy two-way); impl=all selects flat+tuned+bits so the
// control-flow (flat vs tuned) and bitmap (tuned vs bits) deltas share one
// clock state with the end-to-end flat-vs-bits delta.

// One timed block on a fresh book; returns total ns for the whole block.
template <typename Book>
double time_block_ns(const BookSnapshot& snap, const std::vector<L2Update>& ops,
                     int64_t tick_max, uint64_t& sink) {
    Book book(1, tick_max);
    if (!book.load_snapshot(snap)) {
        std::fprintf(stderr, "time_block_ns: snapshot rejected — aborting\n");
        std::exit(2);
    }
    const auto t0 = Clock::now();
    for (const L2Update& u : ops) {
        sink ^= static_cast<uint64_t>(book.apply(u));
        memory_barrier();
    }
    const auto t1 = Clock::now();
    sink ^= static_cast<uint64_t>(book.best_bid());
    sink ^= static_cast<uint64_t>(book.best_ask());
    sink ^= static_cast<uint64_t>(book.level_count());
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

double median_sorted(std::vector<double>& v) {
    // v need not be fully sorted: std::nth_element to the middle is enough.
    const size_t n = v.size();
    const size_t mid = n / 2;
    std::nth_element(v.begin(),
                     v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    return v[mid];
}

// A named timing primitive for one implementation (a pointer to one
// instantiation of time_block_ns). Lets run_interleaved drive any subset of the
// flat/tuned/bits implementations with identical framing.
struct TimedImpl {
    const char* name;
    double (*time_block)(const BookSnapshot& snap, const std::vector<L2Update>& ops,
                         int64_t tick_max, uint64_t& sink);
};

std::vector<TimedImpl> select_impls(bool want_flat, bool want_tuned,
                                    bool want_bits) {
    std::vector<TimedImpl> v;
    if (want_flat)
        v.push_back(TimedImpl{"flat", &time_block_ns<FlatOrderBook>});
    if (want_tuned)
        v.push_back(TimedImpl{"tuned", &time_block_ns<TransitionAwareFlatOrderBook>});
    if (want_bits)
        v.push_back(TimedImpl{"bits", &time_block_ns<BitsetFlatOrderBook>});
    return v;
}

void run_interleaved(const char* wl_sel, const char* scale_sel,
                     uint64_t updates, int blocks,
                     const std::vector<TimedImpl>& impls) {
    const int ni = static_cast<int>(impls.size());
    std::printf("# orderbook_bitmap_bench --inproc: %d implementation%s "
                "interleaved in ONE process (%d blocks each, fresh snapshot per "
                "block, rotating start order), %" PRIu64 " ops/block\n",
                ni, ni == 1 ? "" : "s", blocks, updates);
    std::printf("# impl,wl,scale_n,updates,ns_per_update_median,ns_per_update_mean\n");
    for (int64_t n : kScaleLevels) {
        const bool want_scale = std::strcmp(scale_sel, "all") == 0 ||
                                std::strtoll(scale_sel, nullptr, 10) == n;
        if (!want_scale) continue;
        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const bool want_wl = std::strcmp(wl_sel, "all") == 0 ||
                                 workload_tag(wl)[0] == wl_sel[0];
            if (!want_wl) continue;

            const auto ops = StreamGen::steady_ops(wl, n, updates);
            std::vector<std::vector<double>> per(static_cast<size_t>(ni));
            for (auto& pv : per) pv.reserve(static_cast<size_t>(blocks));
            uint64_t sink = 0;
            for (int b = 0; b < blocks; ++b) {
                // Rotate which impl times first so no impl always follows the
                // others' freshly-touched memory (for ni == 2 this reproduces
                // the legacy flat/bits alternation exactly).
                for (int k = 0; k < ni; ++k) {
                    const size_t ii = static_cast<size_t>((b + k) % ni);
                    per[ii].push_back(impls[ii].time_block(snap, ops, 2 * n,
                                                           sink) /
                                      static_cast<double>(updates));
                }
            }
            for (int i = 0; i < ni; ++i) {
                const size_t ii = static_cast<size_t>(i);
                std::vector<double> sorted = per[ii]; // copy for the median
                std::sort(sorted.begin(), sorted.end());
                double sum = 0.0;
                for (double x : per[ii]) sum += x;
                std::printf("%s,%s,%.0f,%" PRIu64 ",%.3f,%.3f\n",
                            impls[ii].name, workload_tag(wl),
                            static_cast<double>(n), updates,
                            median_sorted(sorted),
                            sum / static_cast<double>(per[ii].size()));
            }
            // sink folds every apply result and end-state read; printing it
            // (not testing it) keeps those timed writes/reads observable.
            std::printf("# sink=0x%016" PRIx64 "\n", sink);
            std::fflush(stdout);
        }
    }
}

void usage(const char* argv0) {
    std::printf(
        "usage: %s [impl] [workload] [scale] [updates=N] [reps=N] "
        "[--check] [--gaps] [--memory] [--inproc=N]\n"
        "  impl      flat | tuned | bits | both | all   (default both)\n"
        "            both = flat + bits (legacy pair); all = flat + tuned + bits\n"
        "            (tuned = TransitionAwareFlatOrderBook, the no-bitmap control\n"
        "            that isolates the transition-aware hot-path control flow)\n"
        "  workload  A B C D E | all           (default all)\n"
        "  scale     1000 | 10000 | 100000 | 1000000 | all  (default all)\n"
        "  updates=N   steady ops per timed block           (default %" PRIu64 ")\n"
        "  reps=N      timed blocks per cell; best is kept  (default %d)\n"
        "  --check   replay streams through Flat, Tuned, Bits; require agreement;\n"
        "            no timing\n"
        "  --gaps    report the best-delete rescan-distance distribution for the\n"
        "            selected workload/scale (single wl and scale); no timing\n"
        "  --memory  report quantity-array vs occupancy-hierarchy bytes at the\n"
        "            selected scale(s); no timing\n"
        "  --inproc=N  interleave N blocks per cell of each selected impl inside\n"
        "            ONE process (requires impl=both or impl=all) for a drift-free\n"
        "            cross-impl DELTA; median & mean ns/update over the blocks\n",
        argv0, kDefaultUpdates, kDefaultReps);
}

} // namespace

int main(int argc, char** argv) {
    const char* impl_sel  = "both";
    const char* wl_sel    = "all";
    const char* scale_sel = "all";
    uint64_t    updates   = kDefaultUpdates;
    int         reps      = kDefaultReps;
    int         inproc    = 0; // >0 selects the interleaved in-process mode
    bool        check     = false;
    bool        gaps      = false;
    bool        memory    = false;

    int pos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--check") == 0) {
            check = true;
        } else if (std::strcmp(a, "--gaps") == 0) {
            gaps = true;
        } else if (std::strcmp(a, "--memory") == 0) {
            memory = true;
        } else if (std::strncmp(a, "--inproc=", 9) == 0) {
            inproc = static_cast<int>(std::strtoul(a + 9, nullptr, 10));
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

    const bool want_flat  = std::strcmp(impl_sel, "flat") == 0 ||
                            std::strcmp(impl_sel, "both") == 0 ||
                            std::strcmp(impl_sel, "all") == 0;
    const bool want_tuned = std::strcmp(impl_sel, "tuned") == 0 ||
                            std::strcmp(impl_sel, "all") == 0;
    const bool want_bits  = std::strcmp(impl_sel, "bits") == 0 ||
                            std::strcmp(impl_sel, "both") == 0 ||
                            std::strcmp(impl_sel, "all") == 0;
    if (!want_flat && !want_tuned && !want_bits) {
        std::fprintf(stderr, "unknown impl '%s' (want flat|tuned|bits|both|all)\n",
                     impl_sel);
        usage(argv[0]);
        return 2;
    }

    const bool single_wl = wl_sel[0] >= 'A' && wl_sel[0] <= 'E' &&
                           std::strlen(wl_sel) == 1;
    const bool single_scale = std::strcmp(scale_sel, "all") != 0;

    if (check) return check_streams(updates);

    if (gaps) {
        if (!single_wl || !single_scale) {
            std::fprintf(stderr, "--gaps needs exactly one workload and one scale\n");
            return 2;
        }
        const Workload wl = static_cast<Workload>(wl_sel[0] - 'A');
        report_gaps(wl, std::strtoll(scale_sel, nullptr, 10), updates);
        return 0;
    }

    if (memory) {
        if (single_scale) {
            report_memory(std::strtoll(scale_sel, nullptr, 10));
        } else {
            for (int64_t n : kScaleLevels) report_memory(n);
        }
        return 0;
    }

    if (inproc > 0) {
        const bool pair_ok = std::strcmp(impl_sel, "both") == 0 ||
                             std::strcmp(impl_sel, "all") == 0;
        if (!pair_ok) {
            std::fprintf(stderr, "--inproc requires impl=both (flat+bits) or "
                                 "impl=all (flat+tuned+bits) so every cross-impl "
                                 "DELTA shares one process and one clock\n");
            return 2;
        }
        run_interleaved(wl_sel, scale_sel, updates, inproc,
                        select_impls(want_flat, want_tuned, want_bits));
        return 0;
    }

    print_header(updates, reps);
    if (want_flat) run_impl<FlatOrderBook>("flat", wl_sel, scale_sel, updates, reps);
    if (want_tuned) run_impl<TransitionAwareFlatOrderBook>("tuned", wl_sel, scale_sel, updates, reps);
    if (want_bits) run_impl<BitsetFlatOrderBook>("bits", wl_sel, scale_sel, updates, reps);
    return 0;
}
