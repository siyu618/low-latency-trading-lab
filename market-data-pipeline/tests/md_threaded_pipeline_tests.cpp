// Experiment 03 Phase 2 — threaded decoder -> SPSC -> book correctness tests.
//
// Phase 2 asks exactly one question: does the threaded composition preserve
// Phase-1 correctness? So there are no timings here, no percentiles, no
// capacity-performance matrix and no affinity. Every assertion is about VALUES:
// the messages that came out of the framer, the state the book ended in, and
// the counts that prove nothing was dropped.
//
// The structure mirrors the question. A reference result is computed
// SINGLE-THREADED through the frozen Phase-1 path — the same messages, applied
// straight to a `MarketDataPipeline<FlatOrderBook>` with no queue and no
// threads — and the threaded runs are required to match it. The threaded path
// therefore has an oracle that shares none of its machinery, rather than being
// compared against itself.
//
//   1. framing            — chunk-boundary semantics, single-threaded, against
//                           the frozen `decode_one`.
//   2. chunk plans        — the SAME encoded stream under whole / 1-byte /
//                           awkward / seeded-random chunk boundaries must
//                           decode to the SAME message sequence.
//   3. threaded reference — the threaded run must match the single-thread
//                           Phase-1 result on every observable value.
//   4. backpressure       — Capacity 2, consumer held behind a gate until the
//                           ring is provably full: nothing dropped, FIFO
//                           preserved.
//   5. termination        — a final message queued immediately before
//                           `producer_done` must still be consumed.
//   6. malformed wire     — a terminal decode error ends the session without
//                           resynchronisation, after the valid prefix is fully
//                           delivered.
//   7. finalization       — which streams ended on a frame boundary: every
//                           possible partial prefix is NeedMoreData at EOF,
//                           every exact boundary is Ok.
//   8. truncated at EOF   — a stream that stops mid-frame is REPORTED as
//                           truncated, while the valid prefix it did deliver
//                           stays identical to the single-thread reference.
//
// Suites 6 and 8 are the two ways a session ends badly and they are deliberately
// kept distinct: MALFORMED means the bytes present prove the frame is not a
// message of this protocol, TRUNCATED means the session simply ended mid-frame
// and nothing was wrong with what arrived. Only the caller knows which one it is
// looking at, because only the caller knows whether more bytes were coming.
//
// A plain CHECK macro reports file/line. Failed CHECKs accumulate and main()
// returns non-zero, so CTest genuinely fails on a bad run.
//
// Exit-code self-test: LLDB_SELFTEST_FAIL=1 runs only a deliberately failing
// suite and exits through the normal path, so the caller can assert non-zero.

#include "flat_order_book.h"
#include "map_order_book.h"
#include "market_data_pipeline.h"
#include "md_decoder.h"
#include "md_encoder.h"
#include "md_message.h"
#include "md_stream_decoder.h"
#include "md_stream_gen.h"
#include "md_threaded_pipeline.h"
#include "md_wire_protocol.h"
#include "types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using llob::FlatOrderBook;
using llob::Side;

using llmd::DecodeOutcome;
using llmd::DecodeStatus;
using llmd::MdCounters;
using llmd::MdMessage;
using llmd::MdState;
using llmd::MarketDataPipeline;
using llmd::StreamDecoder;
using llmd::ThreadedMdPipeline;

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

MdMessage B(std::uint64_t s) { return llmd::md_begin(s); }
MdMessage E(std::uint64_t s) { return llmd::md_end(s); }
MdMessage L(std::uint64_t s, Side side, std::int64_t px, std::int64_t qty) {
    return llmd::md_level(s, side, px, qty);
}

// ---------------------------------------------------------------------------
// The Phase-2 fixture.
//
// Written out message by message rather than generated, because the coverage is
// the point and a generator would hide which case is which. Between them these
// messages exercise every element the phase asks for:
//
//   * an initial snapshot with MULTIPLE levels on both sides
//   * live incrementals after it
//   * a DELETE (qty == 0)
//   * a STALE message (a sequence already consumed)
//   * a SEQUENCE GAP
//   * incrementals SUPPRESSED while out of sync
//   * a RECOVERY snapshot that closes the gap
//   * a live incremental after recovery
//
// Every message is wire-valid: sequence in [1, UINT64_MAX-1], a real side, a
// positive price and a non-negative quantity. The malformed cases are built
// separately, on purpose, so that "the normal fixture" and "the malformed
// fixture" can never be confused for one another.
//
// The gap is placed so that the messages after it are SUPPRESSED rather than
// merely stale: while `Gap`, the pipeline rejects incrementals outright and
// does not consume their sequences, so a bracketed run is the only way back.
// ---------------------------------------------------------------------------
std::vector<MdMessage> phase2_fixture() {
    return {
        // ---- opening snapshot: two levels per side (multi-level) ----
        B(1),
        L(2, Side::Bid, 100, 10),
        L(3, Side::Bid, 99, 5),
        L(4, Side::Ask, 101, 20),
        L(5, Side::Ask, 102, 7),
        E(6),

        // ---- live incrementals on top of the committed snapshot ----
        L(7, Side::Bid, 100, 12),  // replace the resting bid
        L(8, Side::Ask, 101, 0),   // DELETE the resting ask
        L(9, Side::Bid, 98, 4),    // a new best-adjacent level

        // ---- stale: 8 has already been consumed ----
        L(8, Side::Bid, 100, 999),

        // ---- gap: 11 arrives where 10 was expected ----
        L(11, Side::Bid, 100, 3),

        // ---- suppressed while out of sync (rejected, sequence not consumed) ----
        L(12, Side::Bid, 99, 44),
        L(13, Side::Ask, 102, 33),

        // ---- recovery snapshot, bracketed at 30 ----
        B(30),
        L(31, Side::Bid, 100, 50),
        L(32, Side::Bid, 99, 40),
        L(33, Side::Ask, 101, 30),
        L(34, Side::Ask, 102, 25),
        E(35),

        // ---- live incremental after recovery ----
        L(36, Side::Bid, 100, 55),
    };
}

// ---------------------------------------------------------------------------
// A deterministic chunk source.
//
// `sizes` is a repeating pattern of chunk sizes; each call takes the next size
// and clamps it to what remains, so the final chunk may be short and the plan
// always covers exactly the whole stream. Nothing here consults a clock: the
// chunk boundaries are a pure function of the pattern and the input, which is
// what makes "same stream, different chunking" a reproducible experiment rather
// than a source of flakes.
// ---------------------------------------------------------------------------
class ChunkSource {
public:
    ChunkSource(std::span<const std::byte> wire, std::vector<std::size_t> sizes)
        : wire_(wire), sizes_(std::move(sizes)) {}

    std::span<const std::byte> next() {
        if (off_ >= wire_.size() || sizes_.empty()) return {};
        std::size_t n = sizes_[i_ % sizes_.size()];
        ++i_;
        const std::size_t remaining = wire_.size() - off_;
        if (n > remaining) n = remaining;
        const std::span<const std::byte> s = wire_.subspan(off_, n);
        off_ += n;
        return s;
    }

private:
    std::span<const std::byte> wire_;
    std::vector<std::size_t>   sizes_;
    std::size_t                off_ = 0;
    std::size_t                i_   = 0;
};

// The named chunk plans, all applied to the same bytes.
std::vector<std::size_t> plan_whole(std::size_t n) { return {n}; }
std::vector<std::size_t> plan_one_byte() { return {1}; }
std::vector<std::size_t> plan_awkward() { return {3, 7, 11, 2, 17}; }

// A seeded pseudo-random plan. `std::mt19937_64` is exactly specified and is
// used; `std::uniform_int_distribution` is NOT, because its mapping is
// implementation-defined, so the repo's hand-rolled `uniform_below` is used
// instead — the same rule the Phase-1 generator follows.
std::vector<std::size_t> plan_random(std::uint64_t seed) {
    llmd::gen::Rng rng(seed);
    std::vector<std::size_t> sizes;
    sizes.reserve(64);
    for (int i = 0; i < 64; ++i) {
        sizes.push_back(static_cast<std::size_t>(1 + llmd::gen::uniform_below(rng, 8)));
    }
    return sizes;
}

// ---------------------------------------------------------------------------
// Observable state, so that "the two runs agree" is a statement about values
// rather than about code paths.
// ---------------------------------------------------------------------------
struct Observation {
    MdState       state = MdState::NotSynced;
    std::uint64_t last_applied = 0;
    std::uint64_t expected = 0;
    std::int64_t  best_bid = 0;
    std::int64_t  best_ask = 0;
    std::size_t   level_count = 0;
    MdCounters    counters{};
};

// The single-threaded reference: the frozen Phase-1 path, no queue, no threads.
Observation observe_single_thread(const std::vector<MdMessage>& messages) {
    MarketDataPipeline<FlatOrderBook> pipe;
    for (const MdMessage& m : messages) {
        pipe.apply(m);
    }
    Observation o;
    o.state        = pipe.state();
    o.last_applied = pipe.book().last_applied_seq();
    o.expected     = pipe.expected();
    o.best_bid     = pipe.book().best_bid();
    o.best_ask     = pipe.book().best_ask();
    o.level_count  = pipe.book().level_count();
    o.counters     = pipe.counters();
    return o;
}

template <std::size_t Cap>
Observation observe_threaded(const ThreadedMdPipeline<Cap>& p) {
    Observation o;
    o.state        = p.state();
    o.last_applied = p.last_applied_seq();
    o.expected     = p.expected();
    o.best_bid     = p.best_bid();
    o.best_ask     = p.best_ask();
    o.level_count  = p.level_count();
    o.counters     = p.counters();
    return o;
}

// Compare every counter by name, so a divergence names the field that moved.
void check_counters(const char* label, const MdCounters& got, const MdCounters& want) {
    ++g_checks;
    if (got.messages == want.messages && got.applied == want.applied &&
        got.out_of_range == want.out_of_range && got.staged == want.staged &&
        got.snapshot_committed == want.snapshot_committed &&
        got.snapshot_abandoned == want.snapshot_abandoned && got.stale == want.stale &&
        got.gap_detected == want.gap_detected && got.malformed == want.malformed &&
        got.rejected == want.rejected &&
        got.protocol_violations == want.protocol_violations &&
        got.sync_attempts == want.sync_attempts &&
        got.stale_snapshot_begins == want.stale_snapshot_begins &&
        got.malformed_snapshots == want.malformed_snapshots &&
        got.empty_snapshots_committed == want.empty_snapshots_committed &&
        got.failed_initial_syncs == want.failed_initial_syncs &&
        got.staging_limit_hits == want.staging_limit_hits &&
        got.nested_brackets_discarded == want.nested_brackets_discarded &&
        got.outages == want.outages && got.recovered_outages == want.recovered_outages) {
        return;
    }
    ++g_failures;
    std::printf("FAIL %s: counters diverged\n", label);
    std::printf("       got messages=%llu applied=%llu staged=%llu stale=%llu "
                "gap=%llu rejected=%llu snap_ok=%llu snap_bad=%llu\n",
                (unsigned long long)got.messages, (unsigned long long)got.applied,
                (unsigned long long)got.staged, (unsigned long long)got.stale,
                (unsigned long long)got.gap_detected, (unsigned long long)got.rejected,
                (unsigned long long)got.snapshot_committed,
                (unsigned long long)got.snapshot_abandoned);
    std::printf("       want messages=%llu applied=%llu staged=%llu stale=%llu "
                "gap=%llu rejected=%llu snap_ok=%llu snap_bad=%llu\n",
                (unsigned long long)want.messages, (unsigned long long)want.applied,
                (unsigned long long)want.staged, (unsigned long long)want.stale,
                (unsigned long long)want.gap_detected, (unsigned long long)want.rejected,
                (unsigned long long)want.snapshot_committed,
                (unsigned long long)want.snapshot_abandoned);
}

void check_same_observation(const char* label, const Observation& got,
                            const Observation& want) {
    CHECK(got.state == want.state);
    CHECK(got.last_applied == want.last_applied);
    CHECK(got.expected == want.expected);
    CHECK(got.best_bid == want.best_bid);
    CHECK(got.best_ask == want.best_ask);
    CHECK(got.level_count == want.level_count);
    check_counters(label, got.counters, want.counters);
}

// ---------------------------------------------------------------------------
// Suite 1 — stream framing.
//
// The framer's job is to turn arbitrary chunk boundaries into frame boundaries.
// Everything here is single-threaded and compared against the frozen
// `decode_one`, so a framing bug cannot hide behind a threading bug or vice
// versa.
// ---------------------------------------------------------------------------
void suite_framing() {
    const std::vector<MdMessage> msgs = {B(1), L(2, Side::Bid, 100, 10),
                                         L(3, Side::Ask, 101, 20), E(4)};
    const std::vector<std::byte> wire = llmd::encode::encode_all(msgs);

    // One complete message in one chunk.
    {
        StreamDecoder d;
        std::vector<MdMessage> got;
        const std::span<const std::byte> first(wire.data(), 12);
        CHECK(d.feed(first, [&](const MdMessage& m) { got.push_back(m); }) ==
              DecodeStatus::Ok);
        CHECK(got.size() == 1);
        CHECK(got[0].kind == llmd::MdKind::SnapshotBegin);
        CHECK(d.carry_size() == 0);
    }

    // Several complete messages in one chunk.
    {
        StreamDecoder d;
        std::vector<MdMessage> got;
        CHECK(d.feed(wire, [&](const MdMessage& m) { got.push_back(m); }) ==
              DecodeStatus::Ok);
        CHECK(got.size() == msgs.size());
        CHECK(d.carry_size() == 0);
    }

    // A header split across chunks: 1 byte at a time. Every prefix < 12 is a
    // retained carry, never an error.
    {
        StreamDecoder d;
        std::vector<MdMessage> got;
        std::size_t max_carry = 0;
        for (std::size_t i = 0; i < wire.size(); ++i) {
            const std::span<const std::byte> one(wire.data() + i, 1);
            const DecodeStatus s = d.feed(one, [&](const MdMessage& m) { got.push_back(m); });
            CHECK(s == DecodeStatus::Ok);
            if (d.carry_size() > max_carry) max_carry = d.carry_size();
        }
        CHECK(got.size() == msgs.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].kind == msgs[i].kind);
            CHECK(got[i].seq == msgs[i].seq);
            CHECK(got[i].price == msgs[i].price);
            CHECK(got[i].qty == msgs[i].qty);
        }
        // The retained partial never reaches a whole frame: 29 bytes is always
        // either a complete Level or a terminal length error, so at rest the
        // carry holds at most 28.
        CHECK(max_carry <= StreamDecoder::kCarryCapacity - 1);
        CHECK(d.carry_size() == 0);
    }

    // A Level payload split across chunks: cut inside every Level.
    {
        StreamDecoder d;
        std::vector<MdMessage> got;
        const std::size_t cuts[] = {12, 13, 20, 28, 40, 41};
        std::size_t off = 0;
        for (std::size_t c : cuts) {
            const std::size_t n = c - off;
            const std::span<const std::byte> chunk(wire.data() + off, n);
            CHECK(d.feed(chunk, [&](const MdMessage& m) { got.push_back(m); }) ==
                  DecodeStatus::Ok);
            off = c;
        }
        const std::span<const std::byte> rest(wire.data() + off, wire.size() - off);
        CHECK(d.feed(rest, [&](const MdMessage& m) { got.push_back(m); }) ==
              DecodeStatus::Ok);
        CHECK(got.size() == msgs.size());
        CHECK(d.carry_size() == 0);
    }

    // The mixed case the phase calls out: a chunk that ends halfway through
    // message N, whose successor contains the rest of N, all of N+1, and a
    // prefix of N+2.
    {
        StreamDecoder d;
        std::vector<MdMessage> got;
        // Layout: [B(1) 0..11][L(2) 12..40][L(3) 41..69][E(4) 70..81].
        const std::span<const std::byte> c1(wire.data(), 20);        // half of L(2)
        const std::span<const std::byte> c2(wire.data() + 20, 55);   // rest of L(2),
                                                                     // all of L(3),
                                                                     // prefix of E(4)
        const std::span<const std::byte> c3(wire.data() + 75, wire.size() - 75);
        CHECK(d.feed(c1, [&](const MdMessage& m) { got.push_back(m); }) == DecodeStatus::Ok);
        CHECK(got.size() == 1);      // only the bracket completed
        CHECK(d.carry_size() == 8);  // 20 - 12
        CHECK(d.feed(c2, [&](const MdMessage& m) { got.push_back(m); }) == DecodeStatus::Ok);
        CHECK(got.size() == 3);      // L(2) and L(3); E(4) is still split
        CHECK(d.feed(c3, [&](const MdMessage& m) { got.push_back(m); }) == DecodeStatus::Ok);
        CHECK(got.size() == 4);
        CHECK(d.carry_size() == 0);
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].kind == msgs[i].kind);
            CHECK(got[i].seq == msgs[i].seq);
        }
    }

    // An empty chunk is a no-op, not a terminator and not an error.
    {
        StreamDecoder d;
        std::vector<MdMessage> got;
        CHECK(d.feed({}, [&](const MdMessage& m) { got.push_back(m); }) == DecodeStatus::Ok);
        CHECK(got.empty());
        CHECK(d.carry_size() == 0);
    }

    summary("stream framing");
}

// ---------------------------------------------------------------------------
// Suite 2 — the same stream under different chunk plans.
//
// This is the phase's central framing claim: chunk boundaries are a transport
// detail and must not be observable in the decoded message sequence.
// ---------------------------------------------------------------------------
void suite_chunk_plans() {
    const std::vector<MdMessage> msgs = phase2_fixture();
    const std::vector<std::byte> wire = llmd::encode::encode_all(msgs);

    struct Plan {
        const char*              name;
        std::vector<std::size_t> sizes;
    };
    const std::vector<Plan> plans = {
        {"whole", plan_whole(wire.size())},
        {"one byte", plan_one_byte()},
        {"awkward 3,7,11,2,17", plan_awkward()},
        {"random seed 1", plan_random(1)},
        {"random seed 2", plan_random(2)},
        {"random seed 42", plan_random(42)},
    };

    for (const Plan& pl : plans) {
        ChunkSource src(wire, pl.sizes);
        StreamDecoder d;
        std::vector<MdMessage> got;
        for (;;) {
            const std::span<const std::byte> chunk = src.next();
            if (chunk.empty()) break;
            const DecodeStatus s = d.feed(chunk, [&](const MdMessage& m) {
                got.push_back(m);
            });
            // The fixture is entirely well-formed, so any non-Ok here is a bug
            // in the plan or the framer, not a property of the data.
            CHECK(s == DecodeStatus::Ok);
        }

        ++g_checks;
        if (got.size() != msgs.size()) {
            ++g_failures;
            std::printf("FAIL plan '%s': decoded %zu messages, want %zu\n", pl.name,
                        got.size(), msgs.size());
            continue;
        }
        for (std::size_t i = 0; i < got.size(); ++i) {
            ++g_checks;
            if (got[i].kind != msgs[i].kind || got[i].seq != msgs[i].seq ||
                got[i].side != msgs[i].side || got[i].price != msgs[i].price ||
                got[i].qty != msgs[i].qty) {
                ++g_failures;
                std::printf("FAIL plan '%s': message %zu differs\n", pl.name, i);
            }
        }
        // A complete plan leaves nothing retained.
        CHECK(d.carry_size() == 0);
    }

    summary("chunk plan equivalence");
}

// ---------------------------------------------------------------------------
// Suite 3 — the threaded run against the single-threaded Phase-1 reference.
// ---------------------------------------------------------------------------
void suite_threaded_reference() {
    const std::vector<MdMessage> msgs = phase2_fixture();
    const std::vector<std::byte> wire = llmd::encode::encode_all(msgs);
    const Observation want = observe_single_thread(msgs);

    // The fixture must actually exercise the cases it claims to. Without this,
    // a bug that flattened the whole stream to APPLIED would still "match" a
    // reference that had been computed from the same flattened stream -- the
    // reference is only meaningful if it is non-trivial.
    //
    // The counts are worked out message by message rather than observed from a
    // run, so they are a prediction the run can falsify:
    //
    //   messages            20   every message reaches the pipeline
    //   staged              10   5 opening snapshot content + 4 recovery
    //                            content + the recovery Begin itself
    //   applied              4   3 live incrementals before the gap + 1 after
    //                            the recovery
    //   snapshot_committed   2   the opening bracket and the recovery bracket
    //   stale                1   seq 8 replayed after 8 was already consumed
    //   gap_detected         1   seq 11 where 10 was expected
    //   rejected             2   the two incrementals suppressed while in Gap
    //   outages              1   the one episode the Gap opened
    //   recovered_outages    1   ...closed by the recovery commit
    CHECK(want.state == MdState::Live);
    CHECK(want.counters.messages == 20);
    CHECK(want.counters.staged == 10);
    CHECK(want.counters.applied == 4);
    CHECK(want.counters.snapshot_committed == 2);
    CHECK(want.counters.stale == 1);
    CHECK(want.counters.gap_detected == 1);
    CHECK(want.counters.rejected == 2);
    CHECK(want.counters.outages == 1);
    CHECK(want.counters.recovered_outages == 1);
    CHECK(want.counters.snapshot_abandoned == 0);
    CHECK(want.counters.malformed == 0);
    CHECK(want.counters.protocol_violations == 0);
    CHECK(want.counters.sync_attempts == 2);
    // The two representations of one fact, asserted rather than assumed: while
    // Live, the pipeline's watermark is the book's cursor plus one.
    CHECK(want.expected == want.last_applied + 1);
    CHECK(want.last_applied == 36);
    CHECK(want.best_bid == 100);
    CHECK(want.best_ask == 101);
    CHECK(want.level_count == 4);

    // One chunk per chunk plan, through the threads.
    struct Plan {
        const char*              name;
        std::vector<std::size_t> sizes;
    };
    const std::vector<Plan> plans = {
        {"whole", plan_whole(wire.size())},
        {"one byte", plan_one_byte()},
        {"awkward 3,7,11,2,17", plan_awkward()},
        {"random seed 7", plan_random(7)},
    };

    for (const Plan& pl : plans) {
        ThreadedMdPipeline<1024> p;
        ChunkSource src(wire, pl.sizes);
        p.run(src);

        const Observation got = observe_threaded(p);
        check_same_observation(pl.name, got, want);

        // Every decoded message reached the queue, and every queued message was
        // consumed: the pipeline being merely "not wrong" is not enough, the
        // counts have to close.
        CHECK(p.producer_stats().decoded_messages == msgs.size());
        CHECK(p.producer_stats().enqueued_messages == msgs.size());
        CHECK(p.consumer_stats().consumed_messages == msgs.size());
        CHECK(p.producer_stats().decoded_messages == p.producer_stats().enqueued_messages);
        CHECK(!p.producer_stats().terminal_error);
        CHECK(p.producer_stats().terminal_decode_status == DecodeStatus::Ok);
    }

    summary("threaded vs single-thread reference");
}

// ---------------------------------------------------------------------------
// Suite 4 — backpressure, deterministically.
//
// Capacity 2 with the consumer held behind a gate. The producer must therefore
// block on a full ring before the consumer has taken anything, which is the
// only way to exercise the retry path on purpose. The gate is released once the
// producer has PROVABLY seen a full ring, so this is not "sleep and hope".
// ---------------------------------------------------------------------------
template <std::size_t Cap>
void run_backpressure_case(const char* label) {
    const std::vector<MdMessage> msgs = phase2_fixture();
    const std::vector<std::byte> wire = llmd::encode::encode_all(msgs);
    const Observation want = observe_single_thread(msgs);

    std::atomic<bool> gate{false};
    ThreadedMdPipeline<Cap> p;
    p.set_consumer_gate(&gate);

    ChunkSource src(wire, plan_awkward());
    std::thread runner([&] { p.run(src); });

    // Spin until the producer reports a full ring.
    //
    // The bound is deliberately SMALL. It is not there to give the producer
    // time — signalling takes a couple of pushes and happens within
    // microseconds — it is there so that a producer which never signals (say,
    // one that drops instead of retrying, or a ring that never fills) is
    // reported as a FAILED CHECK rather than hanging the suite forever. A
    // bound large enough to look "safe" would be no bound at all: this loop
    // runs at millions of iterations per second, so ten million is already
    // orders of magnitude more patience than the case needs, and the gate is
    // released unconditionally afterwards so the run still terminates and the
    // failing CHECK below is what the reader sees.
    bool saw_full = false;
    for (std::uint64_t i = 0; i < 10'000'000ULL; ++i) {
        if (p.producer_saw_full()) { saw_full = true; break; }
        std::this_thread::yield();
    }
    CHECK(saw_full);

    gate.store(true, std::memory_order_release);
    runner.join();

    const char* why = label;
    const Observation got = observe_threaded(p);
    check_same_observation(why, got, want);

    CHECK(p.producer_stats().decoded_messages == msgs.size());
    CHECK(p.consumer_stats().consumed_messages == msgs.size());
    CHECK(p.producer_stats().producer_full_retries > 0);

    // The strongest available statement about ORDER: the pipeline is
    // order-sensitive by construction — a duplicate, a stale or an out-of-order
    // sequence produces different outcomes and different counters — so matching
    // the reference on every counter AND the final book is only possible if the
    // messages arrived in exactly the order they were decoded. A single swap
    // would move `applied`/`stale`/`gap_detected` and change the book.
    std::printf("       %s: cap=%zu full_retries=%llu empty_retries=%llu\n", label, Cap,
                (unsigned long long)p.producer_stats().producer_full_retries,
                (unsigned long long)p.consumer_stats().consumer_empty_retries);
}

void suite_backpressure() {
    run_backpressure_case<2>("capacity 2");
    run_backpressure_case<4>("capacity 4");
    summary("backpressure, no message dropped");
}

// ---------------------------------------------------------------------------
// Suite 5 — termination.
//
// The hazard this pins is specific: the consumer observes `producer_done`,
// decides the ring is empty, and exits — losing a message that was queued
// immediately before the flag was published. The stream here is short and ends
// with a live incremental, so the final message is exactly the one at risk, and
// its effect on the book is observable.
// ---------------------------------------------------------------------------
void suite_termination() {
    // A bracket, then ONE final incremental. If the last message is lost the
    // book still looks live, still has the same cursor at the End — only the
    // final quantity and `last_applied_seq` differ.
    const std::vector<MdMessage> msgs = {B(1), L(2, Side::Bid, 100, 10),
                                         E(3), L(4, Side::Bid, 100, 77)};
    const std::vector<std::byte> wire = llmd::encode::encode_all(msgs);
    const Observation want = observe_single_thread(msgs);

    // The reference must show the effect of the final message, or this suite
    // would pass even if the message were dropped.
    CHECK(want.last_applied == 4);
    CHECK(want.counters.applied == 1);

    // Capacity 2 makes the "queued just before done" window as tight as the
    // ring allows; the whole-bytes plan delivers everything in one chunk, so
    // the producer finishes and publishes the flag while the consumer is still
    // starting up.
    for (int rep = 0; rep < 8; ++rep) {
        ThreadedMdPipeline<2> p;
        ChunkSource src(wire, plan_whole(wire.size()));
        p.run(src);

        const Observation got = observe_threaded(p);
        check_same_observation("final message not lost", got, want);
        CHECK(p.consumer_stats().consumed_messages == msgs.size());
        CHECK(p.producer_stats().enqueued_messages == msgs.size());
    }

    // And the same with one byte per chunk, which widens the window between the
    // last push and the completion publication.
    for (int rep = 0; rep < 8; ++rep) {
        ThreadedMdPipeline<4> p;
        ChunkSource src(wire, plan_one_byte());
        p.run(src);

        const Observation got = observe_threaded(p);
        check_same_observation("final message not lost (1-byte chunks)", got, want);
        CHECK(p.consumer_stats().consumed_messages == msgs.size());
    }

    summary("termination keeps the final message");
}

// ---------------------------------------------------------------------------
// Suite 6 — malformed wire ends the session.
//
// The policy under test is that a terminal decode status stops the decoder
// thread WITHOUT resynchronisation: no byte is skipped, no message is invented,
// and whatever was already published is still delivered. The alternative —
// skipping a byte and continuing — is not implemented here on purpose, because
// `consumed` is 0 on malformed input and picking a resynchronisation point
// would be guessing.
// ---------------------------------------------------------------------------
void suite_malformed_wire() {
    const std::vector<MdMessage> prefix = {B(1), L(2, Side::Bid, 100, 10),
                                           L(3, Side::Ask, 101, 20), E(4),
                                           L(5, Side::Bid, 100, 12)};
    std::vector<std::byte> wire = llmd::encode::encode_all(prefix);

    // One malformed frame, appended after a complete valid prefix: a full,
    // well-formed Level whose `side` is 5. The frame is COMPLETE — a short one
    // would be retained as a partial and the session would end quietly instead
    // of terminally, which is a different case.
    //
    // Built with `append_raw`, which takes every header field VERBATIM. That is
    // what it is for: `append_message` maps a typed `MdMessage` and so cannot
    // express an invalid field at all, whereas the malformed cases are exactly
    // the ones that need one.
    llmd::encode::RawMessage bad_raw;
    bad_raw.type = static_cast<std::uint8_t>(llmd::wire::MessageType::Level);
    bad_raw.version = llmd::wire::kVersion;
    bad_raw.payload_length = static_cast<std::uint16_t>(llmd::wire::kLevelPayloadSize);
    bad_raw.sequence = 9;
    bad_raw.side = 5; // neither Bid (0) nor Ask (1)
    bad_raw.price = 100;
    bad_raw.quantity = 20;
    std::vector<std::byte> bad;
    llmd::encode::append_raw(bad, bad_raw);
    CHECK(bad.size() == llmd::wire::kLevelMessageSize);
    wire.insert(wire.end(), bad.begin(), bad.end());

    // Trailing valid bytes after the malformed frame. They must NOT be decoded:
    // the session is over, and continuing would mean resynchronising.
    const std::vector<std::byte> after = llmd::encode::encode(L(6, Side::Bid, 100, 99));
    wire.insert(wire.end(), after.begin(), after.end());

    const Observation want = observe_single_thread(prefix);

    // EVERY chunk plan, not just one.
    //
    // The status checks below are the policy; these runs are the CONSEQUENCE of
    // the policy, and the difference matters. A decoder that skipped a byte and
    // carried on would still report the status it hit — what it would not do is
    // stop, and whether the bytes that follow happen to resynchronise into
    // something decodable depends entirely on where the chunk boundaries fall.
    // Under one plan the residue may decode to nothing and the message counts
    // would look innocent; under another it may decode to a plausible message
    // that gets applied to the book. Running every plan is what turns "the
    // session ended" from a claim about a flag into a claim about the messages
    // and the book.
    struct Plan {
        const char*              name;
        std::vector<std::size_t> sizes;
    };
    const std::vector<Plan> plans = {
        {"whole", plan_whole(wire.size())},
        {"one byte", plan_one_byte()},
        {"awkward 3,7,11,2,17", plan_awkward()},
        {"random seed 3", plan_random(3)},
        {"random seed 11", plan_random(11)},
    };

    for (const Plan& pl : plans) {
        ThreadedMdPipeline<64> p;
        ChunkSource src(wire, pl.sizes);
        p.run(src);

        // The producer stopped at the bad frame, having published everything
        // before it and nothing after it.
        CHECK(p.producer_stats().terminal_error);
        CHECK(p.producer_stats().terminal_decode_status == DecodeStatus::InvalidSide);
        CHECK(p.producer_stats().decoded_messages == prefix.size());
        CHECK(p.producer_stats().enqueued_messages == prefix.size());

        // The consumer drained the valid prefix and exited cleanly.
        CHECK(p.consumer_stats().consumed_messages == prefix.size());

        // The final book is exactly the state after the valid prefix — the bad
        // frame and the trailing bytes changed nothing.
        const Observation got = observe_threaded(p);
        check_same_observation(pl.name, got, want);
    }

    // A reserved sequence is the other terminal case worth pinning here: it is
    // an otherwise PERFECT SnapshotBegin, so a decoder that applied it would
    // leave Live and reset the watermark.
    {
        std::vector<std::byte> w2 = llmd::encode::encode_all(prefix);
        llmd::encode::RawMessage bad_seq_raw;
        bad_seq_raw.type =
            static_cast<std::uint8_t>(llmd::wire::MessageType::SnapshotBegin);
        bad_seq_raw.version = llmd::wire::kVersion;
        bad_seq_raw.payload_length = 0;
        // The reserved value at the TOP of the domain. The mirror case (0) is
        // pinned byte-for-byte in md_decoder_tests.cpp; one of the two is enough
        // here, since the threaded layer only has to show that a terminal status
        // ends the session at all.
        bad_seq_raw.sequence = std::numeric_limits<std::uint64_t>::max();
        std::vector<std::byte> bad_seq;
        llmd::encode::append_raw(bad_seq, bad_seq_raw);
        CHECK(bad_seq.size() == llmd::wire::kHeaderSize);
        w2.insert(w2.end(), bad_seq.begin(), bad_seq.end());

        ThreadedMdPipeline<64> p2;
        ChunkSource src2(w2, plan_one_byte());
        p2.run(src2);

        CHECK(p2.producer_stats().terminal_error);
        CHECK(p2.producer_stats().terminal_decode_status == DecodeStatus::InvalidSequence);
        CHECK(p2.producer_stats().decoded_messages == prefix.size());
        CHECK(p2.consumer_stats().consumed_messages == prefix.size());
        const Observation got2 = observe_threaded(p2);
        check_same_observation("reserved sequence", got2, want);
    }

    summary("malformed wire terminates the session");
}

// ---------------------------------------------------------------------------
// Suite 7 — finalization: which streams actually ended on a frame boundary.
//
// `feed` cannot answer this question, and that is the entire reason `finish`
// exists. A retained partial frame is `Ok` mid-stream — it is the ordinary state
// of a framer waiting for the rest of a message on a socket — so nothing in the
// per-chunk return value separates "more bytes are coming" from "the stream
// stopped here". Only the component that owns the byte source knows the source
// is exhausted, so only it can ask, and it asks explicitly.
//
// The coverage is exhaustive rather than sampled, because the range is tiny: all
// 1..11 prefixes of a 12-byte header, and all 1..28 prefixes of a 29-byte Level.
// The cuts named in the brief (12, 13, 20, 28) are all inside that range; the
// rest cost a loop iteration each and close the boundary completely instead of
// leaving it to four samples.
// ---------------------------------------------------------------------------
void suite_finalization() {
    // What one feed-then-finalize reports. `published` is carried so a case can
    // assert that finalization itself published nothing and parsed nothing.
    struct Ended {
        DecodeStatus fed       = DecodeStatus::Ok;
        DecodeStatus fin       = DecodeStatus::Ok;
        std::size_t  carried   = 0;
        std::size_t  published = 0;
    };

    // Feed exactly `n` bytes of `wire` as ONE chunk, then finalize.
    auto feed_cut = [](std::span<const std::byte> wire, std::size_t n) {
        StreamDecoder d;
        Ended e;
        e.fed     = d.feed(wire.first(n), [&](const MdMessage&) { ++e.published; });
        e.fin     = d.finish();
        e.carried = d.carry_size();
        return e;
    };

    // Feed all of `wire` under a chunk plan, then finalize.
    auto feed_plan = [](std::span<const std::byte> wire, std::vector<std::size_t> sizes) {
        ChunkSource src(wire, std::move(sizes));
        StreamDecoder d;
        Ended e;
        for (;;) {
            const std::span<const std::byte> chunk = src.next();
            if (chunk.empty()) break;
            e.fed = d.feed(chunk, [&](const MdMessage&) { ++e.published; });
            if (e.fed != DecodeStatus::Ok) break;
        }
        e.fin     = d.finish();
        e.carried = d.carry_size();
        return e;
    };

    const std::vector<std::byte> bracket = llmd::encode::encode(B(1));
    const std::vector<std::byte> level   = llmd::encode::encode(L(2, Side::Bid, 100, 10));
    CHECK(bracket.size() == llmd::wire::kHeaderSize);
    CHECK(level.size() == llmd::wire::kLevelMessageSize);
    CHECK(StreamDecoder::kCarryCapacity == llmd::wire::kLevelMessageSize);

    std::vector<std::byte> bracket_level = bracket;
    bracket_level.insert(bracket_level.end(), level.begin(), level.end());

    // ---- A. an exact frame boundary reports Ok -----------------------------
    //
    // The empty stream is here too: nothing retained is nothing truncated. This
    // is the negative control for the whole suite. A `finish` that answered
    // NeedMoreData unconditionally would pass every truncation case below and
    // still be worthless, so the clean cases have to be pinned as hard as the
    // truncated ones.
    {
        StreamDecoder d;
        CHECK(d.finish() == DecodeStatus::Ok);
        CHECK(d.carry_size() == 0);
    }

    struct Clean {
        const char*                name;
        const std::vector<std::byte>* wire;
        std::size_t                msgs;
    };
    const std::vector<Clean> clean = {
        {"one bracket", &bracket, 1},
        {"one level", &level, 1},
        {"bracket + level", &bracket_level, 2},
    };
    for (const Clean& c : clean) {
        const std::vector<std::vector<std::size_t>> plans = {
            plan_whole(c.wire->size()), plan_one_byte(), plan_awkward()};
        for (const std::vector<std::size_t>& sizes : plans) {
            const Ended e = feed_plan(*c.wire, sizes);
            CHECK(e.fed == DecodeStatus::Ok);
            CHECK(e.published == c.msgs);
            CHECK(e.carried == 0);
            CHECK(e.fin == DecodeStatus::Ok);
        }
    }

    // ---- B. a partial HEADER is NeedMoreData at EOF ------------------------
    //
    // 1..11 bytes of a 12-byte header. Mid-stream each is ordinary — the framer
    // is simply waiting — and at EOF each is a session that died inside a
    // header. Same bytes, same status, different fact, and only the caller can
    // tell them apart.
    for (std::size_t n = 1; n < llmd::wire::kHeaderSize; ++n) {
        const Ended e = feed_cut(bracket, n);
        CHECK(e.fed == DecodeStatus::Ok);
        CHECK(e.published == 0);
        CHECK(e.carried == n);
        CHECK(e.fin == DecodeStatus::NeedMoreData);
    }

    // ---- C. a partial LEVEL is NeedMoreData at EOF -------------------------
    //
    // Every cut of a 29-byte Level. Four are structurally distinct and worth
    // naming:
    //
    //    12   header complete, payload entirely absent
    //    13   one payload byte
    //    20   mid-payload
    //    28   one byte short — the largest prefix that can exist at all
    //
    // and 28 is exactly the ceiling the fixed carry is sized for. A partial
    // frame can never exceed kLevelMessageSize - 1 bytes, which is why one
    // `std::array<std::byte, 29>` holds every prefix there is and why the
    // steady path never needs to grow a buffer.
    for (std::size_t n = 1; n < level.size(); ++n) {
        const Ended e = feed_cut(level, n);
        CHECK(e.fed == DecodeStatus::Ok);
        CHECK(e.published == 0);
        CHECK(e.carried == n);
        CHECK(e.fin == DecodeStatus::NeedMoreData);
    }
    CHECK(level.size() - 1 == StreamDecoder::kCarryCapacity - 1);

    // ---- D. a truncated decoder is not a dead one --------------------------
    //
    // `finish` reports the carry and changes nothing: it does not clear it and
    // it parses no bytes. So the SAME decoder that reported NeedMoreData at a
    // cut goes on to report Ok once the remainder arrives and completes the
    // frame. The tempting "tidy up on finalize" implementation — drop the
    // partial frame so the object is left clean — could not, and would silently
    // lose the rest of a message that a caller had merely paused on.
    const std::span<const std::byte> level_span(level);
    for (std::size_t n = 1; n < level.size(); ++n) {
        StreamDecoder d;
        std::size_t published = 0;
        CHECK(d.feed(level_span.first(n), [&](const MdMessage&) { ++published; }) ==
              DecodeStatus::Ok);
        CHECK(d.finish() == DecodeStatus::NeedMoreData);
        CHECK(d.carry_size() == n);
        CHECK(published == 0);

        // Idempotent: asking twice reads one field and says the same thing.
        CHECK(d.finish() == DecodeStatus::NeedMoreData);
        CHECK(d.carry_size() == n);

        // The remainder arrives. The frame completes, exactly one message is
        // published, and the stream is clean.
        CHECK(d.feed(level_span.subspan(n), [&](const MdMessage&) { ++published; }) ==
              DecodeStatus::Ok);
        CHECK(published == 1);
        CHECK(d.carry_size() == 0);
        CHECK(d.finish() == DecodeStatus::Ok);
    }

    // ---- the terminal case, and why it is not a contradiction --------------
    //
    // A malformed frame clears the carry and ends the session inside `feed`, so
    // by the time `finish` is asked there is nothing left to finalize and it
    // answers Ok. That is correct, and it is not in tension with anything
    // above: `NeedMoreData` at EOF means "the stream stopped mid-frame", while
    // this means "the stream stopped, cleanly, because the bytes were proven
    // bad". The pipeline takes its `terminal_error` from the `feed` status in
    // this case, not from this call, and `feed`'s report is the one that
    // matters. Pinned so the distinction is a documented fact rather than a
    // surprise for the next reader.
    {
        std::vector<std::byte> bad = bracket;
        bad[llmd::wire::kOffsetVersion] = std::byte{0}; // not kvVersion
        StreamDecoder d;
        std::size_t published = 0;
        CHECK(d.feed(bad, [&](const MdMessage&) { ++published; }) ==
              DecodeStatus::InvalidVersion);
        CHECK(published == 0);
        CHECK(d.carry_size() == 0);
        CHECK(d.finish() == DecodeStatus::Ok);
    }

    summary("stream finalization");
}

// ---------------------------------------------------------------------------
// Suite 8 — a stream that stops MID-FRAME.
//
// This is the defect the finalization call closes. Before it, a session whose
// byte source simply ended while the framer still held a partial frame reported
// a CLEAN SUCCESS: `terminal_error == false`, `terminal_decode_status == Ok`,
// and the retained bytes vanished without appearing in any count, any counter or
// any log. The prefix was delivered correctly, the book was correct, and the
// missing message was invisible — the worst shape a data-integrity bug can take.
//
// Note what is NOT in question here: the valid prefix is delivered either way,
// and must be. So the assertions come in two halves, and the second half is the
// one that keeps the fix honest —
//
//   reported      the session is TRUNCATED: terminal_error, NeedMoreData, and
//                 the partial frame in no count.
//   unaffected    everything before the truncation is identical to the
//                 single-threaded Phase-1 reference for the prefix ALONE.
//
// The second half is what stops the first from being satisfied by breaking
// something else: a "fix" that dropped the last valid message, or reported an
// error on clean input, fails here.
// ---------------------------------------------------------------------------
void suite_truncated_eof() {
    // The valid prefix: a complete snapshot bracket plus one live update, so the
    // session reaches LIVE and the final book is a real, checkable state rather
    // than an empty one.
    const std::vector<MdMessage> prefix = {B(1), L(2, Side::Bid, 100, 10),
                                           L(3, Side::Ask, 101, 20), E(4),
                                           L(5, Side::Bid, 100, 12)};
    const std::vector<std::byte> wire = llmd::encode::encode_all(prefix);

    // The frame the session dies inside. A Level, so a cut can land either in a
    // header or in a payload, and carrying a price that would be plainly visible
    // in the book if it were ever published — the point of the test is that a
    // truncated frame reaches neither the counts nor the levels.
    const std::vector<std::byte> next_frame = llmd::encode::encode(L(6, Side::Bid, 777, 5));

    CHECK(wire.size() == 111); // 12 + 29 + 29 + 12 + 29
    CHECK(next_frame.size() == llmd::wire::kLevelMessageSize);

    const Observation want = observe_single_thread(prefix);
    CHECK(want.state == MdState::Live);
    CHECK(want.last_applied == 5);
    CHECK(want.expected == 6);
    CHECK(want.best_bid == 100); // 777 must never appear
    CHECK(want.best_ask == 101);
    CHECK(want.level_count == 2);

    // Four chunk plans, so the EOF verdict is shown to be independent of how the
    // transport happened to slice the bytes — including plans whose boundaries
    // fall in the middle of the truncated frame, which is where a framing bug
    // would hide.
    struct Plan {
        const char*              name;
        std::vector<std::size_t> sizes;
    };
    const std::vector<Plan> plans = {
        {"whole/large", plan_whole(static_cast<std::size_t>(1) << 16)}, // clamped
        {"one byte", plan_one_byte()},
        {"awkward 3,7,11,2,17", plan_awkward()},
        {"random seed 5", plan_random(5)},
    };

    auto check_truncated = [&](const char* label, const ThreadedMdPipeline<64>& p) {
        const auto& ps = p.producer_stats();
        const auto& cs = p.consumer_stats();

        ++g_checks;
        if (!(ps.terminal_error &&
              ps.terminal_decode_status == DecodeStatus::NeedMoreData &&
              ps.decoded_messages == prefix.size() &&
              ps.enqueued_messages == prefix.size() &&
              cs.consumed_messages == prefix.size())) {
            ++g_failures;
            std::printf("FAIL %s: truncated EOF not reported\n", label);
            std::printf("       terminal_error=%d status=%s\n",
                        static_cast<int>(ps.terminal_error),
                        llmd::decode_status_name(ps.terminal_decode_status));
            std::printf("       decoded=%llu enqueued=%llu consumed=%llu (want %zu each)\n",
                        (unsigned long long)ps.decoded_messages,
                        (unsigned long long)ps.enqueued_messages,
                        (unsigned long long)cs.consumed_messages, prefix.size());
        }

        // The prefix's effect on the book, counter for counter, against the
        // single-threaded Phase-1 reference for the prefix alone.
        check_same_observation(label, observe_threaded(p), want);
    };

    // Every possible cut of the next frame: 1..28 bytes present, the rest never
    // sent. 28 is the largest prefix that can exist, so this covers the ceiling
    // as well as the boundary cases (12 = header complete, no payload).
    for (std::size_t cut = 1; cut < next_frame.size(); ++cut) {
        std::vector<std::byte> truncated = wire;
        const std::span<const std::byte> partial =
            std::span<const std::byte>(next_frame).first(cut);
        truncated.insert(truncated.end(), partial.begin(), partial.end());

        for (const Plan& pl : plans) {
            ThreadedMdPipeline<64> p;
            ChunkSource src(truncated, pl.sizes);
            p.run(src);

            char label[80];
            std::snprintf(label, sizeof label, "%s, cut %zu", pl.name, cut);
            check_truncated(label, p);
        }
    }

    // The negative control: the SAME prefix with no truncated frame appended is
    // a clean session. Without it, a finalization that always answered "ended
    // mid-frame" would satisfy every case above.
    for (const Plan& pl : plans) {
        ThreadedMdPipeline<64> p;
        ChunkSource src(wire, pl.sizes);
        p.run(src);

        CHECK(!p.producer_stats().terminal_error);
        CHECK(p.producer_stats().terminal_decode_status == DecodeStatus::Ok);
        CHECK(p.producer_stats().decoded_messages == prefix.size());
        CHECK(p.producer_stats().enqueued_messages == prefix.size());
        CHECK(p.consumer_stats().consumed_messages == prefix.size());
        check_same_observation(pl.name, observe_threaded(p), want);
    }

    summary("truncated input at EOF");
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

    std::printf("Experiment 03 Phase 2 — threaded decoder -> SPSC -> book correctness\n\n");
    suite_framing();
    suite_chunk_plans();
    suite_threaded_reference();
    suite_backpressure();
    suite_termination();
    suite_malformed_wire();
    suite_finalization();
    suite_truncated_eof();

    if (g_failures_total == 0) {
        std::printf("\nALL SUITES PASSED\n");
    } else {
        std::printf("\n%d CHECK FAILURE(S)\n", g_failures_total);
    }
    return g_failures_total == 0 ? 0 : 1;
}
