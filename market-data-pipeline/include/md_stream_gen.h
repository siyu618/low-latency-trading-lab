#pragma once

#include "md_message.h"
#include "types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Experiment 03 — deterministic market-data stream generator.
//
// Produces MdMessage traces for the sequencer's correctness suite. Everything
// here is PURE: no clock, no I/O, no pipeline. A trace is a value, and building
// one twice from the same (scenario, seed, recipe) yields the same bytes.
//
// A trace is built in two stages, and the split is the point:
//
//   1. a BASE SCENARIO, which is always a well-formed in-order stream that a
//      correct pipeline follows without ever losing sync;
//   2. a RECIPE of MUTATIONS, applied in order to the message vector, each one
//      a single named way a real feed goes wrong.
//
// The catalogue is the human-auditable half: one function per failure
// hypothesis, each independently reviewable and independently pinnable. The
// seeded grammar is the half that actually explores combinations — a fixed
// catalogue only ever exercises the mutations someone thought of in isolation.
// Both are kept, and a trace records which was used so a failure can be
// reproduced from `(scenario, seed, recipe)` alone.
//
// DETERMINISM RULES — these are load-bearing, not stylistic:
//
//   * `std::mt19937_64` IS exactly specified by the standard and is safe.
//   * `std::uniform_int_distribution` is NOT: the standard leaves its mapping
//     from engine output implementation-defined, so the same seed can produce
//     different streams on libstdc++ and libc++. `uniform_below()` below is
//     hand-rolled for that reason. (benchmark/stream_gen.h uses the standard
//     distributions; that is fine there, where no golden value is pinned.)
//   * `std::shuffle` is NOT specified either. Reordering is an explicit swap.
//
// The mutation functions edit ONLY the message vector. They never consult a
// book, never look at a pipeline, and never read a clock — a mutation that
// needed to know how the pipeline would react could not be used to test it.
// ---------------------------------------------------------------------------

namespace llmd::gen {

using Rng = std::mt19937_64;

// Uniform in [0, n). Hand-rolled: see the determinism note above.
inline std::uint64_t uniform_below(Rng& rng, std::uint64_t n) noexcept {
    return n == 0 ? 0 : (rng() % n);
}

inline std::size_t pick_index(Rng& rng, std::size_t n) noexcept {
    return static_cast<std::size_t>(uniform_below(rng, static_cast<std::uint64_t>(n)));
}

// ---------------------------------------------------------------------------
// Base scenarios. Every one of these is well-formed: strictly contiguous
// sequence numbers, valid snapshot content (distinct prices per side, inside
// the domain), no negative quantities.
// ---------------------------------------------------------------------------
enum class MdScenario : std::uint8_t {
    UserSketch,     // the specification sketch, verbatim, gap included
    CleanSteady,    // one opening snapshot, then a long quiet incremental run
    SnapshotHeavy,  // periodic refresh brackets starting exactly at the cursor
    RecoveryHeavy,  // repeated losses, each repaired by a bracket
};

constexpr int kScenarioCount = 4;

inline const char* scenario_name(MdScenario s) noexcept {
    switch (s) {
        case MdScenario::UserSketch:    return "UserSketch";
        case MdScenario::CleanSteady:   return "CleanSteady";
        case MdScenario::SnapshotHeavy: return "SnapshotHeavy";
        case MdScenario::RecoveryHeavy: return "RecoveryHeavy";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The mutation catalogue. Each entry is one way a feed goes wrong.
//
//   DropSlice                 erase a run of messages      -> forward gap
//   DropOne                   erase one message            -> forward gap
//   DuplicateOne              re-send a message            -> replay / Stale
//   ReorderAdjacent           swap two neighbours          -> replay then gap
//   Renumber                  rewrite one seq              -> collision or gap
//   InjectSnapshot            splice a valid bracket in    -> mid-stream refresh
//   TruncateMidSnapshot       cut inside a bracket         -> bracket never closes
//   EmptySnapshot             Begin(N) + End(N+1)          -> synced-empty book
//   NestBegin                 Begin inside an open bracket -> nested bracket
//   DoubleEnd                 an End with nothing open     -> protocol violation
//   StaleBegin                a bracket below the cursor   -> freshness gate
//   NegativeQty               qty = -1                     -> malformed content
//   OutOfDomainPrice          price outside the domain     -> OutOfRange
//   DuplicatePriceInSnapshot  repeat a price inside a bracket -> snapshot refused
//   ZeroWidthEnd              End == its own Begin's seq   -> stale close
// ---------------------------------------------------------------------------
enum class MdMutation : std::uint8_t {
    DropSlice,
    DropOne,
    DuplicateOne,
    ReorderAdjacent,
    Renumber,
    InjectSnapshot,
    TruncateMidSnapshot,
    EmptySnapshot,
    NestBegin,
    DoubleEnd,
    StaleBegin,
    NegativeQty,
    OutOfDomainPrice,
    DuplicatePriceInSnapshot,
    ZeroWidthEnd,
};

constexpr int kMutationCount = 15;

inline const char* mutation_name(MdMutation m) noexcept {
    switch (m) {
        case MdMutation::DropSlice:                return "DropSlice";
        case MdMutation::DropOne:                  return "DropOne";
        case MdMutation::DuplicateOne:             return "DuplicateOne";
        case MdMutation::ReorderAdjacent:          return "ReorderAdjacent";
        case MdMutation::Renumber:                 return "Renumber";
        case MdMutation::InjectSnapshot:           return "InjectSnapshot";
        case MdMutation::TruncateMidSnapshot:      return "TruncateMidSnapshot";
        case MdMutation::EmptySnapshot:            return "EmptySnapshot";
        case MdMutation::NestBegin:                return "NestBegin";
        case MdMutation::DoubleEnd:                return "DoubleEnd";
        case MdMutation::StaleBegin:               return "StaleBegin";
        case MdMutation::NegativeQty:              return "NegativeQty";
        case MdMutation::OutOfDomainPrice:         return "OutOfDomainPrice";
        case MdMutation::DuplicatePriceInSnapshot: return "DuplicatePriceInSnapshot";
        case MdMutation::ZeroWidthEnd:             return "ZeroWidthEnd";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// A trace is a value. `seed`, `base` and `recipe` are stored WITH the messages
// so that a failure can be reproduced, and reported, from the trace alone —
// a vector of messages with no provenance is not a reproducible test case.
// ---------------------------------------------------------------------------
struct MdTrace {
    std::vector<MdMessage>   messages{};
    std::uint64_t            seed = 0;
    MdScenario               base = MdScenario::CleanSteady;
    std::vector<MdMutation>  recipe{};
    const char*              label = "";

    // FNV-1a over the serialized messages. Pins generator behaviour: two builds
    // from the same inputs must agree, and a silent drift in the generator
    // changes this value everywhere at once.
    std::uint64_t fingerprint() const noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        auto mix = [&h](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                h ^= (v >> (8 * i)) & 0xFFULL;
                h *= 1099511628211ULL;
            }
        };
        mix(messages.size());
        for (const MdMessage& m : messages) {
            mix(static_cast<std::uint64_t>(m.kind));
            mix(m.seq);
            mix(static_cast<std::uint64_t>(m.side));
            mix(static_cast<std::uint64_t>(m.price));
            mix(static_cast<std::uint64_t>(m.qty));
        }
        return h;
    }

    // One line per message. Printed on failure so a divergence is readable
    // without re-running anything.
    std::string dump() const {
        std::string out;
        char buf[160];
        for (std::size_t i = 0; i < messages.size(); ++i) {
            const MdMessage& m = messages[i];
            if (m.kind == MdKind::Level) {
                std::snprintf(buf, sizeof(buf), "  [%3zu] %-13s seq=%-6llu %s price=%lld qty=%lld\n",
                              i, md_kind_name(m.kind),
                              static_cast<unsigned long long>(m.seq),
                              llob::is_bid(m.side) ? "Bid" : "Ask",
                              static_cast<long long>(m.price),
                              static_cast<long long>(m.qty));
            } else {
                std::snprintf(buf, sizeof(buf), "  [%3zu] %-13s seq=%llu\n", i,
                              md_kind_name(m.kind),
                              static_cast<unsigned long long>(m.seq));
            }
            out += buf;
        }
        return out;
    }

    // The reproduction key, as text.
    std::string recipe_text() const {
        std::string out = scenario_name(base);
        out += " seed=";
        out += std::to_string(seed);
        for (MdMutation m : recipe) {
            out += " +";
            out += mutation_name(m);
        }
        return out;
    }
};

// The domain here defaults to the books' OWN default domain, and that is not a
// convenience — it is the invariant that makes OutOfDomainPrice mean anything.
// A book constructed with a wider domain than the generator would accept every
// "out of domain" price as perfectly ordinary, and the mutation would silently
// test nothing. Books under test must be constructed as
// `Book(cfg.tick_min, cfg.tick_max)`.
struct MdGenConfig {
    std::int64_t tick_min = llob::kDefaultTickMin;
    std::int64_t tick_max = llob::kDefaultTickMax;
    std::size_t  base_levels = 32;    // live levels per side in the opening snapshot
    std::size_t  base_messages = 400; // incrementals emitted after the opening snapshot
    std::uint32_t mutations = 0;      // mutations applied by build()
};

// ---------------------------------------------------------------------------
// Builder. Holds the generator's own mirror of the book so that snapshot
// content is distinct-by-construction and incrementals are well-formed. The
// mirror is bookkeeping only and never reaches a pipeline.
// ---------------------------------------------------------------------------
namespace detail {

inline std::int64_t window_hi(std::int64_t tick_max, std::size_t levels) {
    return tick_max - static_cast<std::int64_t>(levels) + 1;
}

class Builder {
public:
    Builder(const MdGenConfig& cfg, Rng& rng) : cfg_(cfg), rng_(rng) {}

    // Emit a full bracket over the current mirror; returns nothing, advances seq_.
    void emit_bracket(std::vector<MdMessage>& out) {
        out.push_back(md_begin(seq_++));
        for (const auto& [p, q] : bids_) out.push_back(md_level(seq_++, llob::Side::Bid, p, q));
        for (const auto& [p, q] : asks_) out.push_back(md_level(seq_++, llob::Side::Ask, p, q));
        out.push_back(md_end(seq_++));
    }

    void seed_mirror() {
        const std::int64_t lo = cfg_.tick_min;
        const std::int64_t hi = cfg_.tick_max;
        for (std::size_t i = 0; i < cfg_.base_levels; ++i) {
            const std::int64_t off = static_cast<std::int64_t>(i);
            const std::int64_t bp = hi - off;
            const std::int64_t ap = lo + off;
            if (bp <= ap) break; // never cross the book
            bids_[bp] = qty_for(i);
            asks_[ap] = qty_for(i + 1);
        }
    }

    // One well-formed incremental. Returns the message; the mirror is updated.
    MdMessage emit_incremental() {
        const llob::Side side = (uniform_below(rng_, 2) == 0) ? llob::Side::Bid : llob::Side::Ask;
        auto& live = (side == llob::Side::Bid) ? bids_ : asks_;
        std::vector<std::int64_t> keys;
        keys.reserve(live.size());
        for (const auto& [p, q] : live) keys.push_back(p);

        const std::uint64_t roll = uniform_below(rng_, 100);
        if (roll < 60 && !keys.empty()) {           // requantify a live level
            const std::int64_t p = keys[pick_index(rng_, keys.size())];
            const std::int64_t q = qty_for(static_cast<std::size_t>(uniform_below(rng_, 500)));
            live[p] = q;
            return md_level(seq_++, side, p, q);
        }
        if (roll < 80 && live.size() > 2) {          // delete a live level
            const std::int64_t p = keys[pick_index(rng_, keys.size())];
            live.erase(p);
            return md_level(seq_++, side, p, 0);
        }
        // add at an absent price inside this side's window
        const std::int64_t lo = (side == llob::Side::Bid)
                                    ? window_hi(cfg_.tick_max, cfg_.base_levels)
                                    : cfg_.tick_min;
        const std::int64_t hi = (side == llob::Side::Bid)
                                    ? cfg_.tick_max
                                    : cfg_.tick_min + static_cast<std::int64_t>(cfg_.base_levels) - 1;
        const std::int64_t span = hi - lo + 1;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const std::int64_t p =
                lo + static_cast<std::int64_t>(uniform_below(rng_, static_cast<std::uint64_t>(span)));
            if (live.find(p) == live.end()) {
                const std::int64_t q = qty_for(static_cast<std::size_t>(uniform_below(rng_, 500)));
                live[p] = q;
                return md_level(seq_++, side, p, q);
            }
        }
        const std::int64_t p = keys.empty() ? lo : keys.front(); // window full: requantify
        const std::int64_t q = qty_for(static_cast<std::size_t>(uniform_below(rng_, 500)));
        live[p] = q;
        return md_level(seq_++, side, p, q);
    }

    std::uint64_t seq() const noexcept { return seq_; }
    void set_seq(std::uint64_t s) noexcept { seq_ = s; }
    void advance_seq(std::uint64_t by) noexcept { seq_ += by; }

private:
    static std::int64_t qty_for(std::size_t i) {
        return 10 + static_cast<std::int64_t>(i % 997);
    }

    MdGenConfig cfg_;
    Rng&        rng_;
    std::uint64_t seq_ = 1;
    std::map<std::int64_t, std::int64_t> bids_{};
    std::map<std::int64_t, std::int64_t> asks_{};
};

} // namespace detail

// ---------------------------------------------------------------------------
// The specification sketch, as a literal.
//
//   SnapshotBegin 1 / Bid 100 q10 seq 2 / Ask 101 q20 seq 3 / SnapshotEnd 4
//   Bid 100 q15 seq 5 / Ask 101 q0 seq 6
//   seq 9  <- gap (7 and 8 never arrive)
//   seq 10 <- rejected while in GAP
//   SnapshotBegin 20 / ... / SnapshotEnd -> LIVE again
//
// This is the acceptance test for the whole design: it is the one trace whose
// expected behaviour was specified by hand rather than derived from the code.
// ---------------------------------------------------------------------------
inline MdTrace build_user_sketch() {
    MdTrace t;
    t.base = MdScenario::UserSketch;
    t.label = "user sketch";
    std::vector<MdMessage>& v = t.messages;
    v.push_back(md_begin(1));
    v.push_back(md_level(2, llob::Side::Bid, 100, 10));
    v.push_back(md_level(3, llob::Side::Ask, 101, 20));
    v.push_back(md_end(4));
    v.push_back(md_level(5, llob::Side::Bid, 100, 15));
    v.push_back(md_level(6, llob::Side::Ask, 101, 0));
    // 7 and 8 are lost in transit; 9 is the message that reveals it.
    v.push_back(md_level(9, llob::Side::Bid, 100, 7));
    v.push_back(md_level(10, llob::Side::Bid, 100, 8));
    // A fresh snapshot repairs the view.
    v.push_back(md_begin(20));
    v.push_back(md_level(21, llob::Side::Bid, 100, 15));
    v.push_back(md_level(22, llob::Side::Ask, 101, 25));
    v.push_back(md_end(23));
    // End + 1: proof that the stream resumes from the bracket's own sequence,
    // not from wherever it had got to before the outage. Priced below the
    // restored best bid so the sketch does not accidentally depict a crossed
    // book — ordering is not part of the snapshot contract, but a reader could
    // reasonably mistake it for one.
    v.push_back(md_level(24, llob::Side::Bid, 99, 5));
    return t;
}

inline MdTrace build_base(MdScenario s, const MdGenConfig& cfg, std::uint64_t seed) {
    if (s == MdScenario::UserSketch) {
        MdTrace t = build_user_sketch();
        t.seed = seed;
        return t;
    }

    MdTrace t;
    t.base = s;
    t.seed = seed;
    t.label = scenario_name(s);

    Rng rng(seed);
    detail::Builder b(cfg, rng);
    b.seed_mirror();
    b.emit_bracket(t.messages); // opening snapshot, sequences 1..2+levels

    const std::size_t refresh_every = 64;
    const std::size_t gap_every = 48;

    for (std::size_t i = 0; i < cfg.base_messages; ++i) {
        if (s == MdScenario::SnapshotHeavy && i > 0 && (i % refresh_every) == 0) {
            b.emit_bracket(t.messages); // legitimate mid-stream refresh
            continue;
        }
        if (s == MdScenario::RecoveryHeavy && i > 0 && (i % gap_every) == 0) {
            // Messages are lost: the counter advances without them, then a
            // bracket at the new position repairs the view.
            b.advance_seq(3);
            b.emit_bracket(t.messages);
            continue;
        }
        t.messages.push_back(b.emit_incremental());
    }
    return t;
}

// ---------------------------------------------------------------------------
// Mutations. Each edits only the message vector.
// ---------------------------------------------------------------------------
namespace detail {

// Index of the first message at or after `from` with the given kind, or npos.
inline std::size_t find_kind(const std::vector<MdMessage>& v, MdKind k, std::size_t from) {
    for (std::size_t i = from; i < v.size(); ++i) {
        if (v[i].kind == k) return i;
    }
    return v.size();
}

// Index of the LAST message with the given kind, or npos.
inline std::size_t find_last_kind(const std::vector<MdMessage>& v, MdKind k) {
    for (std::size_t i = v.size(); i-- > 0;) {
        if (v[i].kind == k) return i;
    }
    return v.size();
}

// A sequence number that is plausible at position `i`: the neighbour's.
inline std::uint64_t near_seq(const std::vector<MdMessage>& v, std::size_t i) {
    if (i < v.size()) return v[i].seq;
    return v.empty() ? 1 : v.back().seq + 1;
}

inline void mut_drop_slice(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.size() < 4) return;
    const std::size_t i = pick_index(rng, v.size() - 2);
    const std::size_t room = v.size() - i;
    const std::size_t len = 1 + pick_index(rng, std::min<std::size_t>(4, room));
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(i),
            v.begin() + static_cast<std::ptrdiff_t>(i + len));
}

inline void mut_drop_one(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.empty()) return;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(pick_index(rng, v.size())));
}

inline void mut_duplicate_one(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.empty()) return;
    const std::size_t i = pick_index(rng, v.size());
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(i + 1), v[i]);
}

inline void mut_reorder_adjacent(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.size() < 2) return;
    const std::size_t i = pick_index(rng, v.size() - 1);
    std::swap(v[i], v[i + 1]);
}

inline void mut_renumber(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.empty()) return;
    const std::size_t i = pick_index(rng, v.size());
    const std::uint64_t delta = 1 + uniform_below(rng, 4);
    v[i].seq = (uniform_below(rng, 2) == 0) ? v[i].seq + delta
                                            : (v[i].seq > delta ? v[i].seq - delta : v[i].seq + delta);
}

inline void mut_inject_snapshot(MdTrace& t, Rng& rng, const MdGenConfig& cfg) {
    auto& v = t.messages;
    if (v.empty()) return;
    const std::size_t i = pick_index(rng, v.size());
    const std::uint64_t s = near_seq(v, i);
    const std::int64_t bp = cfg.tick_max - 2;
    const std::int64_t ap = cfg.tick_min + 1;
    std::vector<MdMessage> ins;
    ins.push_back(md_begin(s));
    ins.push_back(md_level(s + 1, llob::Side::Bid, bp, 11));
    ins.push_back(md_level(s + 2, llob::Side::Ask, ap, 12));
    ins.push_back(md_end(s + 3));
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), ins.begin(), ins.end());
}

inline void mut_truncate_mid_snapshot(MdTrace& t, Rng&, const MdGenConfig&) {
    auto& v = t.messages;
    // The LAST bracket, not the first: cutting at the opening snapshot would
    // discard almost the whole trace, and any mutation composed after this one
    // in the same recipe would then have nothing left to act on. Cutting at the
    // final bracket leaves the stream intact and still ends mid-snapshot.
    const std::size_t i = find_last_kind(v, MdKind::SnapshotBegin);
    if (i == v.size()) return;
    const std::size_t keep = std::min(v.size(), i + 2); // Begin + at most one level
    v.resize(keep);
}

inline void mut_empty_snapshot(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.empty()) return;
    const std::size_t i = pick_index(rng, v.size());
    const std::uint64_t s = near_seq(v, i);
    std::vector<MdMessage> ins{md_begin(s), md_end(s + 1)};
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), ins.begin(), ins.end());
}

inline void mut_nest_begin(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    const std::size_t b = find_kind(v, MdKind::SnapshotBegin, 0);
    if (b == v.size()) return;
    const std::size_t e = find_kind(v, MdKind::SnapshotEnd, b);
    if (e == v.size() || e <= b + 1) return;
    const std::size_t at = b + 1 + pick_index(rng, e - b - 1); // strictly inside
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), md_begin(near_seq(v, at)));
}

inline void mut_double_end(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.empty()) return;
    const std::size_t i = pick_index(rng, v.size());
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(i + 1), md_end(near_seq(v, i)));
}

inline void mut_stale_begin(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    if (v.size() < 2) return;
    // Past position 0 on purpose: at index 0 the injected Begin(1) would merely
    // duplicate the real opening Begin(1), whereas one message later the cursor
    // is already past it and the freshness gate is what decides the outcome.
    const std::size_t i = 1 + pick_index(rng, v.size() - 1);
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), md_begin(1)); // below the cursor
}

// The first Level that arrives while the pipeline is LIVE, i.e. the first one
// after the opening bracket closes, or npos.
//
// This matters more than it looks. `find_kind(v, Level, 0)` returns a level
// INSIDE the opening snapshot, and a negative qty or an out-of-domain price
// there is refused by `validate_snapshot` as snapshot content — a completely
// different code path from the same defect arriving as a live incremental.
// Targeting the first level would leave `malformed` and `out_of_range`
// permanently at zero while appearing to cover both.
inline std::size_t first_live_level(const std::vector<MdMessage>& v) {
    const std::size_t end = find_kind(v, MdKind::SnapshotEnd, 0);
    if (end == v.size()) return v.size(); // never went Live
    return find_kind(v, MdKind::Level, end + 1);
}

inline void mut_negative_qty(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    const std::size_t i = first_live_level(v);
    if (i == v.size()) return;
    (void)rng;
    v[i].qty = -1;
}

inline void mut_out_of_domain_price(MdTrace& t, Rng& rng, const MdGenConfig& cfg) {
    auto& v = t.messages;
    const std::size_t i = first_live_level(v);
    if (i == v.size()) return;
    (void)rng;
    v[i].price = cfg.tick_max + 1;
}

inline void mut_duplicate_price_in_snapshot(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    const std::size_t b = find_kind(v, MdKind::SnapshotBegin, 0);
    if (b == v.size()) return;
    const std::size_t e = find_kind(v, MdKind::SnapshotEnd, b);
    const std::size_t lvl = find_kind(v, MdKind::Level, b);
    if (lvl == v.size() || lvl >= e) return;
    (void)rng;
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(lvl + 1), v[lvl]); // same price twice
}

inline void mut_zero_width_end(MdTrace& t, Rng& rng, const MdGenConfig&) {
    auto& v = t.messages;
    const std::size_t b = find_kind(v, MdKind::SnapshotBegin, 0);
    if (b == v.size()) return;
    (void)rng;
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(b + 1), md_end(v[b].seq)); // End == Begin
}

} // namespace detail

inline void apply_mutation(MdTrace& t, MdMutation m, Rng& rng, const MdGenConfig& cfg) {
    switch (m) {
        case MdMutation::DropSlice:                detail::mut_drop_slice(t, rng, cfg); break;
        case MdMutation::DropOne:                  detail::mut_drop_one(t, rng, cfg); break;
        case MdMutation::DuplicateOne:             detail::mut_duplicate_one(t, rng, cfg); break;
        case MdMutation::ReorderAdjacent:          detail::mut_reorder_adjacent(t, rng, cfg); break;
        case MdMutation::Renumber:                 detail::mut_renumber(t, rng, cfg); break;
        case MdMutation::InjectSnapshot:           detail::mut_inject_snapshot(t, rng, cfg); break;
        case MdMutation::TruncateMidSnapshot:      detail::mut_truncate_mid_snapshot(t, rng, cfg); break;
        case MdMutation::EmptySnapshot:            detail::mut_empty_snapshot(t, rng, cfg); break;
        case MdMutation::NestBegin:                detail::mut_nest_begin(t, rng, cfg); break;
        case MdMutation::DoubleEnd:                detail::mut_double_end(t, rng, cfg); break;
        case MdMutation::StaleBegin:               detail::mut_stale_begin(t, rng, cfg); break;
        case MdMutation::NegativeQty:              detail::mut_negative_qty(t, rng, cfg); break;
        case MdMutation::OutOfDomainPrice:         detail::mut_out_of_domain_price(t, rng, cfg); break;
        case MdMutation::DuplicatePriceInSnapshot: detail::mut_duplicate_price_in_snapshot(t, rng, cfg); break;
        case MdMutation::ZeroWidthEnd:             detail::mut_zero_width_end(t, rng, cfg); break;
    }
}

// The k-th catalogue entry, in a fixed order, so the grammar's choices are
// reproducible from the seed alone.
inline MdMutation mutation_at(int k) noexcept {
    return static_cast<MdMutation>(k % kMutationCount);
}

// A base scenario plus `cfg.mutations` mutations chosen by the seed.
inline MdTrace build(MdScenario s, const MdGenConfig& cfg, std::uint64_t seed) {
    MdTrace t = build_base(s, cfg, seed);
    Rng rng(seed ^ 0x9E3779B97F4A7C15ULL); // a stream distinct from the base's
    for (std::uint32_t i = 0; i < cfg.mutations; ++i) {
        const MdMutation m =
            mutation_at(static_cast<int>(uniform_below(rng, static_cast<std::uint64_t>(kMutationCount))));
        apply_mutation(t, m, rng, cfg);
        t.recipe.push_back(m);
    }
    return t;
}

// A base scenario plus an explicit recipe — how a failure found by the grammar
// is frozen into a named, reproducible regression.
inline MdTrace build_with_recipe(MdScenario s, std::initializer_list<MdMutation> recipe,
                                 const MdGenConfig& cfg, std::uint64_t seed) {
    MdTrace t = build_base(s, cfg, seed);
    Rng rng(seed ^ 0x9E3779B97F4A7C15ULL);
    for (MdMutation m : recipe) {
        apply_mutation(t, m, rng, cfg);
        t.recipe.push_back(m);
    }
    return t;
}

} // namespace llmd::gen
