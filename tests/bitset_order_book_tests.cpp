// BitsetFlatOrderBook correctness tests (Experiment 01 Optimization Study).
//
// BitsetFlatOrderBook must be observably identical to FlatOrderBook (same
// externally visible semantics) while discovering the next best price through
// an occupancy bitmap hierarchy instead of a linear scan. Every scenario below
// drives THREE books — MapOrderBook (the oracle), FlatOrderBook, and
// BitsetFlatOrderBook — over the identical sequence of updates and requires all
// three to agree on every observable: apply result, synced state, expected/last
// sequence, best bid/ask PRICE AND QUANTITY, and the full level set.
//
// Alongside the differential checks, the targeted scenarios pin down the
// bitmap-specific edge cases from the spec:
//   * same-word next best (within one 64-level L0 word),
//   * crossing the 64-level L0 word boundary,
//   * crossing the L1 boundary (previous non-empty L0 word more than one L0
//     word away),
//   * crossing the L2 region (occupied groups > ~64 L0 words apart, forcing an
//     L2-word skip),
//   * sparse gaps, a side with a single level, an emptied side,
//   * first/last domain levels (min/max price) and out-of-domain updates,
//   * snapshot load + incremental replay.
//
// A plain CHECK macro reports the file/line of the first failing assertion;
// failures accumulate into a total and main() returns non-zero on any failure.
// The suite doubles as an exit-code self-test (LLDB_SELFTEST_FAIL).

#include "bitset_flat_order_book.h"
#include "flat_order_book.h"
#include "map_order_book.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

using llob::ApplyResult;
using llob::BitsetFlatOrderBook;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::MapOrderBook;
using llob::Side;
using llob::SideSnapshot;

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_failures;                                                   \
            std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

// Accumulated across ALL suites; never reset, so the process exit status
// reflects any CHECK failure anywhere in the run.
int g_failures_total = 0;

void summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("[ok] %-44s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-44s (%d/%d checks FAILED)\n", suite, g_failures, g_checks);
    }
    g_failures_total += g_failures;
    g_checks   = 0;
    g_failures = 0; // per-suite count only; the total is preserved
}

// ---------------------------------------------------------------------------
// Helpers to build snapshots compactly ((price, qty) pairs).
// ---------------------------------------------------------------------------
SideSnapshot side_from(const std::vector<std::pair<int64_t, int64_t>>& lv) {
    SideSnapshot s;
    s.prices.reserve(lv.size());
    s.qtys.reserve(lv.size());
    for (const auto& [p, q] : lv) {
        s.prices.push_back(p);
        s.qtys.push_back(q);
    }
    return s;
}

BookSnapshot make_snapshot(uint64_t seq,
                           std::vector<std::pair<int64_t, int64_t>> bids,
                           std::vector<std::pair<int64_t, int64_t>> asks) {
    BookSnapshot s;
    s.seq  = seq;
    s.bids = side_from(bids);
    s.asks = side_from(asks);
    return s;
}

// ---------------------------------------------------------------------------
// Shared three-book fixture.
// ---------------------------------------------------------------------------
struct Books {
    MapOrderBook        map;
    FlatOrderBook       flat;
    BitsetFlatOrderBook bits;

    explicit Books(int64_t min_t = 1, int64_t max_t = 200000)
        : map(MapOrderBook(min_t, max_t)),
          flat(FlatOrderBook(min_t, max_t)),
          bits(BitsetFlatOrderBook(min_t, max_t)) {}

    void load(const BookSnapshot& s) {
        map.load_snapshot(s);
        flat.load_snapshot(s);
        bits.load_snapshot(s);
    }

    void load_expect_ok(const BookSnapshot& s, const char* tag = "load_expect_ok") {
        const bool ok_m = map.load_snapshot(s);
        const bool ok_f = flat.load_snapshot(s);
        const bool ok_b = bits.load_snapshot(s);
        if (!(ok_m && ok_f && ok_b)) {
            std::printf("   [%s] snapshot rejected: map=%d flat=%d bits=%d "
                        "(expected all ok)\n",
                        tag, ok_m ? 1 : 0, ok_f ? 1 : 0, ok_b ? 1 : 0);
            ++g_failures;
            return;
        }
        check_equal(tag);
    }

    void load(uint64_t seq,
              std::vector<std::pair<int64_t, int64_t>> bids,
              std::vector<std::pair<int64_t, int64_t>> asks) {
        load(make_snapshot(seq, std::move(bids), std::move(asks)));
    }

    // Apply to all three and require each to report `expect`.
    void apply(const L2Update& u, ApplyResult expect) {
        const ApplyResult rm = map.apply(u);
        const ApplyResult rf = flat.apply(u);
        const ApplyResult rb = bits.apply(u);
        if (rm != expect || rf != expect || rb != expect) {
            std::printf(
                "   apply seq=%llu p=%lld q=%lld side=%d -> map=%d flat=%d "
                "bits=%d (expect %d)\n",
                static_cast<unsigned long long>(u.seq),
                static_cast<long long>(u.price),
                static_cast<long long>(u.qty),
                static_cast<int>(u.side), static_cast<int>(rm),
                static_cast<int>(rf), static_cast<int>(rb),
                static_cast<int>(expect));
            ++g_failures;
            return; // don't accumulate further noise
        }
    }

    // Require ALL observable state to match across the three books, including
    // best-quantity parity (not just best price) and full level-set parity.
    void check_equal(const char* tag) {
        const bool ok =
            map.best_bid() == flat.best_bid() &&
            map.best_bid() == bits.best_bid() &&
            map.best_ask() == flat.best_ask() &&
            map.best_ask() == bits.best_ask() &&
            map.best_bid_qty() == flat.best_bid_qty() &&
            map.best_bid_qty() == bits.best_bid_qty() &&
            map.best_ask_qty() == flat.best_ask_qty() &&
            map.best_ask_qty() == bits.best_ask_qty() &&
            map.synced() == flat.synced() &&
            map.synced() == bits.synced() &&
            map.next_expected_seq() == flat.next_expected_seq() &&
            map.next_expected_seq() == bits.next_expected_seq() &&
            map.last_applied_seq() == flat.last_applied_seq() &&
            map.last_applied_seq() == bits.last_applied_seq() &&
            map.level_count() == flat.level_count() &&
            map.level_count() == bits.level_count();

        if (!ok) {
            std::printf(
                "   [%s] state mismatch: map(bid=%lld ask=%lld syn=%d exp=%llu "
                "lvl=%zu) flat(bid=%lld ask=%lld) bits(bid=%lld ask=%lld syn=%d "
                "exp=%llu lvl=%zu)\n",
                tag, static_cast<long long>(map.best_bid()),
                static_cast<long long>(map.best_ask()), map.synced() ? 1 : 0,
                static_cast<unsigned long long>(map.next_expected_seq()),
                map.level_count(),
                static_cast<long long>(flat.best_bid()),
                static_cast<long long>(flat.best_ask()),
                static_cast<long long>(bits.best_bid()),
                static_cast<long long>(bits.best_ask()), bits.synced() ? 1 : 0,
                static_cast<unsigned long long>(bits.next_expected_seq()),
                bits.level_count());
            ++g_failures;
            return;
        }

        // Full level-set parity: every (price, qty) the map holds must be
        // reported identically by the flat book and the bitset book. Combined
        // with the level_count() equality above, this catches both missing and
        // phantom levels in either flat implementation.
        for (const auto& [p, q] : map.bids()) {
            if (q == 0) continue;
            if (flat.level_qty(p, Side::Bid) != q ||
                bits.level_qty(p, Side::Bid) != q) {
                std::printf("   [%s] bid level mismatch at %lld: map=%lld "
                            "flat=%lld bits=%lld\n",
                            tag, static_cast<long long>(p),
                            static_cast<long long>(q),
                            static_cast<long long>(flat.level_qty(p, Side::Bid)),
                            static_cast<long long>(bits.level_qty(p, Side::Bid)));
                ++g_failures;
                return;
            }
        }
        for (const auto& [p, q] : map.asks()) {
            if (q == 0) continue;
            if (flat.level_qty(p, Side::Ask) != q ||
                bits.level_qty(p, Side::Ask) != q) {
                std::printf("   [%s] ask level mismatch at %lld: map=%lld "
                            "flat=%lld bits=%lld\n",
                            tag, static_cast<long long>(p),
                            static_cast<long long>(q),
                            static_cast<long long>(flat.level_qty(p, Side::Ask)),
                            static_cast<long long>(bits.level_qty(p, Side::Ask)));
                ++g_failures;
                return;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// 1. Same-word next best. Best and next-best sit in the SAME 64-level L0 word:
//    the bitmap must find the next best in the lower (bid) / upper (ask) bits
//    of the current word without climbing the hierarchy.
// ---------------------------------------------------------------------------
void test_same_word_next_best() {
    // Domain [1, 200000]; slot = price - 1. Slots 100 and 99 are both in L0
    // word 1 (slots 64..127).
    Books b;
    b.load(0, {{101, 5}, {100, 3}}, {{100, 3}, {101, 7}});

    // Bid: delete best bid (price 101) -> next-best 100 in the same word.
    b.apply(L2Update{1, 101, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("same-word bid");
    CHECK(b.bits.best_bid() == 100);

    // Ask: delete best ask (price 100) -> next-best 101 in the same word.
    b.apply(L2Update{2, 100, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("same-word ask");
    CHECK(b.bits.best_ask() == 101);
}

// ---------------------------------------------------------------------------
// 2. Crossing the 64-level L0 word boundary.
//    Bid: best at price 65 (slot 64, first slot of word 1); next best at price
//    64 (slot 63, last slot of word 0). Deleting the best must jump to the
//    preceding word's highest bit.
//    Ask: mirror, best at price 64 (slot 63, last slot of word 0); next at
//    price 65 (slot 64, first slot of word 1), crossing upward.
// ---------------------------------------------------------------------------
void test_cross_l0_word_boundary() {
    Books b;
    b.load(0, {{65, 5}, {64, 3}}, {{64, 3}, {65, 7}});

    b.apply(L2Update{1, 65, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("cross L0 word boundary bid");
    CHECK(b.bits.best_bid() == 64);

    b.apply(L2Update{2, 64, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("cross L0 word boundary ask");
    CHECK(b.bits.best_ask() == 65);
}

// ---------------------------------------------------------------------------
// 3. Crossing the L1 boundary / a large sparse gap (bid and ask). Two levels
//    ~4,160 slots apart: the gap spans several L0 words, so deleting the best
//    must climb L0 -> L1 (and here, an L1-word skip) to find the far level.
// ---------------------------------------------------------------------------
void test_cross_l1_gap() {
    // price 4164 -> slot 4163 (L0 word 65); price 4 -> slot 3 (L0 word 0).
    Books b;
    b.load(0, {{4164, 5}, {4, 3}}, {{4, 3}, {4164, 7}});

    b.apply(L2Update{1, 4164, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("large-gap bid (L1 skip)");
    CHECK(b.bits.best_bid() == 4);

    b.apply(L2Update{2, 4, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("large-gap ask (L1 skip)");
    CHECK(b.bits.best_ask() == 4164);
}

// ---------------------------------------------------------------------------
// 4. Crossing the L2 region. Occupied levels ~266,300 slots apart: the far L0
//    word lives in a different L1 word AND a different L2 word, so the delete
//    must walk all three summary levels (including an L2-word skip).
//    Needs a domain large enough to contain the address: span 300,000.
// ---------------------------------------------------------------------------
void test_cross_l2_region() {
    // price 266305 -> slot 266304 -> L0 word 4161 -> L1 word 65 -> L2 word 1.
    // price 4      -> slot 3      -> L0 word 0   -> L1 word 0 -> L2 word 0.
    const int64_t HI = 300000; // span 300000 -> L0: 4688 words, L1: 74, L2: 2
    Books b(1, HI);
    b.load(0, {{266305, 5}, {4, 3}}, {{4, 3}, {266305, 7}});

    b.apply(L2Update{1, 266305, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("L2-region bid (far gap)");
    CHECK(b.bits.best_bid() == 4);

    b.apply(L2Update{2, 4, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("L2-region ask (far gap)");
    CHECK(b.bits.best_ask() == 266305);
}

// ---------------------------------------------------------------------------
// 5. Sparse gaps with several far-apart levels; deleting the best must always
//    land on the NEXT-best (not skip it), walking in from whatever distance.
// ---------------------------------------------------------------------------
void test_sparse_gaps() {
    // Bids descending: 500000, 400000, 100000, 90000 (skipping far).
    // Asks ascending: 90001, 100001, 400001, 500001.
    const int64_t HI = 1'000'000;
    Books b(1, HI);
    b.load(0,
           {{500000, 1}, {400000, 1}, {100000, 1}, {90000, 1}},
           {{90001, 1}, {100001, 1}, {400001, 1}, {500001, 1}});

    // Drain the bid side best-first; each new best must be the next one down.
    b.apply(L2Update{1, 500000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("sparse drain bid 1");
    CHECK(b.bits.best_bid() == 400000);
    b.apply(L2Update{2, 400000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("sparse drain bid 2");
    CHECK(b.bits.best_bid() == 100000);
    b.apply(L2Update{3, 100000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("sparse drain bid 3");
    CHECK(b.bits.best_bid() == 90000);
    b.apply(L2Update{4, 90000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("sparse drain bid empty");
    CHECK(b.bits.best_bid() == 0);

    // Drain the ask side best-first; each new best must be the next one up.
    b.apply(L2Update{5, 90001, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("sparse drain ask 1");
    CHECK(b.bits.best_ask() == 100001);
    b.apply(L2Update{6, 100001, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("sparse drain ask 2");
    CHECK(b.bits.best_ask() == 400001);
    b.apply(L2Update{7, 400001, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("sparse drain ask 3");
    CHECK(b.bits.best_ask() == 500001);
    b.apply(L2Update{8, 500001, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("sparse drain ask empty");
    CHECK(b.bits.best_ask() == 0);
    CHECK(b.bits.empty());
}

// ---------------------------------------------------------------------------
// 6. Only one level on a side; deleting it empties that side (and only that
//    side). Also: a single level far from the domain's best-end.
// ---------------------------------------------------------------------------
void test_single_level_and_empty_side() {
    Books b;
    b.load(0, {{12345, 5}}, {{200000, 5}});

    b.apply(L2Update{1, 12345, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("single bid removed");
    CHECK(b.bits.best_bid() == 0);
    CHECK(b.bits.best_ask() == 200000); // ask untouched
    CHECK(!b.bits.empty());

    b.apply(L2Update{2, 200000, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("single ask removed");
    CHECK(b.bits.best_ask() == 0);
    CHECK(b.bits.empty());
}

// ---------------------------------------------------------------------------
// 7. Domain edge levels: the best sits at the very first/last addressable slot
//    (tick_min / tick_max). Bid at the top of the domain, ask at the bottom;
//    a side with its only level at the extreme; out-of-range updates ignored.
// ---------------------------------------------------------------------------
void test_domain_boundaries() {
    const int64_t MIN_T = 1, MAX_T = 64; // tiny span: one partial L0 word
    Books b(MIN_T, MAX_T);

    // best bid AT tick_max (slot 63, word 0's top bit); best ask AT tick_min
    // (slot 0). Only one level each side.
    b.load(0, {{64, 5}}, {{1, 7}});
    CHECK(b.bits.best_bid() == 64);
    CHECK(b.bits.best_ask() == 1);

    // Delete the top-of-domain bid -> bid side empty (no slot below slot 63).
    b.apply(L2Update{1, 64, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("top-of-domain bid removed");
    CHECK(b.bits.best_bid() == 0);

    // Re-add it above nothing and delete again (new best handling at the edge).
    b.apply(L2Update{2, 64, 9, Side::Bid}, ApplyResult::Applied);
    b.check_equal("top-of-domain bid restored");
    CHECK(b.bits.best_bid() == 64);

    // Delete the bottom-of-domain ask -> ask side empty.
    b.apply(L2Update{3, 1, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("bottom-of-domain ask removed");
    CHECK(b.bits.best_ask() == 0);

    // Out-of-range is refused (seq consumed, state untouched) on all three.
    b.apply(L2Update{4, 1000, 3, Side::Ask}, ApplyResult::OutOfRange);
    b.apply(L2Update{5, 0, 3, Side::Ask}, ApplyResult::OutOfRange);
    b.check_equal("out-of-range ignored");
    CHECK(b.bits.best_bid() == 64);
    CHECK(b.bits.best_ask() == 0);
}

// ---------------------------------------------------------------------------
// 8. Snapshot load with a sparse/degenerate layout (exercises the cold-path
//    hierarchy build: set_occ propagation and best from an empty book), then
//    incremental replay.
// ---------------------------------------------------------------------------
void test_snapshot_and_incremental_replay() {
    // Domain with MIN != 1 to exercise the price->slot offset arithmetic.
    const int64_t MIN_T = 1000, MAX_T = 301000; // slot = price - 1000
    Books b(MIN_T, MAX_T);

    // Sparse snapshot straddling every hierarchy boundary; empty asks.
    b.load_expect_ok(
        make_snapshot(50,
                      {{301000, 4}, {300000, 3}, {266304, 2}, {1001, 1}},
                      {}),
        "sparse seed");
    CHECK(b.bits.best_bid() == 301000);
    CHECK(b.bits.best_ask() == 0);

    // Incremental replay that crosses word boundaries as it goes.
    b.apply(L2Update{51, 301000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay delete top");
    CHECK(b.bits.best_bid() == 300000);

    b.apply(L2Update{52, 300000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay delete second");
    CHECK(b.bits.best_bid() == 266304);

    // A new bid HIGHER than the current best appears (cache promote path).
    b.apply(L2Update{53, 301000, 7, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay promote higher bid");
    CHECK(b.bits.best_bid() == 301000);
    CHECK(b.bits.best_bid_qty() == 7);

    // Re-quantify a present level (positive -> positive, bitmap untouched).
    b.apply(L2Update{54, 1001, 42, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay re-quantify far level");
    CHECK(b.bits.best_bid() == 301000);
    CHECK(b.bits.level_qty(1001, Side::Bid) == 42);

    // Drain a side to empty through several words.
    b.apply(L2Update{55, 301000, 0, Side::Bid}, ApplyResult::Applied);
    b.apply(L2Update{56, 300000, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay drain down");
    CHECK(b.bits.best_bid() == 266304);
    b.apply(L2Update{57, 266304, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay drain below L2 region");
    CHECK(b.bits.best_bid() == 1001);
    b.apply(L2Update{58, 1001, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("replay drained empty");
    CHECK(b.bits.best_bid() == 0);
    CHECK(b.bits.best_ask() == 0);
    CHECK(b.bits.empty());

    // The book must still accept a fresh snapshot afterwards.
    b.load_expect_ok(make_snapshot(100, {{2000, 3}}, {{300000, 5}}), "reload");
    CHECK(b.bits.best_bid() == 2000);
    CHECK(b.bits.best_ask() == 300000);
}

// ---------------------------------------------------------------------------
// 9. Sequence semantics identical across the three books: stale replay, gap
//    desync, invalid (negative qty) refusal, out-of-range, recovery.
// ---------------------------------------------------------------------------
void test_sequence_semantics() {
    Books b;
    b.load_expect_ok(make_snapshot(10, {{100, 5}, {99, 1}}, {{200, 5}}), "seq seed");
    CHECK(b.bits.synced());
    CHECK(b.bits.next_expected_seq() == 11);

    b.apply(L2Update{11, 100, 6, Side::Bid}, ApplyResult::Applied);
    b.apply(L2Update{11, 100, 6, Side::Bid}, ApplyResult::Stale); // replay
    b.apply(L2Update{17, 100, 7, Side::Bid}, ApplyResult::GapDetected); // 12 -> 17
    CHECK(!b.bits.synced());
    b.apply(L2Update{18, 100, 8, Side::Bid}, ApplyResult::Stale); // unsynced
    b.check_equal("unsynced stale");

    // Negative qty refuses and desyncs without consuming the sequence.
    Books b2;
    b2.load_expect_ok(make_snapshot(10, {{100, 5}}, {{200, 5}}), "neg seed");
    L2Update bad{11, 100, -4, Side::Bid};
    CHECK(b2.map.apply(bad) == ApplyResult::InvalidUpdate);
    CHECK(b2.flat.apply(bad) == ApplyResult::InvalidUpdate);
    CHECK(b2.bits.apply(bad) == ApplyResult::InvalidUpdate);
    CHECK(!b2.bits.synced());
    CHECK(b2.bits.last_applied_seq() == 10);
    b2.check_equal("invalid unsynced");

    // Recovery via snapshot.
    b2.load_expect_ok(make_snapshot(30, {{50, 1}}, {{300, 1}}), "recover");
    CHECK(b2.bits.synced());
    CHECK(b2.bits.best_bid() == 50);
    CHECK(b2.bits.best_ask() == 300);
}

// ---------------------------------------------------------------------------
// 10. Differential fuzz across a domain large enough to exercise all three
//     hierarchy levels (span 300000: L0 words 4688, L1 74, L2 2), biased toward
//     the best price, with periodic snapshot resyncs. The three books must stay
//     identical throughout.
// ---------------------------------------------------------------------------
void test_differential_fuzz() {
    Books b(1, 300000);

    std::mt19937 rng(0xB17F1A7);
    const int64_t lo = 1;
    const int64_t hi = 300000;
    std::uniform_int_distribution<int64_t> price(lo, hi);
    std::uniform_int_distribution<int64_t> qty(0, 1000);
    std::uniform_int_distribution<int>      which(0, 99);

    auto seed_snapshot = [&](uint64_t seq) {
        std::vector<std::pair<int64_t, int64_t>> bids, asks;
        for (int64_t p = lo; p <= hi; p += 7) {
            if (p % 3 == 0) bids.emplace_back(p, 1 + static_cast<int64_t>(qty(rng)));
            else            asks.emplace_back(p, 1 + static_cast<int64_t>(qty(rng)));
        }
        return make_snapshot(seq, std::move(bids), std::move(asks));
    };

    uint64_t seq = 0;
    for (int iter = 0; iter < 8000; ++iter) {
        if (iter % 1000 == 0) {
            seq = static_cast<uint64_t>(iter) * 1000;
            BookSnapshot snap = seed_snapshot(seq);
            CHECK(b.map.load_snapshot(snap));
            CHECK(b.flat.load_snapshot(snap));
            CHECK(b.bits.load_snapshot(snap));
            if (iter % 2000 == 0) b.check_equal("fuzz resync");
            continue;
        }
        const int r = which(rng);
        Side side;
        int64_t p;
        if (r < 30) {
            side = Side::Bid;
            p    = price(rng);
        } else if (r < 60) {
            side = Side::Ask;
            p    = price(rng);
        } else {
            // Heavy on the current best: delete/refresh whichever side.
            side = (r < 80) ? Side::Bid : Side::Ask;
            const int64_t best = (side == Side::Bid) ? b.map.best_bid()
                                                     : b.map.best_ask();
            // Most of the time hit the current best exactly; sometimes below.
            p = (best != 0 && (r % 3 != 0)) ? best : price(rng);
        }
        ++seq;
        b.apply(L2Update{seq, p, qty(rng), side}, ApplyResult::Applied);
        if (iter % 800 == 799) b.check_equal("fuzz mid-stream");
    }
    b.check_equal("fuzz final");
}

// ---------------------------------------------------------------------------
// 11. Exit-code self-test (mirrors order_book_tests): a failed CHECK must
//     produce a non-zero exit status.
// ---------------------------------------------------------------------------
void self_test_exit_code() {
    std::printf("  (self-test) intentionally failing one CHECK...\n");
    CHECK(1 == 2); // must trip
}

} // namespace

int main() {
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("exit-code self-test: expecting non-zero exit\n");
        self_test_exit_code();
        summary("self-test (expected FAIL)");
        return g_failures_total == 0 ? 0 : 1;
    }

    test_same_word_next_best();
    test_cross_l0_word_boundary();
    test_cross_l1_gap();
    test_cross_l2_region();
    test_sparse_gaps();
    test_single_level_and_empty_side();
    test_domain_boundaries();
    test_snapshot_and_incremental_replay();
    test_sequence_semantics();
    test_differential_fuzz();

    summary("all suites");
    return g_failures_total == 0 ? 0 : 1;
}
