// Order-book correctness tests.
//
// Every scenario is run against BOTH MapOrderBook (the oracle) and
// FlatOrderBook. Both books are driven over the identical sequence of
// well-formed updates, so parity of observable state is a strong check that
// FlatOrderBook's dense addressing and best-price caching agree with the map.
//
// A plain CHECK macro reports the file/line of the first failing assertion.
// Any failed CHECK accumulates into a total counter, and main() returns
// non-zero if that total is non-zero, so CTest genuinely fails on a bad run.
// No external test framework is needed.
//
// Build each book pair over the same configured domain: the Books fixture
// defaults to the shared [1, 200000] band, and callers may request a different
// shared domain via Books(domain_min, domain_max).
//
// Exit-code self-test: set LLDB_SELFTEST_FAIL=1 to run only a deliberately
// failing suite and exit through the normal path; the caller can assert the
// exit status is non-zero.

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

// Accumulated across ALL suites; never reset, so the process exit status
// reflects any CHECK failure anywhere in the run.
int g_failures_total = 0;

void summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("[ok] %-42s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-42s (%d/%d checks FAILED)\n", suite, g_failures, g_checks);
    }
    g_failures_total += g_failures;
    g_checks   = 0;
    g_failures = 0; // per-suite count only; the total is preserved
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

    explicit Books(int64_t min_t = 1, int64_t max_t = 200000)
        : map(MapOrderBook(min_t, max_t)), flat(FlatOrderBook(min_t, max_t)) {}

    void load(const BookSnapshot& s) {
        map.load_snapshot(s);
        flat.load_snapshot(s);
    }

    // Feed the same snapshot to both books and require each to accept it.
    void load_expect_ok(const BookSnapshot& s, const char* tag = "load_expect_ok") {
        const bool ok_m = map.load_snapshot(s);
        const bool ok_f = flat.load_snapshot(s);
        if (!(ok_m && ok_f)) {
            std::printf("   [%s] snapshot rejected: map=%d flat=%d (expected both ok)\n",
                        tag, ok_m ? 1 : 0, ok_f ? 1 : 0);
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
    BookSnapshot s = make_snapshot(100,
                                   {{500, 1}, {499, 2}, {498, 4}},
                                   {{501, 1}, {502, 2}});
    b.load_expect_ok(s, "initial snapshot");
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
            BookSnapshot snap = seed_snapshot(seq);
            const bool ok_m = b.map.load_snapshot(snap);
            const bool ok_f = b.flat.load_snapshot(snap);
            CHECK(ok_m);
            CHECK(ok_f);
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

// ---------------------------------------------------------------------------
// 12. A negative quantity on a brand-new, in-order sequence is corrupt: both
//     books return InvalidUpdate and become unsynced, and the seq is NOT
//     consumed (last_applied_seq() is unchanged).
// ---------------------------------------------------------------------------
void test_negative_qty_invalidates() {
    Books b;
    b.load_expect_ok(make_snapshot(10, {{100, 5}}, {{200, 5}}), "neg-qty seed");
    CHECK(b.map.synced());
    CHECK(b.flat.synced());

    L2Update bad{11, 100, -4, Side::Bid};
    CHECK(b.map.apply(bad) == ApplyResult::InvalidUpdate);
    CHECK(b.flat.apply(bad) == ApplyResult::InvalidUpdate);
    CHECK(!b.map.synced());
    CHECK(!b.flat.synced());
    // Sequence not consumed -> cannot self-heal.
    CHECK(b.map.last_applied_seq() == 10);
    CHECK(b.flat.last_applied_seq() == 10);
    b.check_equal("unsynced after invalid");
    // Both still agree, both need a snapshot.
    CHECK(b.map.apply(L2Update{12, 100, 6, Side::Bid}) == ApplyResult::Stale);
    CHECK(b.flat.apply(L2Update{12, 100, 6, Side::Bid}) == ApplyResult::Stale);
}

// ---------------------------------------------------------------------------
// 13. A brand-new seq whose price is outside the shared domain is refused but
//     does NOT desync: OutOfRange, seq consumed, book stays synced and usable.
//     (Both books must return the identical result.)
// ---------------------------------------------------------------------------
void test_out_of_range_update() {
    Books b; // default domain [1, 200000]
    b.load_expect_ok(make_snapshot(10, {{100, 5}}, {{200, 5}}), "oor seed");
    CHECK(b.map.synced());
    CHECK(b.flat.synced());

    // Well above the band.
    L2Update hi{11, 999'999'999, 7, Side::Bid};
    CHECK(b.map.apply(hi) == ApplyResult::OutOfRange);
    CHECK(b.flat.apply(hi) == ApplyResult::OutOfRange);
    CHECK(b.map.synced());
    CHECK(b.flat.synced());
    CHECK(b.map.last_applied_seq() == 11);
    CHECK(b.flat.last_applied_seq() == 11);
    b.check_equal("after oob, still synced");
    // Best prices unchanged (update ignored).
    CHECK(b.flat.best_bid() == 100);

    // Stream is contiguous: the next in-range update applies normally.
    CHECK(b.map.apply(L2Update{12, 100, 8, Side::Bid}) == ApplyResult::Applied);
    CHECK(b.flat.apply(L2Update{12, 100, 8, Side::Bid}) == ApplyResult::Applied);
    CHECK(b.map.synced());
    CHECK(b.flat.synced());
    b.check_equal("in-range update after oob");
    CHECK(b.flat.best_bid_qty() == 8);
}

// ---------------------------------------------------------------------------
// 14. Malformed snapshots are rejected wholesale by BOTH books: they return
//     false, leave all prior state intact, and do not flip synced().
// ---------------------------------------------------------------------------
void test_malformed_snapshot() {
    // Mismatched prices/qtys arrays (the book must not silently truncate).
    {
        Books b;
        b.load_expect_ok(make_snapshot(10, {{100, 5}}, {{200, 5}}), "mal seed");

        BookSnapshot s = make_snapshot(20, {{100, 5}}, {{200, 5}});
        s.bids.prices = {100, 90}; // 2 prices, 1 qty
        CHECK(!b.map.load_snapshot(s));
        CHECK(!b.flat.load_snapshot(s));
        b.check_equal("unchanged after len-mismatch bid snapshot");
        CHECK(b.map.synced());
        CHECK(b.flat.synced());
        CHECK(b.map.best_bid() == 100);
        CHECK(b.flat.best_bid() == 100);
        CHECK(b.map.next_expected_seq() == 11);
        CHECK(b.flat.next_expected_seq() == 11);

        // A good snapshot afterwards still works (nothing wedged).
        b.load_expect_ok(make_snapshot(30, {{500, 1}}, {{600, 1}}), "recover after mal");
    }

    // Negative qty in a snapshot.
    {
        Books b;
        BookSnapshot s = make_snapshot(5, {{100, -1}}, {});
        CHECK(!b.map.load_snapshot(s));
        CHECK(!b.flat.load_snapshot(s));
        CHECK(!b.map.synced());
        CHECK(!b.flat.synced());
    }

    // Price outside the configured domain.
    {
        Books b(1, 1000); // shared narrow band
        BookSnapshot s = make_snapshot(5, {{100, 1}, {5000, 1}}, {});
        CHECK(!b.map.load_snapshot(s));
        CHECK(!b.flat.load_snapshot(s));
        b.check_equal("empty/unchanged after oor snapshot");
    }

    // Duplicate price on the same side.
    {
        Books b;
        BookSnapshot s = make_snapshot(5, {{100, 1}, {100, 2}}, {});
        CHECK(!b.map.load_snapshot(s));
        CHECK(!b.flat.load_snapshot(s));
    }
}

// ---------------------------------------------------------------------------
// 15. Adjacent best-price deletion. Best and next-best sit adjacent to each
//     other inside a domain whose far edge is well away; deleting the current
//     best must promote the ADJACENT next-best, not scan from the far edge.
//
//     This is a CORRECTNESS test, so the domain is deliberately modest
//     ([1, 1M] => 16 MB of flat storage, not gigabytes). It verifies only that
//     the inward re-scan finds the adjacent level. Best-deletion SCAN COST is
//     a throughput property and belongs in the benchmark (workload C), not in
//     a unit-test-sized book.
// ---------------------------------------------------------------------------
void test_large_domain_adjacent_best_delete() {
    // Domain [1, 1_000_000]: flat storage is 2 * 1e6 * 8 bytes = 16 MB. A best
    // at 900_000 with the next-best at 899_999 keeps the new best directly
    // adjacent while the far edge of the domain (price 1_000_000) is ~100k
    // ticks away — far enough that a scan from the wrong extreme would have to
    // traverse most of the domain.
    const int64_t MIN_T = 1, MAX_T = 1'000'000;
    const int64_t kBest = 900'000, kNext = 899'999, kLow = 100'000;
    Books b(MIN_T, MAX_T);

    BookSnapshot snap = make_snapshot(
        10,
        {{kBest, 5}, {kNext, 2}, {kLow, 1}}, // bids desc: best, then adjacent next, then low
        {{kBest + 1, 3}, {kBest + 2, 4}});
    b.load_expect_ok(snap, "large-domain seed");

    CHECK(b.map.best_bid() == kBest);
    CHECK(b.flat.best_bid() == kBest);

    // Deleting the best bid promotes the ADJACENT next-best bid.
    b.apply(L2Update{11, kBest, 0, Side::Bid}, ApplyResult::Applied);
    b.check_equal("adjacent next-best promoted");
    CHECK(b.flat.best_bid() == kNext);

    // Ask side: delete the best ask (at kBest+1); next ask kBest+2 is adjacent.
    b.apply(L2Update{12, kBest + 1, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("adjacent next-best ask promoted");
    CHECK(b.flat.best_ask() == kBest + 2);

    // Exhaust a side to empty; the inward scan must terminate cleanly.
    b.apply(L2Update{13, kBest + 2, 0, Side::Ask}, ApplyResult::Applied);
    b.check_equal("ask side emptied");
    CHECK(b.flat.best_ask() == 0);
}

// ---------------------------------------------------------------------------
// 16. Exit-code self-test. A failed CHECK must produce a non-zero exit status.
//     This function is not run in the normal suite; it is invoked via the
//     LLDB_SELFTEST_FAIL environment variable (see main). It deliberately
//     fails one CHECK and returns, so the caller can assert the exit code is 1
//     and therefore that CTest would report a failure.
// ---------------------------------------------------------------------------
void self_test_exit_code() {
    std::printf("  (self-test) intentionally failing one CHECK...\n");
    CHECK(1 == 2); // must trip
}

} // namespace

int main() {
    // Exit-code self-test mode: run ONLY the deliberately-failing suite so the
    // caller can verify the process returns non-zero when a CHECK fails.
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("exit-code self-test: expecting non-zero exit\n");
        self_test_exit_code();
        summary("self-test (expected FAIL)");
        // Same exit logic as the normal path: the tripped CHECK must drive the
        // process to exit 1. If the runner's exit-code handling regresses, this
        // returns 0 and the shell assertion below catches it.
        return g_failures_total == 0 ? 0 : 1;
    }

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
    test_negative_qty_invalidates();
    test_out_of_range_update();
    test_malformed_snapshot();
    test_large_domain_adjacent_best_delete();

    summary("all suites");
    // g_failures_total is never reset, so ANY failed CHECK anywhere yields a
    // non-zero exit and makes CTest report failure.
    return g_failures_total == 0 ? 0 : 1;
}
