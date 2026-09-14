// Experiment 03 — market-data pipeline correctness tests.
//
// Five layers, each answering a question the others cannot:
//
//   1. The specification sketch, as a literal expected-outcome table. This is
//      the only layer that can catch a SPECIFICATION error: its expectations
//      were written by hand from the sketch, not derived from the code.
//   2. Hand-written scenario vectors — one per failure hypothesis, each with a
//      per-message outcome trace and a final book.
//   3. Accounting: the identity that every message has exactly one outcome, and
//      a coverage assertion that the corpus actually REACHES every state.
//      Without it, a differential test proving both sides agree on zero would
//      look like success.
//   4. Differential fuzz against tests/md_oracle.h, an independently written
//      reference implementation. Catches implementation divergence, NOT a
//      misread contract — layers 1 and 2 are for that.
//   5. Sink agreement: the same corrupted stream through MapOrderBook and
//      FlatOrderBook, which must never diverge.
//
// A plain CHECK macro reports file/line. Failed CHECKs accumulate and main()
// returns non-zero, so CTest genuinely fails on a bad run.
//
// Exit-code self-test: LLDB_SELFTEST_FAIL=1 runs only a deliberately failing
// suite and exits through the normal path, so the caller can assert non-zero.

#include "flat_order_book.h"
#include "map_order_book.h"
#include "market_data_pipeline.h"
#include "md_message.h"
#include "md_oracle.h"
#include "md_stream_gen.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using llob::FlatOrderBook;
using llob::MapOrderBook;
using llob::Side;

using llmd::LossCause;
using llmd::MdCounters;
using llmd::MdKind;
using llmd::MdMessage;
using llmd::MdOutcome;
using llmd::MdRecoveryEpisode;
using llmd::MdResult;
using llmd::MdState;
using llmd::MarketDataPipeline;

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

int g_failures_total = 0;

void summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("[ok] %-42s (%d checks)\n", suite, g_checks);
    } else {
        std::printf("[!!] %-42s (%d/%d checks FAILED)\n", suite, g_failures, g_checks);
    }
    g_failures_total += g_failures;
    g_checks   = 0;
    g_failures = 0;
}

// Report a divergence with the message that caused it. Every differential
// failure prints the recipe, so it can be re-run from (scenario, seed, recipe)
// without re-deriving anything.
void report(const char* what, const llmd::gen::MdTrace& t, std::size_t i,
            const std::string& detail) {
    ++g_failures;
    ++g_checks;
    std::printf("FAIL %s: %s  [%zu]  %s\n", what, t.recipe_text().c_str(), i,
                detail.c_str());
    if (i < t.messages.size()) {
        const MdMessage& m = t.messages[i];
        std::printf("       msg: %s seq=%llu %s price=%lld qty=%lld\n",
                    llmd::md_kind_name(m.kind),
                    static_cast<unsigned long long>(m.seq),
                    llob::is_bid(m.side) ? "Bid" : "Ask",
                    static_cast<long long>(m.price), static_cast<long long>(m.qty));
    }
}

[[maybe_unused]] const char* out_name(MdOutcome o) { return llmd::md_outcome_name(o); }

// ---------------------------------------------------------------------------
// Trace running.
// ---------------------------------------------------------------------------
struct Run {
    std::vector<MdOutcome>    outcomes;
    std::vector<MdState>      states;
    std::vector<std::uint64_t> expected;
    MdCounters                counters;
    MdState                   final_state;
    std::uint64_t             final_expected;
    std::uint64_t             final_cursor;
    std::int64_t              best_bid;
    std::int64_t              best_ask;
    std::size_t               level_count;
    bool                      synced;
};

template <class Book>
Run run(const std::vector<MdMessage>& msgs,
        typename MarketDataPipeline<Book>::Config cfg = {}) {
    MarketDataPipeline<Book> p{Book{}, cfg};
    Run r;
    r.outcomes.reserve(msgs.size());
    r.states.reserve(msgs.size());
    r.expected.reserve(msgs.size());
    for (const MdMessage& m : msgs) {
        const MdResult res = p.apply(m);
        r.outcomes.push_back(res.outcome);
        r.states.push_back(res.state_after);
        r.expected.push_back(res.expected_after);
        // The invariant that keeps the pipeline from drifting from its sink.
        // Checked here rather than in one suite so it holds on EVERY trace.
        if (p.live() && p.expected() != p.book().next_expected_seq()) {
            ++g_failures;
            std::printf("FAIL Live invariant broken: pipeline expects %llu, book expects %llu\n",
                        static_cast<unsigned long long>(p.expected()),
                        static_cast<unsigned long long>(p.book().next_expected_seq()));
        }
    }
    r.counters       = p.counters();
    r.final_state    = p.state();
    r.final_expected = p.expected();
    r.final_cursor   = p.cursor();
    r.best_bid       = p.book().best_bid();
    r.best_ask       = p.book().best_ask();
    r.level_count    = p.book().level_count();
    r.synced         = p.book().synced();
    return r;
}

// Shorthand constructors, so a scenario reads as a message list.
MdMessage B(std::uint64_t s) { return llmd::md_begin(s); }
MdMessage E(std::uint64_t s) { return llmd::md_end(s); }
MdMessage L(std::uint64_t s, Side side, std::int64_t px, std::int64_t qty) {
    return llmd::md_level(s, side, px, qty);
}

// Expected summary of a finished trace.
struct Expect {
    MdState       state = MdState::NotSynced;
    std::uint64_t expected = 0;
    std::int64_t  best_bid = 0;
    std::int64_t  best_ask = 0;
    std::size_t   level_count = 0;
};

// ---------------------------------------------------------------------------
// Layer 1 — the specification sketch, verbatim.
//
//   SnapshotBegin 1 / Bid 100 q10 seq 2 / Ask 101 q20 seq 3 / SnapshotEnd 4
//   Bid 100 q15 seq 5 / Ask 101 q0 seq 6
//   seq 9  <- gap
//   seq 10 <- rejected while GAP
//   SnapshotBegin 20 / ... / SnapshotEnd -> LIVE again
//
// The expectations below are read off the sketch, not off the implementation.
// ---------------------------------------------------------------------------
void suite_user_sketch() {
    const llmd::gen::MdTrace t = llmd::gen::build_user_sketch();
    CHECK(t.messages.size() == 13);

    const Run r = run<MapOrderBook>(t.messages);

    const MdOutcome want[] = {
        MdOutcome::Staged,             // 1  SnapshotBegin
        MdOutcome::Staged,             // 2  Bid 100 q10
        MdOutcome::Staged,             // 3  Ask 101 q20
        MdOutcome::SnapshotCommitted,  // 4  SnapshotEnd  -> LIVE
        MdOutcome::Applied,            // 5  Bid 100 q15
        MdOutcome::Applied,            // 6  Ask 101 q0   (delete)
        MdOutcome::GapDetected,        // 9  <- gap: 7 and 8 never arrived
        MdOutcome::Rejected,           // 10 <- while GAP
        MdOutcome::Staged,             // 20 SnapshotBegin (repair)
        MdOutcome::Staged,             // 21 Bid 100 q15
        MdOutcome::Staged,             // 22 Ask 101 q25
        MdOutcome::SnapshotCommitted,  // 23 SnapshotEnd  -> LIVE again
        MdOutcome::Applied,            // 24 End + 1
    };
    CHECK(r.outcomes.size() == sizeof(want) / sizeof(want[0]));
    for (std::size_t i = 0; i < r.outcomes.size() && i < 13; ++i) {
        if (r.outcomes[i] != want[i]) {
            std::printf("FAIL sketch[%zu]: got %s want %s\n", i, out_name(r.outcomes[i]),
                        out_name(want[i]));
            ++g_failures;
        }
        ++g_checks;
    }

    // The gap is not papered over: the offending sequence is never consumed, so
    // `expected` is still 7 after both the gap and the rejected message.
    CHECK(r.expected[6] == 7);
    CHECK(r.expected[7] == 7);
    CHECK(r.states[6] == MdState::Gap);
    CHECK(r.states[7] == MdState::Gap);

    // The book survived the outage rather than being wiped: a gap makes the
    // view unusable, it does not destroy it. At the gap the book still holds
    // the bid at 100, and the ask delete at seq 6 had already emptied the ask
    // side — `best_ask` of 0 is an empty side, not a price of zero.
    CHECK(r.states[7] == MdState::Gap);
    CHECK(r.best_ask == 101); // then the repair bracket restores the ask

    CHECK(r.final_state == MdState::Live);
    CHECK(r.final_expected == 25);          // End(23) + 1, then 24 applied
    CHECK(r.final_cursor == 24);
    CHECK(r.synced);
    CHECK(r.best_bid == 100);               // max(100 from snapshot, 99 at seq 24)
    CHECK(r.best_ask == 101);
    CHECK(r.level_count == 3);              // bid 100, bid 99, ask 101

    CHECK(r.counters.messages == 13);
    CHECK(r.counters.snapshot_committed == 2);
    CHECK(r.counters.gap_detected == 1);
    CHECK(r.counters.rejected == 1);

    summary("spec sketch (verbatim)");
}

// ---------------------------------------------------------------------------
// Layer 2 — scenario vectors.
// ---------------------------------------------------------------------------
struct Case {
    const char*        label;
    std::vector<MdMessage> msgs;
    std::vector<MdOutcome> outcomes;
    Expect             want;
};

void suite_scenarios() {
    const std::vector<Case> cases = {
        // -- Framing ------------------------------------------------------
        {"End with no bracket open -> ProtocolViolation, stays NotSynced",
         {E(5)},
         {MdOutcome::ProtocolViolation},
         {MdState::NotSynced, 1, 0, 0, 0}},

        // The stray End is refused and the stream continues from where it was:
        // seq 5 is still the next sequence, so it applies and the view survives.
        // Losing a synced book over one misplaced frame would be self-inflicted.
        {"End while Live -> ProtocolViolation, healthy book NOT invalidated",
         {B(1), L(2, Side::Bid, 100, 10), E(3), L(4, Side::Bid, 100, 11), E(5),
          L(5, Side::Bid, 100, 12)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotCommitted,
          MdOutcome::Applied, MdOutcome::ProtocolViolation, MdOutcome::Applied},
         {MdState::Live, 6, 100, 0, 1}},

        {"empty snapshot: Begin immediately followed by End",
         {B(1), E(2), L(3, Side::Bid, 100, 10)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied},
         {MdState::Live, 4, 100, 0, 1}},

        {"snapshot that never ends -> final state Snapshot, not Live",
         {B(1), L(2, Side::Bid, 100, 10), L(3, Side::Ask, 101, 20)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged},
         {MdState::Snapshot, 4, 0, 0, 0}},

        // A bracket that closes on its own opening sequence never advanced, so
        // the End is behind the bracket's cursor and the run stays OPEN. The
        // levels that follow it are still staged, not rejected.
        {"zero-width bracket: End carrying the Begin's own seq is Stale",
         {B(1), E(1), L(2, Side::Bid, 100, 10)},
         {MdOutcome::Staged, MdOutcome::Stale, MdOutcome::Staged},
         {MdState::Snapshot, 3, 0, 0, 0}},

        {"nested Begin restarts the bracket; first run is discarded",
         {B(1), L(2, Side::Bid, 100, 10), B(5), L(6, Side::Bid, 200, 7), E(7)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted},
         {MdState::Live, 8, 200, 0, 1}},

        // -- Ordering -----------------------------------------------------
        {"Level while NotSynced -> Rejected, no view to apply it to",
         {L(1, Side::Bid, 100, 10)},
         {MdOutcome::Rejected},
         {MdState::NotSynced, 1, 0, 0, 0}},

        {"cold start that never syncs: NotSynced, no outage episode opened",
         {L(1, Side::Bid, 100, 10), L(2, Side::Ask, 101, 5)},
         {MdOutcome::Rejected, MdOutcome::Rejected},
         {MdState::NotSynced, 1, 0, 0, 0}},

        {"duplicate level while Live -> Stale, book unchanged",
         {B(1), L(2, Side::Bid, 100, 10), E(3), L(4, Side::Bid, 100, 11),
          L(4, Side::Bid, 100, 11), L(5, Side::Bid, 100, 12)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotCommitted,
          MdOutcome::Applied, MdOutcome::Stale, MdOutcome::Applied},
         {MdState::Live, 6, 100, 0, 1}},

        {"replayed old sequence while Live -> Stale",
         {B(1), E(2), L(3, Side::Bid, 100, 10), L(9, Side::Bid, 100, 11),
          L(3, Side::Bid, 100, 99)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied,
          MdOutcome::GapDetected, MdOutcome::Rejected},
         {MdState::Gap, 4, 100, 0, 1}},

        {"duplicate inside a bracket -> Stale, the staging SURVIVES it",
         {B(1), L(2, Side::Bid, 100, 10), L(2, Side::Bid, 100, 10),
          L(3, Side::Ask, 101, 20), E(4)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Stale, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted},
         {MdState::Live, 5, 100, 101, 2}},

        {"jump inside a bracket -> whole run abandoned, view lost",
         {B(1), L(2, Side::Bid, 100, 10), L(7, Side::Bid, 100, 10)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotAbandoned},
         {MdState::NotSynced, 1, 0, 0, 0}},

        // -- Content ------------------------------------------------------
        {"delete of an absent level is idempotent",
         {B(1), L(2, Side::Bid, 100, 10), E(3), L(4, Side::Ask, 777, 0),
          L(5, Side::Bid, 100, 10)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotCommitted,
          MdOutcome::Applied, MdOutcome::Applied},
         {MdState::Live, 6, 100, 0, 1}},

        {"qty == 0 inside snapshot content means 'no level at this price'",
         {B(1), L(2, Side::Bid, 100, 10), L(3, Side::Bid, 101, 0),
          L(4, Side::Ask, 102, 7), E(5)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted},
         {MdState::Live, 6, 100, 102, 2}},

        {"negative qty on a LIVE level -> Malformed, view lost",
         {B(1), E(2), L(3, Side::Bid, 100, -1),
          B(8), L(9, Side::Bid, 100, 5), E(10)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Malformed,
          MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotCommitted},
         {MdState::Live, 11, 100, 0, 1}},

        {"out-of-domain price on a LIVE level -> OutOfRange, seq consumed, stays Live",
         {B(1), E(2), L(3, Side::Bid, 999999999, 5),
          L(4, Side::Bid, 100, 7)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::OutOfRange,
          MdOutcome::Applied},
         {MdState::Live, 5, 100, 0, 1}},

        {"negative qty INSIDE a snapshot -> whole run refused",
         {B(1), L(2, Side::Bid, 100, -1), E(3)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotAbandoned},
         {MdState::NotSynced, 1, 0, 0, 0}},

        {"out-of-domain price INSIDE a snapshot -> whole run refused",
         {B(1), L(2, Side::Bid, 999999999, 10), E(3)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotAbandoned},
         {MdState::NotSynced, 1, 0, 0, 0}},

        {"duplicate price inside a snapshot -> whole run refused",
         {B(1), L(2, Side::Bid, 100, 10), L(3, Side::Bid, 100, 20), E(4)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotAbandoned},
         {MdState::NotSynced, 1, 0, 0, 0}},

        {"same price on BOTH sides inside a snapshot is NOT a duplicate",
         {B(1), L(2, Side::Bid, 100, 10), L(3, Side::Ask, 100, 20), E(4)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted},
         {MdState::Live, 5, 100, 100, 2}},

        {"a price listed twice with qty 0 still counts as listed twice",
         {B(1), L(2, Side::Bid, 100, 10), L(3, Side::Bid, 100, 0), E(4)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotAbandoned},
         {MdState::NotSynced, 1, 0, 0, 0}},

        // -- Freshness ----------------------------------------------------
        {"stale SnapshotBegin while Live -> Stale, no rewind",
         {B(1), L(2, Side::Bid, 100, 10), E(3), L(4, Side::Bid, 100, 20),
          B(1), E(2)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::SnapshotCommitted,
          MdOutcome::Applied, MdOutcome::Stale, MdOutcome::ProtocolViolation},
         {MdState::Live, 5, 100, 0, 1}},

        // A bracket from BEFORE the view we lost is not a repair: committing it
        // would restore an old book and then replay old messages over it. The
        // gate is against the WATERMARK, and a gap does NOT advance the book's
        // cursor — so `cursor + 1` is the first sequence a repair may begin at,
        // and one numbered at the lost cursor itself is refused.
        {"repair bracket below the lost cursor -> Stale, still in Gap",
         {B(1), E(2), L(3, Side::Bid, 100, 10), L(50, Side::Bid, 100, 11),
          B(2)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied,
          MdOutcome::GapDetected, MdOutcome::Stale},
         {MdState::Gap, 4, 100, 0, 1}},

        // The off-by-one that matters. `B(3)` is numbered at the last sequence
        // the book CONSUMED, which is one behind the watermark of 4. Gating on
        // `last_applied_seq()` instead of `expected()` would accept it.
        {"repair bracket AT the lost cursor -> Stale, still in Gap",
         {B(1), E(2), L(3, Side::Bid, 100, 10), L(50, Side::Bid, 100, 11),
          B(3)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied,
          MdOutcome::GapDetected, MdOutcome::Stale},
         {MdState::Gap, 4, 100, 0, 1}},

        {"repair bracket at the watermark is accepted and recovers",
         {B(1), E(2), L(3, Side::Bid, 100, 10), L(50, Side::Bid, 100, 11),
          B(4), L(5, Side::Bid, 100, 7), E(6), L(7, Side::Ask, 60, 2)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied,
          MdOutcome::GapDetected, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted, MdOutcome::Applied},
         {MdState::Live, 8, 100, 60, 2}},

        // The same off-by-one while Live, where `expected() == cursor + 1`.
        // `B(3)` is refused and the healthy book is UNTOUCHED — not even a
        // ProtocolViolation, just a stale frame the live view steps over. The
        // `B(4)` that follows is the same frame one sequence later, and it is
        // accepted: the pair pins the boundary exactly.
        {"SnapshotBegin at the last applied seq while Live -> Stale, no rewind",
         {B(1), E(2), L(3, Side::Bid, 100, 10), B(3),
          B(4), L(5, Side::Bid, 100, 20), E(6)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied,
          MdOutcome::Stale, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted},
         {MdState::Live, 7, 100, 0, 1}},

        // While a bracket is open the watermark is the BRACKET's cursor, so a
        // nested Begin behind the run in progress is stale — and the run
        // survives it, exactly as it survives a duplicate level.
        {"nested Begin behind the bracket cursor -> Stale, run survives",
         {B(1), L(2, Side::Bid, 100, 10), L(3, Side::Bid, 101, 5),
          B(2), L(4, Side::Bid, 102, 7), E(5)},
         {MdOutcome::Staged, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::Stale, MdOutcome::Staged, MdOutcome::SnapshotCommitted},
         {MdState::Live, 6, 102, 0, 3}},

        {"a snapshot whose End resumes a long-lost stream",
         {B(1), E(2), L(3, Side::Bid, 100, 10), L(500, Side::Bid, 100, 11),
          B(900), L(901, Side::Bid, 55, 5), E(902), L(903, Side::Ask, 60, 6)},
         {MdOutcome::Staged, MdOutcome::SnapshotCommitted, MdOutcome::Applied,
          MdOutcome::GapDetected, MdOutcome::Staged, MdOutcome::Staged,
          MdOutcome::SnapshotCommitted, MdOutcome::Applied},
         {MdState::Live, 904, 55, 60, 2}},
    };

    for (const Case& c : cases) {
        const Run r = run<MapOrderBook>(c.msgs);
        if (r.outcomes != c.outcomes) {
            std::printf("FAIL scenario '%s'\n", c.label);
            std::printf("       outcomes:");
            for (MdOutcome o : r.outcomes) std::printf(" %s", out_name(o));
            std::printf("\n       want:    ");
            for (MdOutcome o : c.outcomes) std::printf(" %s", out_name(o));
            std::printf("\n");
            ++g_failures;
        }
        ++g_checks;

        if (r.final_state != c.want.state) {
            std::printf("FAIL scenario '%s': state %s want %s\n", c.label,
                        llmd::md_state_name(r.final_state),
                        llmd::md_state_name(c.want.state));
            ++g_failures;
        }
        ++g_checks;

        if (r.final_expected != c.want.expected) {
            std::printf("FAIL scenario '%s': expected %llu want %llu\n", c.label,
                        static_cast<unsigned long long>(r.final_expected),
                        static_cast<unsigned long long>(c.want.expected));
            ++g_failures;
        }
        ++g_checks;

        if (r.best_bid != c.want.best_bid || r.best_ask != c.want.best_ask ||
            r.level_count != c.want.level_count) {
            std::printf("FAIL scenario '%s': book bid=%lld ask=%lld n=%zu want bid=%lld ask=%lld n=%zu\n",
                        c.label, static_cast<long long>(r.best_bid),
                        static_cast<long long>(r.best_ask), r.level_count,
                        static_cast<long long>(c.want.best_bid),
                        static_cast<long long>(c.want.best_ask), c.want.level_count);
            ++g_failures;
        }
        ++g_checks;
    }

    summary("scenario vectors");
}

// ---------------------------------------------------------------------------
// Layer 3 — accounting.
// ---------------------------------------------------------------------------

// The identity: every message has exactly one outcome, so the ten outcome
// counters must partition the stream. This is what makes the outcome vector a
// complete account of a trace rather than a sample of it.
void check_identity(const MdCounters& c, const char* where, std::size_t n) {
    const std::uint64_t sum = c.applied + c.out_of_range + c.staged +
                              c.snapshot_committed + c.snapshot_abandoned + c.stale +
                              c.gap_detected + c.malformed + c.rejected +
                              c.protocol_violations;
    if (sum != c.messages) {
        std::printf("FAIL accounting identity at %s: %llu outcomes for %llu messages\n",
                    where, static_cast<unsigned long long>(sum),
                    static_cast<unsigned long long>(c.messages));
        ++g_failures;
    }
    ++g_checks;
    if (c.messages != n) {
        std::printf("FAIL message count at %s: %llu vs %zu\n", where,
                    static_cast<unsigned long long>(c.messages), n);
        ++g_failures;
    }
    ++g_checks;
}

void suite_accounting() {
    // --- multi-attempt outage: one episode, two brackets, correct accounting.
    {
        std::vector<MdMessage> msgs = {
            B(1), E(2), L(3, Side::Bid, 100, 10),   // Live at 3
            L(50, Side::Bid, 100, 11),              // gap -> episode opens (missing 4)
            L(51, Side::Bid, 100, 12),              // rejected, discarded
            B(100), L(101, Side::Bid, 100, 5),      // attempt 1
            L(700, Side::Bid, 100, 5),              // jumps: attempt 1 abandoned
            B(900), L(901, Side::Bid, 100, 6), E(902), // attempt 2 commits
            L(903, Side::Ask, 101, 9),
        };
        MarketDataPipeline<MapOrderBook> p;
        for (const MdMessage& m : msgs) p.apply(m);

        check_identity(p.counters(), "multi-attempt outage", msgs.size());

        CHECK(p.episodes().size() == 1);
        if (p.episodes().size() == 1) {
            const MdRecoveryEpisode& e = p.episodes()[0];
            CHECK(e.cause == LossCause::SeqJumpLive);
            CHECK(e.first_missing_seq == 4);   // the book's own position
            CHECK(e.gap_detect_seq == 50);
            CHECK(e.lost_span == 46);          // 50 - 4
            CHECK(e.max_seq_seen == 902);      // highest seen while open
            CHECK(e.recovered);
            CHECK(e.recovery_end_seq == 902);
            CHECK(e.attempts == 2);            // two brackets accepted while open
            CHECK(e.recovery_begin_seq == 900); // the most recent attempt
            // One level Rejected while out of sync, plus the one level that
            // attempt 1 had staged before it was thrown away. Both halves of
            // the documented definition of `discarded`.
            CHECK(e.discarded == 1 + 1);
        }
        CHECK(p.counters().outages == 1);
        CHECK(p.counters().recovered_outages == 1);
        CHECK(p.open_episode() == nullptr);
        CHECK(p.counters().gap_detected == 1);
        CHECK(p.counters().rejected == 1);
    }

    // --- a failed mid-stream refresh: first_missing comes from the BOOK.
    {
        std::vector<MdMessage> msgs = {
            B(1), E(2), L(3, Side::Bid, 100, 10),   // Live, cursor 3
            B(4), L(5, Side::Bid, 100, 20),         // refresh bracket
            L(9, Side::Bid, 100, 30),               // jumps -> refresh abandoned
            B(20), L(21, Side::Bid, 100, 40), E(22),
        };
        MarketDataPipeline<MapOrderBook> p;
        for (const MdMessage& m : msgs) p.apply(m);
        CHECK(p.episodes().size() == 1);
        if (p.episodes().size() == 1) {
            const MdRecoveryEpisode& e = p.episodes()[0];
            CHECK(e.cause == LossCause::SeqJumpInSnapshot);
            // The book was last good at 3, NOT at the bracket's cursor of 6.
            // Taking the bracket's position would understate the outage by the
            // three sequences the bracket had already staged.
            CHECK(e.first_missing_seq == 4);
            CHECK(e.gap_detect_seq == 9);
            CHECK(e.lost_span == 5);
            // The bracket that opened this outage was accepted BEFORE it existed,
            // so it is not counted as an attempt.
            CHECK(e.attempts == 1);              // only the B(20) repair
            CHECK(e.recovery_begin_seq == 20);
            // The refresh had staged exactly one level (seq 5) before seq 9
            // abandoned it, and that level is charged to the outage this very
            // abandonment opened — which is why the charge happens after
            // lose_view and not before.
            CHECK(e.discarded == 1);
        }
        check_identity(p.counters(), "failed refresh", msgs.size());
    }

    // --- the staging cap is reachable and abandons the bracket.
    {
        MarketDataPipeline<MapOrderBook>::Config cfg;
        cfg.max_staged_levels_per_side = 3;
        MarketDataPipeline<MapOrderBook> p{MapOrderBook{}, cfg};
        p.apply(B(1));
        for (std::uint64_t i = 0; i < 3; ++i)
            p.apply(L(2 + i, Side::Bid, 100 + static_cast<std::int64_t>(i), 10));
        const MdResult over = p.apply(L(5, Side::Bid, 200, 10)); // 4th level, cap 3
        CHECK(over.outcome == MdOutcome::SnapshotAbandoned);
        CHECK(p.counters().staging_limit_hits == 1);
        CHECK(p.counters().snapshot_abandoned == 1);
        CHECK(p.state() == MdState::NotSynced); // never had a view to lose
        CHECK(p.counters().failed_initial_syncs == 1);
        CHECK(p.counters().outages == 0);       // cold start is not an outage
    }

    // --- a stream ending mid-outage leaves the episode OPEN and unlisted.
    {
        std::vector<MdMessage> msgs = {B(1), E(2), L(3, Side::Bid, 100, 10),
                                       L(50, Side::Bid, 100, 11)};
        MarketDataPipeline<MapOrderBook> p;
        for (const MdMessage& m : msgs) p.apply(m);
        CHECK(p.episodes().empty());            // not closed, so not in the list
        CHECK(p.open_episode() != nullptr);
        CHECK(p.open_episode()->recovered == false);
        CHECK(p.state() == MdState::Gap);
        check_identity(p.counters(), "open outage", msgs.size());
    }

    // --- a repair bracket in flight keeps the outage visible.
    // `open_episode()` is NOT "non-null iff Gap": during the repair it is
    // non-null while state() == Snapshot, which is the whole point — hiding the
    // outage for the duration of the repair would be a reporting bug.
    {
        std::vector<MdMessage> msgs = {B(1), E(2), L(3, Side::Bid, 100, 10),
                                       L(50, Side::Bid, 100, 11), B(100)};
        MarketDataPipeline<MapOrderBook> p;
        for (const MdMessage& m : msgs) p.apply(m);
        CHECK(p.state() == MdState::Snapshot);
        CHECK(p.open_episode() != nullptr);
        CHECK(p.open_episode()->attempts == 1);
    }

    summary("recovery accounting");
}

// Every outcome and every loss cause must be REACHED by the corpus. A
// differential test where both implementations agree on a permanent zero proves
// nothing about that state, so this is what keeps layer 4 honest.
void suite_corpus_coverage() {
    llmd::gen::MdGenConfig cfg;
    cfg.base_messages = 150;
    cfg.base_levels = 10;

    MdCounters total;
    auto accumulate = [&total](const MdCounters& c) {
        total.messages += c.messages;
        total.applied += c.applied;
        total.out_of_range += c.out_of_range;
        total.staged += c.staged;
        total.snapshot_committed += c.snapshot_committed;
        total.snapshot_abandoned += c.snapshot_abandoned;
        total.stale += c.stale;
        total.gap_detected += c.gap_detected;
        total.malformed += c.malformed;
        total.rejected += c.rejected;
        total.protocol_violations += c.protocol_violations;
        total.sync_attempts += c.sync_attempts;
        total.stale_snapshot_begins += c.stale_snapshot_begins;
        total.malformed_snapshots += c.malformed_snapshots;
        total.empty_snapshots_committed += c.empty_snapshots_committed;
        total.failed_initial_syncs += c.failed_initial_syncs;
        total.staging_limit_hits += c.staging_limit_hits;
        total.nested_brackets_discarded += c.nested_brackets_discarded;
        total.outages += c.outages;
        total.recovered_outages += c.recovered_outages;
    };

    bool saw_cause[4] = {false, false, false, false};
    bool saw_state[4] = {false, false, false, false};

    auto drive = [&](const std::vector<MdMessage>& msgs, std::size_t cap) {
        MarketDataPipeline<MapOrderBook>::Config pc;
        pc.max_staged_levels_per_side = cap;
        MarketDataPipeline<MapOrderBook> p{MapOrderBook{}, pc};
        for (const MdMessage& m : msgs) {
            p.apply(m);
            saw_state[static_cast<int>(p.state())] = true;
            if (p.snapshot_in_progress()) saw_state[static_cast<int>(MdState::Snapshot)] = true;
        }
        accumulate(p.counters());
        for (const MdRecoveryEpisode& e : p.episodes())
            saw_cause[static_cast<int>(e.cause)] = true;
        if (p.open_episode()) saw_cause[static_cast<int>(p.open_episode()->cause)] = true;
    };

    for (int s = 0; s < llmd::gen::kScenarioCount; ++s) {
        for (int m = 0; m < llmd::gen::kMutationCount; ++m) {
            const llmd::gen::MdTrace t = llmd::gen::build_with_recipe(
                static_cast<llmd::gen::MdScenario>(s),
                {llmd::gen::mutation_at(m)}, cfg, 99);
            drive(t.messages, std::size_t{1} << 20);
        }
    }
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        llmd::gen::MdGenConfig fc = cfg;
        fc.mutations = static_cast<std::uint32_t>(seed % 5);
        const llmd::gen::MdTrace t = llmd::gen::build(
            static_cast<llmd::gen::MdScenario>(seed % llmd::gen::kScenarioCount), fc, seed);
        drive(t.messages, std::size_t{1} << 20);
    }
    for (std::size_t cap : {std::size_t{1}, std::size_t{3}, std::size_t{6}}) {
        for (std::uint64_t seed = 1; seed <= 100; ++seed) {
            llmd::gen::MdGenConfig fc = cfg;
            fc.mutations = 3;
            const llmd::gen::MdTrace t = llmd::gen::build(
                static_cast<llmd::gen::MdScenario>(seed % llmd::gen::kScenarioCount), fc, seed);
            drive(t.messages, cap);
        }
    }

    struct Named { const char* name; std::uint64_t v; };
    const Named reached[] = {
        {"applied", total.applied},
        {"out_of_range", total.out_of_range},
        {"staged", total.staged},
        {"snapshot_committed", total.snapshot_committed},
        {"snapshot_abandoned", total.snapshot_abandoned},
        {"stale", total.stale},
        {"gap_detected", total.gap_detected},
        {"malformed", total.malformed},
        {"rejected", total.rejected},
        {"protocol_violations", total.protocol_violations},
        {"sync_attempts", total.sync_attempts},
        {"stale_snapshot_begins", total.stale_snapshot_begins},
        {"malformed_snapshots", total.malformed_snapshots},
        {"empty_snapshots_committed", total.empty_snapshots_committed},
        {"failed_initial_syncs", total.failed_initial_syncs},
        {"staging_limit_hits", total.staging_limit_hits},
        {"nested_brackets_discarded", total.nested_brackets_discarded},
        {"outages", total.outages},
        {"recovered_outages", total.recovered_outages},
    };
    for (const Named& n : reached) {
        if (n.v == 0) {
            std::printf("FAIL corpus never reaches counter '%s'\n", n.name);
            ++g_failures;
        }
        ++g_checks;
    }
    for (int i = 0; i < 4; ++i) {
        if (!saw_cause[i]) {
            std::printf("FAIL corpus never reaches loss cause '%s'\n",
                        llmd::loss_cause_name(static_cast<LossCause>(i)));
            ++g_failures;
        }
        ++g_checks;
        if (!saw_state[i]) {
            std::printf("FAIL corpus never reaches state '%s'\n",
                        llmd::md_state_name(static_cast<MdState>(i)));
            ++g_failures;
        }
        ++g_checks;
    }
    check_identity(total, "corpus", total.messages);

    summary("corpus coverage");
}

// ---------------------------------------------------------------------------
// Layer 4 — differential fuzz against the independent oracle.
// ---------------------------------------------------------------------------
template <class Book>
void diff_one(const llmd::gen::MdTrace& t, std::size_t cap) {
    typename MarketDataPipeline<Book>::Config pc;
    pc.max_staged_levels_per_side = cap;
    MarketDataPipeline<Book> p{Book{}, pc};

    llmd::oracle::OracleConfig oc;
    oc.max_staged_levels_per_side = cap;
    llmd::oracle::Oracle o(oc);

    // Tally the outcomes actually returned, so the counters can be checked
    // against them. Comparing counters alone would pass even if every message
    // incremented the wrong counter — the identity check would still balance.
    std::uint64_t tally[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    for (std::size_t i = 0; i < t.messages.size(); ++i) {
        const MdResult a = p.apply(t.messages[i]);
        const llmd::oracle::OracleResult b = o.apply(t.messages[i]);
        ++tally[static_cast<int>(a.outcome)];
        if (a.outcome != b.outcome) {
            report("oracle outcome", t, i,
                   std::string(out_name(a.outcome)) + " vs " + out_name(b.outcome));
            return; // one report per trace is enough to identify it
        }
        if (a.state_after != b.state_after) {
            report("oracle state_after", t, i,
                   std::string(llmd::md_state_name(a.state_after)) + " vs " +
                       llmd::md_state_name(b.state_after));
            return;
        }
        if (a.expected_after != b.expected_after) {
            report("oracle expected_after", t, i,
                   std::to_string(a.expected_after) + " vs " + std::to_string(b.expected_after));
            return;
        }
    }

    const MdCounters& x = p.counters();
    {
        struct O { const char* n; std::uint64_t counted; std::uint64_t returned; };
        const O outs[] = {
            {"applied", x.applied, tally[static_cast<int>(MdOutcome::Applied)]},
            {"out_of_range", x.out_of_range, tally[static_cast<int>(MdOutcome::OutOfRange)]},
            {"staged", x.staged, tally[static_cast<int>(MdOutcome::Staged)]},
            {"snapshot_committed", x.snapshot_committed,
             tally[static_cast<int>(MdOutcome::SnapshotCommitted)]},
            {"snapshot_abandoned", x.snapshot_abandoned,
             tally[static_cast<int>(MdOutcome::SnapshotAbandoned)]},
            {"stale", x.stale, tally[static_cast<int>(MdOutcome::Stale)]},
            {"gap_detected", x.gap_detected, tally[static_cast<int>(MdOutcome::GapDetected)]},
            {"malformed", x.malformed, tally[static_cast<int>(MdOutcome::Malformed)]},
            {"rejected", x.rejected, tally[static_cast<int>(MdOutcome::Rejected)]},
            {"protocol_violations", x.protocol_violations,
             tally[static_cast<int>(MdOutcome::ProtocolViolation)]},
        };
        for (const O& c : outs) {
            if (c.counted != c.returned) {
                report("counter vs outcome", t, 0,
                       std::string(c.n) + " counter=" + std::to_string(c.counted) +
                           " returned=" + std::to_string(c.returned));
                return;
            }
        }
    }
    const MdCounters& y = o.counters();
    struct C { const char* n; std::uint64_t a; std::uint64_t b; };
    const C counters[] = {
        {"messages", x.messages, y.messages},
        {"applied", x.applied, y.applied},
        {"out_of_range", x.out_of_range, y.out_of_range},
        {"staged", x.staged, y.staged},
        {"snapshot_committed", x.snapshot_committed, y.snapshot_committed},
        {"snapshot_abandoned", x.snapshot_abandoned, y.snapshot_abandoned},
        {"stale", x.stale, y.stale},
        {"gap_detected", x.gap_detected, y.gap_detected},
        {"malformed", x.malformed, y.malformed},
        {"rejected", x.rejected, y.rejected},
        {"protocol_violations", x.protocol_violations, y.protocol_violations},
        {"sync_attempts", x.sync_attempts, y.sync_attempts},
        {"stale_snapshot_begins", x.stale_snapshot_begins, y.stale_snapshot_begins},
        {"malformed_snapshots", x.malformed_snapshots, y.malformed_snapshots},
        {"empty_snapshots_committed", x.empty_snapshots_committed, y.empty_snapshots_committed},
        {"failed_initial_syncs", x.failed_initial_syncs, y.failed_initial_syncs},
        {"staging_limit_hits", x.staging_limit_hits, y.staging_limit_hits},
        {"nested_brackets_discarded", x.nested_brackets_discarded,
         y.nested_brackets_discarded},
        {"outages", x.outages, y.outages},
        {"recovered_outages", x.recovered_outages, y.recovered_outages},
    };
    for (const C& c : counters) {
        if (c.a != c.b) {
            report("oracle counter", t, 0,
                   std::string(c.n) + " " + std::to_string(c.a) + " vs " + std::to_string(c.b));
            return;
        }
    }

    if (p.state() != o.state() || p.expected() != o.expected() || p.cursor() != o.cursor()) {
        report("oracle final position", t, 0, "state/expected/cursor");
        return;
    }
    if (p.book().synced() != o.book().synced || p.book().best_bid() != o.book().best_bid() ||
        p.book().best_ask() != o.book().best_ask() ||
        p.book().level_count() != o.book().level_count() ||
        p.staged_level_count() != o.staged_level_count()) {
        report("oracle final book", t, 0, "synced/best/count");
        return;
    }

    // Full level-set parity, not just the best price: a book can agree on its
    // top of book and disagree everywhere else.
    {
        const auto want = o.book().bids_desc();
        std::size_t k = 0;
        bool ok = true;
        for (const auto& lv : p.book().bids()) {
            if (k >= want.size() || want[k].first != lv.first || want[k].second != lv.second) {
                ok = false;
                break;
            }
            ++k;
        }
        if (!ok || k != want.size()) {
            report("oracle bid levels", t, 0, "level set differs");
            return;
        }
    }
    {
        const auto want = o.book().asks_asc();
        std::size_t k = 0;
        bool ok = true;
        for (const auto& lv : p.book().asks()) {
            if (k >= want.size() || want[k].first != lv.first || want[k].second != lv.second) {
                ok = false;
                break;
            }
            ++k;
        }
        if (!ok || k != want.size()) {
            report("oracle ask levels", t, 0, "level set differs");
            return;
        }
    }

    if (p.episodes().size() != o.episodes().size()) {
        report("oracle episode count", t, 0, "episode list length differs");
        return;
    }
    for (std::size_t i = 0; i < p.episodes().size(); ++i) {
        const MdRecoveryEpisode& e = p.episodes()[i];
        const MdRecoveryEpisode& f = o.episodes()[i];
        if (e.cause != f.cause || e.first_missing_seq != f.first_missing_seq ||
            e.gap_detect_seq != f.gap_detect_seq || e.lost_span != f.lost_span ||
            e.max_seq_seen != f.max_seq_seen || e.discarded != f.discarded ||
            e.attempts != f.attempts || e.recovery_begin_seq != f.recovery_begin_seq ||
            e.recovery_end_seq != f.recovery_end_seq || e.recovered != f.recovered) {
            report("oracle episode", t, 0, "episode " + std::to_string(i) + " differs");
            return;
        }
    }
    const MdRecoveryEpisode* oa = p.open_episode();
    const MdRecoveryEpisode* ob = o.open_episode();
    if ((oa == nullptr) != (ob == nullptr)) {
        report("oracle open episode", t, 0, "presence differs");
        return;
    }
    if (oa != nullptr &&
        (oa->cause != ob->cause || oa->first_missing_seq != ob->first_missing_seq ||
         oa->gap_detect_seq != ob->gap_detect_seq || oa->lost_span != ob->lost_span ||
         oa->max_seq_seen != ob->max_seq_seen || oa->discarded != ob->discarded ||
         oa->attempts != ob->attempts ||
         oa->recovery_begin_seq != ob->recovery_begin_seq)) {
        report("oracle open episode", t, 0, "fields differ");
        return;
    }
    ++g_checks; // this trace agreed in full
}

void suite_differential() {
    llmd::gen::MdGenConfig cfg;
    cfg.base_messages = 120;
    cfg.base_levels = 8;

    std::size_t traces = 0;

    // Every scenario with every single mutation, on its own.
    for (int s = 0; s < llmd::gen::kScenarioCount; ++s) {
        for (int m = 0; m < llmd::gen::kMutationCount; ++m) {
            const llmd::gen::MdTrace t = llmd::gen::build_with_recipe(
                static_cast<llmd::gen::MdScenario>(s),
                {llmd::gen::mutation_at(m)}, cfg, 99);
            diff_one<MapOrderBook>(t, std::size_t{1} << 20);
            ++traces;
        }
    }

    // Seeded composition.
    for (std::uint64_t seed = 1; seed <= 250; ++seed) {
        for (std::uint32_t nm = 0; nm <= 4; ++nm) {
            llmd::gen::MdGenConfig fc = cfg;
            fc.mutations = nm;
            const llmd::gen::MdTrace t = llmd::gen::build(
                static_cast<llmd::gen::MdScenario>(seed % llmd::gen::kScenarioCount), fc, seed);
            diff_one<MapOrderBook>(t, std::size_t{1} << 20);
            ++traces;
        }
    }

    // Small staging caps, the only way the cap and the abandon-on-cap path are
    // reachable at all. Without this pass, a wrong cap comparison is invisible.
    for (std::size_t cap : {std::size_t{1}, std::size_t{3}, std::size_t{6}, std::size_t{20}}) {
        for (std::uint64_t seed = 1; seed <= 60; ++seed) {
            llmd::gen::MdGenConfig fc = cfg;
            fc.mutations = 3;
            const llmd::gen::MdTrace t = llmd::gen::build(
                static_cast<llmd::gen::MdScenario>(seed % llmd::gen::kScenarioCount), fc, seed);
            diff_one<MapOrderBook>(t, cap);
            ++traces;
        }
    }

    std::printf("       %zu traces compared against the oracle\n", traces);
    summary("differential vs oracle");
}

// ---------------------------------------------------------------------------
// Layer 5 — sink agreement: Map vs Flat over the same corrupted stream.
// ---------------------------------------------------------------------------
void suite_sink_agreement() {
    llmd::gen::MdGenConfig cfg;
    cfg.base_messages = 120;
    cfg.base_levels = 8;

    std::size_t traces = 0;
    auto compare = [&](const llmd::gen::MdTrace& t, std::size_t cap) {
        ++traces;
        typename MarketDataPipeline<MapOrderBook>::Config mc;
        mc.max_staged_levels_per_side = cap;
        MarketDataPipeline<MapOrderBook> a{MapOrderBook{}, mc};

        typename MarketDataPipeline<FlatOrderBook>::Config fc;
        fc.max_staged_levels_per_side = cap;
        MarketDataPipeline<FlatOrderBook> b{FlatOrderBook{}, fc};

        for (std::size_t i = 0; i < t.messages.size(); ++i) {
            const MdResult ra = a.apply(t.messages[i]);
            const MdResult rb = b.apply(t.messages[i]);
            if (ra.outcome != rb.outcome || ra.state_after != rb.state_after ||
                ra.expected_after != rb.expected_after) {
                report("map/flat pipeline", t, i,
                       std::string(out_name(ra.outcome)) + " vs " + out_name(rb.outcome));
                return;
            }
        }

        struct C { const char* n; std::uint64_t a; std::uint64_t b; };
        const MdCounters& x = a.counters();
        const MdCounters& y = b.counters();
        const C counters[] = {
            {"messages", x.messages, y.messages},
            {"applied", x.applied, y.applied},
            {"out_of_range", x.out_of_range, y.out_of_range},
            {"staged", x.staged, y.staged},
            {"snapshot_committed", x.snapshot_committed, y.snapshot_committed},
            {"snapshot_abandoned", x.snapshot_abandoned, y.snapshot_abandoned},
            {"stale", x.stale, y.stale},
            {"gap_detected", x.gap_detected, y.gap_detected},
            {"malformed", x.malformed, y.malformed},
            {"rejected", x.rejected, y.rejected},
            {"protocol_violations", x.protocol_violations, y.protocol_violations},
        };
        for (const C& c : counters) {
            if (c.a != c.b) {
                report("map/flat counter", t, 0,
                       std::string(c.n) + " " + std::to_string(c.a) + " vs " + std::to_string(c.b));
                return;
            }
        }

        if (a.state() != b.state() || a.expected() != b.expected() ||
            a.cursor() != b.cursor()) {
            report("map/flat position", t, 0, "state/expected/cursor");
            return;
        }
        if (a.book().synced() != b.book().synced() ||
            a.book().best_bid() != b.book().best_bid() ||
            a.book().best_ask() != b.book().best_ask() ||
            a.book().best_bid_qty() != b.book().best_bid_qty() ||
            a.book().best_ask_qty() != b.book().best_ask_qty() ||
            a.book().level_count() != b.book().level_count()) {
            report("map/flat book", t, 0, "synced/best/count");
            return;
        }
        // Level-set parity by enumerating the map book's keys — the only one of
        // the two that can enumerate. Agreement on the top of book is not
        // agreement on the book.
        for (const auto& lv : a.book().bids()) {
            if (b.book().level_qty(lv.first, Side::Bid) != lv.second) {
                report("map/flat bid level", t, 0,
                       "price " + std::to_string(lv.first) + " differs");
                return;
            }
        }
        for (const auto& lv : a.book().asks()) {
            if (b.book().level_qty(lv.first, Side::Ask) != lv.second) {
                report("map/flat ask level", t, 0,
                       "price " + std::to_string(lv.first) + " differs");
                return;
            }
        }
        ++g_checks; // this trace agreed in full
    };

    for (int s = 0; s < llmd::gen::kScenarioCount; ++s) {
        for (int m = 0; m < llmd::gen::kMutationCount; ++m) {
            compare(llmd::gen::build_with_recipe(static_cast<llmd::gen::MdScenario>(s),
                                                 {llmd::gen::mutation_at(m)}, cfg, 99),
                    std::size_t{1} << 20);
        }
    }
    for (std::uint64_t seed = 1; seed <= 120; ++seed) {
        llmd::gen::MdGenConfig fc = cfg;
        fc.mutations = static_cast<std::uint32_t>(seed % 5);
        compare(llmd::gen::build(
                    static_cast<llmd::gen::MdScenario>(seed % llmd::gen::kScenarioCount), fc, seed),
                std::size_t{1} << 20);
    }
    for (std::size_t cap : {std::size_t{1}, std::size_t{4}, std::size_t{12}}) {
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            llmd::gen::MdGenConfig fc = cfg;
            fc.mutations = 3;
            compare(llmd::gen::build(
                        static_cast<llmd::gen::MdScenario>(seed % llmd::gen::kScenarioCount), fc, seed),
                    cap);
        }
    }

    std::printf("       %zu traces compared across sinks\n", traces);
    summary("map vs flat sink agreement");
}

// ---------------------------------------------------------------------------
// Layer 6 — the generator itself. A failure in the layers above is only
// reproducible if the inputs are; that is what these assert.
// ---------------------------------------------------------------------------
void suite_generator() {
    llmd::gen::MdGenConfig cfg;
    cfg.base_messages = 100;
    cfg.base_levels = 6;

    // Same inputs, same bytes.
    for (int s = 0; s < llmd::gen::kScenarioCount; ++s) {
        const auto sc = static_cast<llmd::gen::MdScenario>(s);
        const llmd::gen::MdTrace a = llmd::gen::build(sc, cfg, 4242);
        const llmd::gen::MdTrace b = llmd::gen::build(sc, cfg, 4242);
        CHECK(a.fingerprint() == b.fingerprint());
        CHECK(a.messages.size() == b.messages.size());
        CHECK(a.messages.size() > 0);
    }

    // Different seeds must actually explore differently, or the fuzz corpus is
    // one trace repeated.
    {
        std::vector<std::uint64_t> fps;
        for (std::uint64_t seed = 1; seed <= 40; ++seed) {
            llmd::gen::MdGenConfig fc = cfg;
            fc.mutations = 3;
            fps.push_back(
                llmd::gen::build(llmd::gen::MdScenario::CleanSteady, fc, seed).fingerprint());
        }
        std::size_t distinct = 0;
        for (std::size_t i = 0; i < fps.size(); ++i) {
            bool first = true;
            for (std::size_t j = 0; j < i; ++j)
                if (fps[i] == fps[j]) first = false;
            if (first) ++distinct;
        }
        CHECK(distinct >= fps.size() - 2); // allow a rare collision
    }

    // build_with_recipe is the freeze path: the same recipe must reproduce the
    // same trace, because that is how a fuzz failure becomes a regression.
    {
        using llmd::gen::MdMutation;
        const std::vector<MdMutation> recipe = {MdMutation::DropSlice, MdMutation::DuplicateOne,
                                                MdMutation::InjectSnapshot};
        const llmd::gen::MdTrace a = llmd::gen::build_with_recipe(
            llmd::gen::MdScenario::SnapshotHeavy, {recipe[0], recipe[1], recipe[2]}, cfg, 77);
        const llmd::gen::MdTrace b = llmd::gen::build_with_recipe(
            llmd::gen::MdScenario::SnapshotHeavy, {recipe[0], recipe[1], recipe[2]}, cfg, 77);
        CHECK(a.fingerprint() == b.fingerprint());
        CHECK(a.recipe.size() == 3);
    }

    // The sketch is stable and is what the specification says it is.
    {
        const llmd::gen::MdTrace t = llmd::gen::build_user_sketch();
        CHECK(t.messages.size() == 13);
        CHECK(t.messages[0].kind == MdKind::SnapshotBegin && t.messages[0].seq == 1);
        CHECK(t.messages[6].kind == MdKind::Level && t.messages[6].seq == 9);
        CHECK(t.messages[7].kind == MdKind::Level && t.messages[7].seq == 10);
        CHECK(t.messages[8].kind == MdKind::SnapshotBegin && t.messages[8].seq == 20);
        CHECK(t.fingerprint() == llmd::gen::build_user_sketch().fingerprint());
        CHECK(t.dump().find("SnapshotBegin") != std::string::npos);
    }

    // Base scenarios are well-formed: strictly contiguous sequences, so any gap
    // in the fuzz corpus comes from a mutation and never from the base.
    for (int s = 1; s < llmd::gen::kScenarioCount; ++s) {
        const auto sc = static_cast<llmd::gen::MdScenario>(s);
        const llmd::gen::MdTrace t = llmd::gen::build_base(sc, cfg, 8);
        bool contiguous = true;
        for (std::size_t i = 1; i < t.messages.size(); ++i) {
            if (t.messages[i].seq <= t.messages[i - 1].seq) contiguous = false;
        }
        if (!contiguous) {
            std::printf("FAIL base scenario '%s' is not strictly increasing\n",
                        llmd::gen::scenario_name(sc));
            ++g_failures;
        }
        ++g_checks;
    }

    summary("generator determinism");
}

// ---------------------------------------------------------------------------
// The deliberate failure, reached only through the self-test env var.
// ---------------------------------------------------------------------------
void suite_selftest_failure() {
    CHECK(1 == 1);
    CHECK(1 == 2); // deliberately false: proves the exit path is non-zero
    summary("deliberate failure (self-test)");
}

} // namespace

int main() {
    if (std::getenv("LLDB_SELFTEST_FAIL") != nullptr) {
        std::printf("LLDB_SELFTEST_FAIL set: running the deliberate-failure suite only\n");
        suite_selftest_failure();
        std::printf("selftest: %d failure(s) recorded; exit status must be non-zero\n",
                    g_failures_total);
        return g_failures_total == 0 ? 0 : 1;
    }

    std::printf("Experiment 03 — market-data pipeline correctness\n\n");
    suite_user_sketch();
    suite_scenarios();
    suite_accounting();
    suite_corpus_coverage();
    suite_differential();
    suite_sink_agreement();
    suite_generator();

    if (g_failures_total == 0) {
        std::printf("\nALL SUITES PASSED\n");
    } else {
        std::printf("\n%d CHECK FAILURE(S)\n", g_failures_total);
    }
    return g_failures_total == 0 ? 0 : 1;
}
