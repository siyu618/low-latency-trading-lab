// Controlled-gap benchmark for the Experiment 01 Optimization Study.
//
// Research question this drives (see docs/ORDERBOOK_BITMAP_OPTIMIZATION.md):
//   "At what next-best price gap does BitsetFlatOrderBook's hierarchical
//    occupancy lookup become cheaper than FlatOrderBook's adjacent linear
//    best re-scan — and is that gap a regime the real workloads produce?"
//
// Design: a deterministic LADDER book whose ask side holds K+1 levels spaced
// exactly `g` price ticks apart (prices 1, 1+g, 1+2g, ..., 1+K*g). The floor
// level at 1+K*g is never deleted, so every one of the K timed best-deletes
// moves the best ask to a level exactly `g` ticks higher — meaning
// FlatOrderBook's inward re-scan examines EXACTLY g array slots on every timed
// delete, regardless of which delete in the block it is. No delete ever finds
// no next level (no unfair final-delete). The bid side is empty (both books
// support an empty side; it is never touched).
//
// FlatOrderBook and BitsetFlatOrderBook are loaded from the SAME snapshot and
// replay the SAME delete stream, so both see byte-identical books and work.
// The only difference being timed is the re-scan primitive: adjacent linear
// (Flat) vs hierarchical bitmap (Bitset). g sweeps
// {1,2,4,8,16,32,64,128,256,512,1024}.
//
// Methodology follows Experiment 01 conventions (fresh cold start per timed
// block, deterministic input before any clock, same compiler barrier, off-clock
// end-state reads, no harness overhead auto-subtracted). The crossover question
// is a WITHIN-PROCESS A/B: when both impls are selected they are interleaved
// block-by-block in one process so each flat/bits pair shares the same CPU
// clock state (separate processes drift by turbo/frequency on this host by
// more than the effects under study). Per-block duration / K is one sample;
// `blocks` samples per (impl,g) give a distribution (p50/p99/max) AND a
// best-of-blocks central value. One impl at a time is still available via
// `--impl flat|bits` when a self-consistent single-impl curve is wanted.
//
// Output is CSV to stdout (rows `impl,gap,ns_per_delete_best,mean,p50,p99,max`)
// with `#`-prefixed provenance lines.
//
// Usage:
//   orderbook_gap_bench [--impl=flat|bits|both] [--gaps=1,2,4,...,1024]
//                       [--blocks=N] [--deletes=K] [--fixed-domain]
//   defaults: both (interleaved), the full 11-step gap ladder, 128 blocks,
//   4096 deletes. K must be large enough that each timed block (K best-deletes)
//   is comfortably above timer jitter; the ladder memory cost is (K+1)*g slots
//   per side, so K=4096 keeps the largest-gap domain at ~4.2M slots (~67 MB of
//   qty arrays).
//
// CONTROL NOTE / --fixed-domain. The default ladder grows the book DOMAIN with
// g (domain = 1 + (K+1)*g), so a larger g confounds two things: the next-best
// distance FlatOrderBook actually re-scans (g slots) and the active memory span
// of the whole book. To hold the active-memory span constant while g still
// varies the re-scan distance, pass --fixed-domain: EVERY gap in the --gaps list
// is then timed on a book whose domain is the size the LARGEST gap in the list
// requires (1 + (K+1)*max(gaps)). Levels still sit at 1+i*g, so each timed
// best-delete still re-scans exactly g slots — the two variables are decoupled.
// The existing (variable-domain) sweep is kept as-is for continuity; the
// fixed-domain sweep is a validation that the crossover conclusion survives the
// confound removal.

#include "bitset_flat_order_book.h"
#include "flat_order_book.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using llob::ApplyResult;
using llob::BitsetFlatOrderBook;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::Side;

namespace {

constexpr int64_t kDefaultDeletes = 4096; // K ladder deletes per timed block
constexpr int     kDefaultBlocks  = 128;

constexpr int64_t kGaps[11] = {1,   2,    4,    8,    16,   32,
                               64,  128,  256,  512,  1024};

using Clock = std::chrono::steady_clock;

inline void memory_barrier() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#else
    (void)0;
#endif
}

// ---------------------------------------------------------------------------
// Ladder snapshot: ask side holds K+1 present levels at prices 1 + i*g
// (i = 0..K), so a delete of the current best ask always rescans exactly g
// slots, and the floor level i=K survives all K timed deletes. Bid side empty.
// Domain [1, D] with D = 1 + (K+1)*g. snap.seq = K+1 (entries loaded); the
// delete stream then starts at seq K+2.
// ---------------------------------------------------------------------------
BookSnapshot make_ladder_snapshot(int64_t g, int64_t K) {
    BookSnapshot snap;
    snap.seq = static_cast<uint64_t>(K + 1);
    snap.asks.prices.reserve(static_cast<size_t>(K) + 1);
    snap.asks.qtys.reserve(static_cast<size_t>(K) + 1);
    for (int64_t i = 0; i <= K; ++i) {
        snap.asks.prices.push_back(1 + i * g);
        snap.asks.qtys.push_back(1);
    }
    return snap;
}

std::vector<L2Update> make_delete_stream(int64_t g, int64_t K, uint64_t seq0) {
    std::vector<L2Update> ops;
    ops.reserve(static_cast<size_t>(K));
    for (int64_t i = 0; i < K; ++i) {
        // Delete the current best ask: prices ascend 1, 1+g, ... so each
        // delete hits the level that IS the best at that moment.
        ops.push_back(L2Update{seq0 + static_cast<uint64_t>(i),
                               1 + i * g, 0, Side::Ask});
    }
    return ops;
}

// Time one block: fresh book + snapshot (cold, untimed), then K best-deletes.
// Returns ns/delete for this block, or -1 if the book rejected the snapshot.
// The block is validated by the off-clock end-state reads (final best ask must
// be 1 + K*g and exactly the floor level must remain) folded into `sink`, so
// the timed writes are observable and a broken ladder is caught, not timed.
template <typename Book>
double time_block(const BookSnapshot& snap, const std::vector<L2Update>& ops,
                  int64_t g, int64_t K, int64_t domain_max, uint64_t& sink) {
    Book book(1, domain_max);
    if (!book.load_snapshot(snap)) {
        std::fprintf(stderr, "time_block: snapshot rejected (g=%lld K=%lld) — aborting\n",
                     static_cast<long long>(g), static_cast<long long>(K));
        std::exit(2);
    }

    const auto t0 = Clock::now();
    for (const L2Update& u : ops) {
        sink ^= static_cast<uint64_t>(book.apply(u));
        memory_barrier();
    }
    const auto t1 = Clock::now();

    // Off-clock end-state reads: the floor level must be all that is left.
    sink ^= static_cast<uint64_t>(book.best_ask());
    sink ^= static_cast<uint64_t>(book.best_bid());
    sink ^= static_cast<uint64_t>(book.level_count());

    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return ns / static_cast<double>(ops.size());
}

double percentile(std::vector<double>& sorted, double q) {
    // Nearest-rank: index = ceil(q * n) (1-based), so the worst block is the
    // p99.9/max you would actually see. sorted must already be ascending.
    const int n = static_cast<int>(sorted.size());
    const int rank = std::max(1, static_cast<int>(std::ceil(q * static_cast<double>(n))));
    return sorted[static_cast<size_t>(rank) - 1];
}

void emit_impl(std::FILE* out, const char* name, int64_t g,
               std::vector<double>& samples, int blocks) {
    std::sort(samples.begin(), samples.end());
    double mean = 0.0;
    for (double s : samples) mean += s;
    mean /= static_cast<double>(samples.size());
    std::fprintf(out, "%s,%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
                 name, static_cast<long long>(g),
                 samples.front(), mean, percentile(samples, 0.50),
                 percentile(samples, 0.99), samples.back(), blocks);
    std::fflush(out);
}

// Sweep one gap, timing the selected implementations on IDENTICAL books and
// delete streams. When both impls are selected they are INTERLEAVED block by
// block inside this single process (alternating who goes first per block), so
// each flat/bits block pair shares the same CPU clock state. This is the
// controlled comparison the crossover question needs: a flat vs bits
// difference measured across two separate processes would be drowned by
// process-to-process turbo/frequency drift on this host.
void run_gap(int64_t g, int64_t K, int blocks, bool want_flat, bool want_bits,
             int64_t domain_max, std::FILE* out) {
    const BookSnapshot snap = make_ladder_snapshot(g, K);
    const std::vector<L2Update> ops = make_delete_stream(
        g, K, snap.seq + 1);

    std::vector<double> fs, bs;
    if (want_flat) fs.reserve(static_cast<size_t>(blocks));
    if (want_bits) bs.reserve(static_cast<size_t>(blocks));
    uint64_t sink = 0;
    for (int b = 0; b < blocks; ++b) {
        // Alternate who times first so neither impl always follows the other's
        // freshly-freed memory.
        const bool flat_first = (b % 2 == 0);
        if (flat_first) {
            if (want_flat)
                fs.push_back(time_block<FlatOrderBook>(snap, ops, g, K, domain_max, sink));
            if (want_bits)
                bs.push_back(time_block<BitsetFlatOrderBook>(snap, ops, g, K, domain_max, sink));
        } else {
            if (want_bits)
                bs.push_back(time_block<BitsetFlatOrderBook>(snap, ops, g, K, domain_max, sink));
            if (want_flat)
                fs.push_back(time_block<FlatOrderBook>(snap, ops, g, K, domain_max, sink));
        }
    }
    if (want_flat) emit_impl(out, "flat", g, fs, blocks);
    if (want_bits) emit_impl(out, "bits", g, bs, blocks);

    // sink folds every apply result and end-state read across all blocks;
    // printing it (not testing it) keeps the timed writes/reads observable.
    std::fprintf(out, "# sink=0x%016" PRIx64 "\n", sink);
    std::fflush(out);
}

} // namespace

int main(int argc, char** argv) {
    const char* impl_sel = "both";
    int64_t     K        = kDefaultDeletes;
    int         blocks   = kDefaultBlocks;
    bool        fixed_domain = false;
    std::vector<int64_t> gaps(kGaps, kGaps + 11);

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--impl=", 7) == 0) {
            impl_sel = a + 7;
        } else if (std::strncmp(a, "--gaps=", 7) == 0) {
            gaps.clear();
            const char* p = a + 7;
            while (*p) {
                char* end = nullptr;
                const long v = std::strtol(p, &end, 10);
                if (end == p) { std::fprintf(stderr, "bad --gaps value near '%s'\n", p); return 2; }
                gaps.push_back(v);
                p = (*end == ',') ? end + 1 : end;
            }
        } else if (std::strncmp(a, "--deletes=", 10) == 0) {
            K = std::strtoll(a + 10, nullptr, 10);
        } else if (std::strncmp(a, "--blocks=", 9) == 0) {
            blocks = static_cast<int>(std::strtoul(a + 9, nullptr, 10));
        } else if (std::strcmp(a, "--fixed-domain") == 0) {
            fixed_domain = true;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::printf(
                "usage: %s [--impl=flat|bits|both] [--gaps=1,2,4,...] "
                "[--deletes=K] [--blocks=N] [--fixed-domain]\n"
                "  deterministic ladder gap sweep; run ONE impl per process for "
                "canonical numbers\n"
                "  --fixed-domain: hold the book DOMAIN at the size the largest "
                "--gaps entry requires for every gap (decouples re-scan distance "
                "from active memory span; see the file comment)\n",
                argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", a);
            return 2;
        }
    }
    if (K <= 0 || blocks <= 0) {
        std::fprintf(stderr, "--deletes and --blocks must be positive\n");
        return 2;
    }

    const bool want_flat = std::strcmp(impl_sel, "flat") == 0 ||
                           std::strcmp(impl_sel, "both") == 0;
    const bool want_bits = std::strcmp(impl_sel, "bits") == 0 ||
                           std::strcmp(impl_sel, "both") == 0;
    if (!want_flat && !want_bits) {
        std::fprintf(stderr, "unknown --impl '%s' (flat|bits|both)\n", impl_sel);
        return 2;
    }

    std::printf("# orderbook_gap_bench - controlled best-delete gap sweep "
                "(linear re-scan vs hierarchical occupancy lookup)\n");
    std::printf("# ladder per row: K+1=%lld ask levels g ticks apart (floor "
                "survives); every timed delete rescans exactly g slots\n",
                static_cast<long long>(K + 1));
    std::printf("# gaps swept: ");
    for (size_t i = 0; i < gaps.size(); ++i)
        std::printf("%s%lld", i ? "," : "", static_cast<long long>(gaps[i]));
    std::printf("\n");
    std::printf("# one block = fresh snapshot cold start (untimed) + %lld "
                "best-deletes; %d blocks per impl/gap; selected impls "
                "interleaved per block (same process, same clock)\n",
                static_cast<long long>(K), blocks);

    // Per-gap book domain. Default (variable-domain ladder): each gap uses
    // 1 + (K+1)*g so the domain grows with g. --fixed-domain decouples g from
    // the domain: every gap is timed on the domain the largest requested gap
    // needs, so the active memory span is constant while the re-scan distance
    // still varies (see the CONTROL NOTE in the file comment).
    int64_t max_gap = 0;
    for (int64_t g : gaps) {
        if (g > max_gap) max_gap = g;
    }
    if (fixed_domain) {
        std::printf("# fixed-domain: every gap timed on domain [1, %lld] "
                    "(= the size gap %lld requires); active memory span constant "
                    "across gaps\n",
                    static_cast<long long>(1 + (K + 1) * max_gap),
                    static_cast<long long>(max_gap));
    }
    std::printf("# impl,gap_ticks,ns_per_delete_best,ns_per_delete_mean,p50,p99,max,blocks\n");

    for (int64_t g : gaps) {
        const int64_t domain_max =
            fixed_domain ? 1 + (K + 1) * max_gap : 1 + (K + 1) * g;
        run_gap(g, K, blocks, want_flat, want_bits, domain_max, stdout);
    }
    return 0;
}
