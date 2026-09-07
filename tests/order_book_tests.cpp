// Order-book correctness tests.
//
// Every scenario is run against BOTH MapOrderBook (the oracle) and
// FlatOrderBook. Both books are driven over the identical sequence of
// well-formed updates, so parity of observable state is a strong check that
// FlatOrderBook's dense addressing and best-price caching agree with the map.
//
// A plain CHECK macro reports the file/line of the first failing assertion and
// exits non-zero. No external test framework is needed for Phase 1.

#include "flat_order_book.h"
#include "map_order_book.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

using llob::ApplyResult;
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

void summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("[ok] %-42s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-42s (%d/%d checks FAILED)\n", suite, g_failures, g_checks);
    }
    g_checks   = 0;
    g_failures = 0;
}

// ---------------------------------------------------------------------------
// Helpers to build snapshots compactly.
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

// make_snapshot(seq, {bids...}, {asks...})  — levels are (price, qty) pairs.
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
// Shared book fixture: drives MapOrderBook and FlatOrderBook identically.
// ---------------------------------------------------------------------------
struct Books {
    MapOrderBook   map;
    FlatOrderBook  flat;

    explicit Books() : flat(FlatOrderBook(1, 200000)) {}

    void load(const BookSnapshot& s) {
        map.load_snapshot(s);
        flat.load_snapshot(s);
    }

    void load(uint64_t seq,
              std::vector<std::pair<int64_t, int64_t>> bids,
              std::vector<std::pair<int64_t, int64_t>> asks) {
        load(make_snapshot(seq, std::move(bids), std::move(asks)));
    }

    // Apply to both, and require both books to report the same result.
    void apply(const L2Update& u, ApplyResult expect) {
        const ApplyResult rm = map.apply(u);
        const ApplyResult rf = flat.apply(u);
        if (rm != expect || rf != expect) {
            std::printf(
                "   apply seq=%llu p=%lld q=%lld side=%d -> map=%d flat=%d (expect %d)\n",
                static_cast<unsigned long long>(u.seq),
                static_cast<long long>(u.price),
                static_cast<long long>(u.qty),
                static_cast<int>(u.side), static_cast<int>(rm),
                static_cast<int>(rf), static_cast<int>(expect));
            ++g_failures;
            return; // don't accumulate further noise
        }
    }

    // Require all observable state to match between the two books.
    void check_equal(const char* tag) {
        const bool ok =
            map.best_bid() == flat.best_bid() &&
            map.best_ask() == flat.best_ask() &&
            map.synced() == flat.synced() &&
            map.next_expected_seq() == flat.next_expected_seq() &&
            map.last_applied_seq() == flat.last_applied_seq() &&
            map.level_count() == flat.level_count();

        if (!ok) {
            std::printf(
                "   [%s] state mismatch: map(bid=%lld ask=%lld syn=%d exp=%llu lvl=%zu) "
                "flat(bid=%lld ask=%lld syn=%d exp=%llu lvl=%zu)\n",
                tag, static_cast<long long>(map.best_bid()),
                static_cast<long long>(map.best_ask()), map.synced() ? 1 : 0,
                static_cast<unsigned long long>(map.next_expected_seq()),
                map.level_count(),
                static_cast<long long>(flat.best_bid()),
                static_cast<long long>(flat.best_ask()), flat.synced() ? 1 : 0,
                static_cast<unsigned long long>(flat.next_expected_seq()),
                flat.level_count());
            ++g_failures;
            return;
        }

        // Full level-set parity: for every (price, qty) the map holds, the flat
        // book must report the identical quantity.
        for (const auto& [p, q] : map.bids()) {
            if (q == 0) continue;
            if (flat.level_qty(p, Side::Bid) != q) {
                std::printf("   [%s] bid level mismatch at %lld: map=%lld flat=%lld\n",
                            tag, static_cast<long long>(p),
                            static_cast<long long>(q),
                            static_cast<long long>(flat.level_qty(p, Side::Bid)));
                ++g_failures;
                return;
            }
        }
        for (const auto& [p, q] : map.asks()) {
            if (q == 0) continue;
            if (flat.level_qty(p, Side::Ask) != q) {
                std::printf("   [%s] ask level mismatch at %lld: map=%lld flat=%lld\n",
                            tag, static_cast<long long>(p),
                            static_cast<long long>(q),
                            static_cast<long long>(flat.level_qty(p, Side::Ask)));
                ++g_failures;
                return;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// 1. Adding a bid and an ask; best prices become visible.
// ---------------------------------------------------------------------------
void test_add_and_best() {
    Books b;
    b.load(0, {{100, 5}}, {{200, 7}});
    b.check_equal("add bid/ask");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.best_ask() == 200);

    // Add a second level on each side.
    b.apply(L2Update{1, 95, 3, Side::Bid}, ApplyResult::Applied);
    b.apply(L2Update{2, 205, 4, Side::Ask}, ApplyResult::Applied);
    b.check_equal("add second level");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.best_ask() == 200);
}

// ---------------------------------------------------------------------------
// 2. Updating the quantity of an existing (non-best) level.
// ---------------------------------------------------------------------------
void test_update_quantity() {
    Books b;
    b.load(0, {{100, 5}, {95, 2}}, {{200, 7}, {205, 1}});
    b.apply(L2Update{1, 95, 9, Side::Bid}, ApplyResult::Applied); // non-best
    b.check_equal("update non-best qty");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.level_qty(95, Side::Bid) == 9);

    // Update the best bid itself; price stays, quantity changes.
    b.apply(L2Update{2, 100, 3, Side::Bid}, ApplyResult::Applied);
    b.check_equal("update best qty");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.best_bid_qty() == 3);
}

// ---------------------------------------------------------------------------
// 3. Deleting a level that is not the best.
// ---------------------------------------------------------------------------
void test_delete_non_best() {
    Books b;
    b.load(0, {{100, 5}, {95, 2}, {90, 1}}, {{200, 7}});
    b.apply(L2Update{1, 95, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("delete non-best");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.level_qty(95, Side::Bid) == 0);
}

// ---------------------------------------------------------------------------
// 4. Deleting the current best level (cache invalidated; scan finds new best).
// ---------------------------------------------------------------------------
void test_delete_best() {
    Books b;
    b.load(0, {{100, 5}, {95, 2}}, {{200, 7}});
    b.apply(L2Update{1, 100, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("delete best bid");
    CHECK(b.flat.best_bid() == 95); // promoted

    // Delete the last remaining bid on that side -> bid side empty.
    b.apply(L2Update{2, 95, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("delete last bid");
    CHECK(b.flat.best_bid() == 0);
    CHECK(b.flat.best_ask() == 200); // ask side untouched
}

// ---------------------------------------------------------------------------
// 5. Empty book / empty sides report best price 0 and are "empty".
// ---------------------------------------------------------------------------
void test_empty_book() {
    Books b;
    b.load(0, {}, {});
    CHECK(b.map.empty());
    CHECK(b.flat.empty());
    CHECK(b.map.best_bid() == 0);
    CHECK(b.map.best_ask() == 0);
    CHECK(b.flat.best_bid() == 0);
    CHECK(b.flat.best_ask() == 0);
    CHECK(b.flat.level_count() == 0);
}

// ---------------------------------------------------------------------------
// 6. A new, better best price appears (higher bid / lower ask).
// ---------------------------------------------------------------------------
void test_new_best() {
    Books b;
    b.load(0, {{100, 5}}, {{200, 7}});
    b.apply(L2Update{1, 101, 3, Side::Bid}, ApplyResult::Applied); // higher bid
    b.check_equal("higher bid");
    CHECK(b.flat.best_bid() == 101);

    b.apply(L2Update{2, 199, 2, Side::Ask}, ApplyResult::Applied); // lower ask
    b.check_equal("lower ask");
    CHECK(b.flat.best_ask() == 199);
}

// ---------------------------------------------------------------------------
// 7. Snapshot loading (full reset, best prices recomputed by scan).
// ---------------------------------------------------------------------------
void test_snapshot_load() {
    Books b;
    b.load(100,
           {{500, 1}, {499, 2}, {498, 4}},
           {{501, 1}, {502, 2}});
    b.check_equal("initial snapshot");
    CHECK(b.flat.best_bid() == 500);
    CHECK(b.flat.best_ask() == 501);
    CHECK(b.flat.synced());
    CHECK(b.flat.next_expected_seq() == 101);
}

// ---------------------------------------------------------------------------
// 8. Sequence gap desynchronizes the book.
// ---------------------------------------------------------------------------
void test_sequence_gap() {
    Books b;
    b.load(10, {{100, 5}}, {{200, 7}}); // next expected: 11
    b.apply(L2Update{11, 100, 6, Side::Bid}, ApplyResult::Applied);
    b.apply(L2Update{17, 100, 7, Side::Bid}, ApplyResult::GapDetected); // 12 -> 17
    CHECK(!b.map.synced());
    CHECK(!b.flat.synced());

    // While unsynced further updates cannot repair the book: they are all
    // ignored (Stale) — the book stays desynchronized until a snapshot.
    b.apply(L2Update{18, 100, 8, Side::Bid}, ApplyResult::Stale);
    b.apply(L2Update{25, 100, 9, Side::Ask}, ApplyResult::Stale);
    CHECK(!b.map.synced());
    CHECK(!b.flat.synced());
    b.check_equal("stale while unsynced");
}

// ---------------------------------------------------------------------------
// 9. A fresh snapshot resynchronizes a gap-desynced book.
// ---------------------------------------------------------------------------
void test_recovery_after_snapshot() {
    Books b;
    b.load(10, {{100, 5}}, {{200, 7}});
    b.apply(L2Update{14, 100, 6, Side::Bid}, ApplyResult::GapDetected);
    CHECK(!b.flat.synced());

    b.load(20, {{100, 1}, {90, 1}}, {{200, 2}});
    b.check_equal("recovered snapshot");
    CHECK(b.flat.synced());
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.best_ask() == 200);

    b.apply(L2Update{21, 90, 0, Side::Bid}, ApplyResult::Applied);
    b.apply(L2Update{22, 200, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("post-recovery updates");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.best_ask() == 0);
}

// ---------------------------------------------------------------------------
// 10. Best-price discipline across the spread (bids < asks when both present)
//     and independent per-side correctness under churn.
// ---------------------------------------------------------------------------
void test_bid_ask_correctness() {
    Books b;
    b.load(0, {{100, 5}, {99, 5}}, {{101, 5}, {102, 5}});
    CHECK(b.flat.best_bid() < b.flat.best_ask());

    // Churn the top bid: delete it, then restore it.
    b.apply(L2Update{1, 100, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("delete top bid");
    CHECK(b.flat.best_bid() == 99);
    CHECK(b.flat.best_bid() < b.flat.best_ask());

    b.apply(L2Update{2, 100, 9, Side::Bid}, ApplyResult::Applied);
    b.check_equal("restore top bid");
    CHECK(b.flat.best_bid() == 100);
    CHECK(b.flat.best_bid() < b.flat.best_ask());

    // New best on each side, then cross-check.
    b.apply(L2Update{3, 101, 1, Side::Bid}, ApplyResult::Applied);
    b.apply(L2Update{4, 101, 0, Side::Ask}, ApplyResult::Applied); // delete ask 101
    b.apply(L2Update{5, 100, 0, Side::Ask}, ApplyResult::Applied); // delete ask 100 (absent; idempotent)
    b.check_equal("crossed-spread sanity");
    CHECK(b.flat.best_bid() == 101);
    CHECK(b.flat.best_ask() == 102);
    CHECK(b.flat.best_bid() < b.flat.best_ask());
}

// ---------------------------------------------------------------------------
// 11. Differential fuzz: thousands of random well-formed updates applied to
//     both books; state must stay identical. Periodic snapshot resyncs
//     exercise the recovery path.
// ---------------------------------------------------------------------------
void test_differential_fuzz() {
    Books b;

    std::mt19937 rng(0xC0FFEE);
    const int64_t lo = 1000;
    const int64_t hi = 2000; // inside the flat domain [1, 200000]
    std::uniform_int_distribution<int64_t> price(lo, hi);
    std::uniform_int_distribution<int64_t> qty(0, 1000);
    std::uniform_int_distribution<int>      which(0, 99);

    auto seed_snapshot = [&](uint64_t seq) {
        std::vector<std::pair<int64_t, int64_t>> bids, asks;
        for (int64_t p = lo; p <= hi; p += 3)
            bids.emplace_back(p, 1 + static_cast<int64_t>(qty(rng)) + 1);
        for (int64_t p = lo + 1; p <= hi; p += 3)
            asks.emplace_back(p, 1 + static_cast<int64_t>(qty(rng)) + 1);
        return make_snapshot(seq, std::move(bids), std::move(asks));
    };

    uint64_t seq = 0;
    for (int iter = 0; iter < 5000; ++iter) {
        if (iter % 1000 == 0) {
            // Resync both books to the same state.
            seq = static_cast<uint64_t>(iter) * 1000;
            auto snap = seed_snapshot(seq);
            b.map.load_snapshot(snap);
            b.flat.load_snapshot(snap);
            if (iter % 3000 == 0) b.check_equal("fuzz resync");
            continue;
        }
        const int r = which(rng);
        Side side;
        int64_t p;
        if (r < 40) {
            side = Side::Bid;
            p    = price(rng);
        } else if (r < 80) {
            side = Side::Ask;
            p    = price(rng);
        } else {
            // Heavy on the best price: delete or refresh whichever side.
            side = (r < 90) ? Side::Bid : Side::Ask;
            const int64_t best =
                (side == Side::Bid) ? b.map.best_bid() : b.map.best_ask();
            // Half the time hit the current best exactly.
            p = (best != 0 && (r % 2 == 0)) ? best : price(rng);
        }
        ++seq;
        b.apply(L2Update{seq, p, qty(rng), side}, ApplyResult::Applied);
        if (iter % 500 == 499) b.check_equal("fuzz mid-stream");
    }
    b.check_equal("fuzz final");
}

} // namespace

int main() {
    test_add_and_best();
    test_update_quantity();
    test_delete_non_best();
    test_delete_best();
    test_empty_book();
    test_new_best();
    test_snapshot_load();
    test_sequence_gap();
    test_recovery_after_snapshot();
    test_bid_ask_correctness();
    test_differential_fuzz();

    summary("all suites");
    return g_failures == 0 ? 0 : 1;
}
