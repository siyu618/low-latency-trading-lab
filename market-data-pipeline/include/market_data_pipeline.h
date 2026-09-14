#pragma once

#include "md_message.h"
#include "types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// MarketDataPipeline — the Experiment 03 sequencer.
//
// WHY THIS EXISTS. Experiments 01 and 02 built a correct order book and a
// correct SPSC transport. Neither answers the question a real feed handler
// faces: what does a consumer do when the SEQUENCED stream it is reading is
// lossy? A book that has silently missed a message is not a slightly stale
// book, it is a WRONG book, and the only recovery is a full state replace.
//
// This class owns the whole of that policy. It consumes MdMessage values and
// maintains:
//
//   * which messages are in order, which are replays and which skipped ahead;
//   * an in-progress snapshot assembly, published atomically at SnapshotEnd;
//   * the outage accounting: how long the book was unusable and what it cost.
//
// THE SEAM. The pipeline owns FRAMING and SEQUENCING. The book owns SNAPSHOT
// VALIDITY and APPLICATION. Snapshot content accumulates into a staging
// BookSnapshot; at SnapshotEnd the pipeline hands that to the sink's own
// `load_snapshot()`, so `validate_snapshot()` governs exactly as it does for
// every other snapshot path in this repository. There is ONE snapshot contract
// here, not two.
//
// The rejected alternative — staging by `apply()`-ing each level into a fresh
// empty book — would give snapshots a different validity rule from every other
// consumer: a repeated price would be silently overwritten instead of refused,
// and an out-of-domain price silently consumed. It would also cost a second
// full-size book and could not be swapped in atomically.
//
// IN-ORDER LEVELS ARE DELEGATED TO THE SINK. The pipeline never decides by hand
// whether an in-order level is good: it calls `book_.apply()` and maps the
// result. That is not a convenience. Two of the book's five outcomes are
// invisible to a sequence comparison alone —
//
//   * `InvalidUpdate` (`qty < 0`) makes the book UNSYNC ITSELF;
//   * `OutOfRange` CONSUMES the sequence and leaves the book synced.
//
// — so a pipeline that only compared sequence numbers would report `Live`
// forever over an unsynced book. Delegating also means the book is invalidated
// by the book's own code path rather than by a second mechanism the pipeline
// would have to invent; no book in this repository exposes an `invalidate()`.
//
// WHAT THIS CLASS DOES NOT DO. It does not request a resync — a real handler
// would ask the venue for a fresh snapshot when it detects a gap, and this one
// waits for one. It does not time anything: no clock is read anywhere in this
// phase, and the outage accounting is in MESSAGES and SEQUENCE NUMBERS, never
// in nanoseconds. It does not reorder or buffer: a message that arrives out of
// order is stale or a gap, never held for a later arrival. It does not validate
// that a snapshot is best-first or uncrossed — `validate_snapshot()` checks
// duplicates, negatives and the price domain, and ordering is not part of the
// contract it enforces.
// ---------------------------------------------------------------------------

namespace llmd {

// ---------------------------------------------------------------------------
// States.
//
//   NotSynced  no usable view has EVER been established. The initial state.
//   Live       the book is in step with the stream; in-order levels apply.
//   Snapshot   a bracketed full-state run is being assembled into staging.
//   Gap        a view WAS established and has been lost. Only a snapshot that
//              commits can restore one.
//
// NotSynced is kept distinct from Gap because the two cost different things.
// A cold start that never syncs has an outage of unknown length; losing a live
// view has a measurable one, and only the latter opens a recovery episode.
// The transition Gap -> NotSynced is impossible.
// ---------------------------------------------------------------------------
enum class MdState : std::uint8_t { NotSynced, Live, Snapshot, Gap };

inline const char* md_state_name(MdState s) noexcept {
    switch (s) {
        case MdState::NotSynced: return "NotSynced";
        case MdState::Live:      return "Live";
        case MdState::Snapshot:  return "Snapshot";
        case MdState::Gap:       return "Gap";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Per-message outcome. Exactly ONE outcome per message, always.
//
// This is deliberately richer than the book's `ApplyResult`, which conflates
// "you sent me a duplicate" with "my view is unusable" into a single `Stale`
// (see types.h). A feed handler must tell those apart, so the pipeline splits
// them and adds the framing outcomes the book has no opinion about.
//
//   Applied            in-order level, applied to the live book
//   OutOfRange         in-order level outside the book's price domain: the
//                      sequence IS consumed, the book stays synced, we stay Live
//   Staged             bracket opened, or level absorbed into the open snapshot
//   SnapshotCommitted  SnapshotEnd published a valid snapshot; now Live
//   SnapshotAbandoned  an open snapshot was discarded (jump inside it, nested
//                      bracket, malformed content, staging cap)
//   Stale              seq is behind the relevant cursor: replay or duplicate
//   GapDetected        forward jump while Live; the view is lost
//   Malformed          in-order level the book refused (`qty < 0`); view lost
//   Rejected           level arriving while there is no view to apply it to
//   ProtocolViolation  SnapshotEnd with no bracketed run open
//
// `Stale` here is unambiguous, unlike the book's: the pipeline only consults
// the book while it believes it is Live, so a stale result can only mean the
// message was behind the cursor.
// ---------------------------------------------------------------------------
enum class MdOutcome : std::uint8_t {
    Applied,
    OutOfRange,
    Staged,
    SnapshotCommitted,
    SnapshotAbandoned,
    Stale,
    GapDetected,
    Malformed,
    Rejected,
    ProtocolViolation,
};

inline const char* md_outcome_name(MdOutcome o) noexcept {
    switch (o) {
        case MdOutcome::Applied:           return "Applied";
        case MdOutcome::OutOfRange:        return "OutOfRange";
        case MdOutcome::Staged:            return "Staged";
        case MdOutcome::SnapshotCommitted: return "SnapshotCommitted";
        case MdOutcome::SnapshotAbandoned: return "SnapshotAbandoned";
        case MdOutcome::Stale:             return "Stale";
        case MdOutcome::GapDetected:       return "GapDetected";
        case MdOutcome::Malformed:         return "Malformed";
        case MdOutcome::Rejected:          return "Rejected";
        case MdOutcome::ProtocolViolation: return "ProtocolViolation";
    }
    return "?";
}

// Why a view was lost. Recorded once per episode, at the moment it opens.
enum class LossCause : std::uint8_t {
    SeqJumpLive,           // in-order stream skipped ahead while Live
    SeqJumpInSnapshot,     // the bracket's own run skipped ahead
    MalformedContentLive,  // in-order level with qty < 0 while Live
    MalformedSnapshot,     // the assembled snapshot failed validation
};

inline const char* loss_cause_name(LossCause c) noexcept {
    switch (c) {
        case LossCause::SeqJumpLive:          return "SeqJumpLive";
        case LossCause::SeqJumpInSnapshot:    return "SeqJumpInSnapshot";
        case LossCause::MalformedContentLive: return "MalformedContentLive";
        case LossCause::MalformedSnapshot:    return "MalformedSnapshot";
    }
    return "?";
}

// What one message did. `expected_after` is the pipeline's own view of the next
// sequence it needs, read AFTER the message was processed.
struct MdResult {
    MdOutcome     outcome;
    MdState       state_after;
    std::uint64_t expected_after;
};

// ---------------------------------------------------------------------------
// Counters.
//
// The first block partitions the message stream: `messages` equals the sum of
// the ten outcome counters, always. That identity is asserted by the tests, and
// it is what makes the outcome vector a complete account of a trace rather than
// a sample of it.
//
// The second block is diagnostics — things worth knowing that are not an
// outcome of any single message.
// ---------------------------------------------------------------------------
struct MdCounters {
    std::uint64_t messages = 0;

    std::uint64_t applied = 0;
    std::uint64_t out_of_range = 0;
    std::uint64_t staged = 0;
    std::uint64_t snapshot_committed = 0;
    std::uint64_t snapshot_abandoned = 0;
    std::uint64_t stale = 0;
    std::uint64_t gap_detected = 0;
    std::uint64_t malformed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t protocol_violations = 0;

    std::uint64_t sync_attempts = 0;          // SnapshotBegins accepted
    std::uint64_t stale_snapshot_begins = 0;  // refused by the freshness gate
    std::uint64_t malformed_snapshots = 0;    // load_snapshot refused the staging
    std::uint64_t empty_snapshots_committed = 0;
    std::uint64_t failed_initial_syncs = 0;   // cold-start brackets that never committed
    std::uint64_t staging_limit_hits = 0;
    // Brackets thrown away by a nested SnapshotBegin. A DIAGNOSTIC, not an
    // outcome: the Begin that caused the discard is one message with one
    // outcome (`Staged`), so counting the discarded bracket under
    // `snapshot_abandoned` would make the identity above overshoot by one for
    // every nested bracket in the trace.
    std::uint64_t nested_brackets_discarded = 0;
    std::uint64_t outages = 0;                // episodes opened
    std::uint64_t recovered_outages = 0;      // episodes closed by a commit
};

// ---------------------------------------------------------------------------
// One outage.
//
// `max_seq_seen` is the highest sequence observed while the episode was open,
// so `max_seq_seen - first_missing_seq + 1` is how far the stream ran while the
// book was unusable. `discarded` is what that cost in messages the pipeline
// could not use: levels Rejected while out of sync, plus staged levels thrown
// away when an attempted repair was abandoned.
//
// A stream that ends mid-outage leaves the episode OPEN, and an open episode is
// NOT copied into `episodes()`. A truncation test must be able to see
// `recovered == false`, and it could not if the episode were only visible after
// being closed — which never happens.
// ---------------------------------------------------------------------------
struct MdRecoveryEpisode {
    LossCause     cause = LossCause::SeqJumpLive;
    // The first sequence the BOOK can no longer be advanced to — its cursor
    // plus one at the moment the outage opened. It is read from the book, not
    // from the bracket that happened to fail: a mid-stream refresh bracket
    // carries its own cursor, which runs AHEAD of the book's, and reporting
    // that one would understate the outage by every sequence between the two.
    std::uint64_t first_missing_seq = 0;
    std::uint64_t gap_detect_seq = 0;     // the seq that revealed the loss
    std::uint64_t lost_span = 0;          // gap_detect_seq - first_missing_seq
    std::uint64_t max_seq_seen = 0;       // highest seq observed while open
    std::uint64_t discarded = 0;          // unusable messages during the outage
    // Brackets ACCEPTED while this outage was open. The bracket whose failure
    // opened the outage was accepted before it existed and is not counted here;
    // an outage opened by a failed refresh therefore reports 0 until some later
    // bracket is accepted — and `recovery_begin_seq` is 0 for the same reason.
    std::uint32_t attempts = 0;
    std::uint64_t recovery_begin_seq = 0; // Begin of the most recent attempt
    std::uint64_t recovery_end_seq = 0;   // the committing End's seq
    bool          recovered = false;
};

// ---------------------------------------------------------------------------
// MarketDataPipeline<Book>
//
// The pipeline OWNS its sink. Phase 2 will want ownership anyway, and it makes
// the Map-vs-Flat agreement harness in the tests a pair of plain objects.
//
// `book()` is const-only on purpose: handing out a mutable reference would let
// a caller call `load_snapshot()` directly and desynchronise the sequence the
// pipeline believes in from the sequence the book holds.
//
// PRECONDITION: sequence numbers are strictly positive and below UINT64_MAX.
// `snap_expected_ = seq + 1` and `lost_span = observed - first_missing` both
// wrap at the top of the range. Defending against that would cost a branch on
// every message to rule out a case no feed produces, so it is stated instead:
// a stream that reaches UINT64_MAX is outside this experiment's contract.
// ---------------------------------------------------------------------------
template <class Book>
class MarketDataPipeline {
public:
    struct Config {
        // Staging is bounded. A bracket that never closes is a live possibility
        // on any stream, and without a cap it is an unbounded allocation driven
        // by remote input. Exceeding the cap abandons the bracket; it never
        // truncates one, because a truncated snapshot is a wrong snapshot.
        std::size_t max_staged_levels_per_side = std::size_t{1} << 20;
    };

    explicit MarketDataPipeline(Book book = Book{}, Config cfg = {})
        : book_(std::move(book)), cfg_(cfg) {}

    MarketDataPipeline(const MarketDataPipeline&) = delete;
    MarketDataPipeline& operator=(const MarketDataPipeline&) = delete;
    MarketDataPipeline(MarketDataPipeline&&) = default;
    MarketDataPipeline& operator=(MarketDataPipeline&&) = default;

    // ---- The one entry point ----------------------------------------------

    MdResult apply(const MdMessage& m) {
        ++counters_.messages;
        if (open_episode_ && m.seq > open_episode_->max_seq_seen) {
            open_episode_->max_seq_seen = m.seq;
        }
        switch (m.kind) {
            case MdKind::SnapshotBegin: return on_begin(m);
            case MdKind::SnapshotEnd:   return on_end(m);
            case MdKind::Level:         return on_level(m);
        }
        return note(MdOutcome::Rejected); // unreachable: MdKind is exhaustive
    }

    // ---- Sequencing view ---------------------------------------------------

    MdState state() const noexcept { return state_; }
    bool    live() const noexcept { return state_ == MdState::Live; }

    // The next sequence the pipeline requires.
    //
    // Derived, never stored — while a bracket is open it is the bracket's own
    // cursor, and otherwise it is the BOOK's. There is exactly one sequence
    // counter in this class and it belongs to the book, so the pipeline cannot
    // drift from its sink. In Gap or NotSynced this is "the sequence we still
    // need", which is what an operator would ask for.
    std::uint64_t expected() const noexcept {
        return state_ == MdState::Snapshot ? snap_expected_ : book_.next_expected_seq();
    }

    // The last sequence the book holds state for.
    std::uint64_t cursor() const noexcept { return book_.last_applied_seq(); }

    // ---- Snapshot assembly view -------------------------------------------

    bool        snapshot_in_progress() const noexcept { return state_ == MdState::Snapshot; }
    std::size_t staged_levels(llob::Side s) const noexcept {
        return llob::is_bid(s) ? staging_.bids.prices.size() : staging_.asks.prices.size();
    }
    std::size_t staged_level_count() const noexcept {
        return staging_.bids.prices.size() + staging_.asks.prices.size();
    }

    // ---- Book view ---------------------------------------------------------

    const Book& book() const noexcept { return book_; }

    // ---- Accounting --------------------------------------------------------

    const MdCounters& counters() const noexcept { return counters_; }
    const std::vector<MdRecoveryEpisode>& episodes() const noexcept { return episodes_; }

    // The in-flight outage, or nullptr when no outage is open.
    //
    // Non-null whenever a view is lost and has not been restored — which
    // includes the interval spent assembling a repair bracket, where
    // state() == MdState::Snapshot. It is NOT "non-null iff Gap": that reading
    // would hide the outage for exactly as long as the repair is in flight.
    const MdRecoveryEpisode* open_episode() const noexcept {
        return open_episode_ ? &*open_episode_ : nullptr;
    }

private:
    // EVERY result this class returns is built here, and this is the ONLY place
    // an outcome counter is incremented. That is what makes
    //     messages == applied + out_of_range + staged + ... + protocol_violations
    // hold by construction rather than by remembering to increment at each of
    // the thirteen points that return a result.
    //
    // The scattered form was tried first and was wrong: `SnapshotBegin` returned
    // `Staged` without counting it, and three separate stale paths returned
    // `Stale` without counting it, so the identity was off by exactly the number
    // of brackets and stale frames in a trace. A differential test could not see
    // it — the reference implementation had been written from this one and
    // inherited the same omission, which is precisely the failure mode that hand
    // written vectors and this identity exist to catch.
    //
    // The switch is exhaustive over MdOutcome, so a new outcome will not compile
    // until it is counted here.
    //
    // Diagnostics are NOT counted here: `sync_attempts`, `staging_limit_hits`
    // and friends are not the outcome of any single message, and several of them
    // must be incremented in addition to (or instead of) an outcome.
    MdResult note(MdOutcome o) noexcept {
        switch (o) {
            case MdOutcome::Applied:           ++counters_.applied; break;
            case MdOutcome::OutOfRange:        ++counters_.out_of_range; break;
            case MdOutcome::Staged:            ++counters_.staged; break;
            case MdOutcome::SnapshotCommitted: ++counters_.snapshot_committed; break;
            case MdOutcome::SnapshotAbandoned: ++counters_.snapshot_abandoned; break;
            case MdOutcome::Stale:             ++counters_.stale; break;
            case MdOutcome::GapDetected:       ++counters_.gap_detected; break;
            case MdOutcome::Malformed:         ++counters_.malformed; break;
            case MdOutcome::Rejected:          ++counters_.rejected; break;
            case MdOutcome::ProtocolViolation: ++counters_.protocol_violations; break;
        }
        return MdResult{o, state_, expected()};
    }

    // A snapshot is FRESH only if it is not behind the watermark — the next
    // sequence the pipeline requires. This is the pipeline's own guard, not the
    // book's: `load_snapshot()` has no staleness check and will happily REWIND a
    // book to an older sequence. Committing a stale bracket would move
    // `last_applied` backwards and then double-apply every message between the
    // two positions, with no gap firing to reveal it.
    //
    // The comparison is against `expected()`, NOT `book_.last_applied_seq()`.
    // While Live those two differ by exactly one, and the difference is the
    // whole point: `last_applied_seq()` is the last sequence already CONSUMED,
    // so gating on it accepts a `SnapshotBegin` numbered at a sequence we have
    // already used. That is a frame behind the watermark, and a frame behind the
    // watermark is stale whatever its kind.
    //
    // Because `expected()` is derived, this is one rule with one meaning in
    // every state: while a bracket is open it is the bracket's own cursor, and
    // otherwise it is the book's. So a nested Begin behind the run already in
    // progress is stale too, and the run survives it.
    //
    // Applied in EVERY state, including Gap. A repair bracket older than the
    // view we lost is not a repair: it would restore an old book and then
    // replay old messages over it.
    bool snapshot_fresh(std::uint64_t seq) const noexcept {
        return seq >= expected();
    }

    void reset_staging() {
        staging_.bids.prices.clear();
        staging_.bids.qtys.clear();
        staging_.asks.prices.clear();
        staging_.asks.qtys.clear();
        staging_.seq = 0;
        snap_expected_ = 0;
    }

    // Open a bracket. Caller has already cleared whatever it needs to.
    void start_snapshot(std::uint64_t begin_seq) {
        reset_staging();
        snap_expected_ = begin_seq + 1;
        state_ = MdState::Snapshot;
        ++counters_.sync_attempts;
    }

    void open_episode(LossCause cause, std::uint64_t first_missing,
                      std::uint64_t observed) {
        MdRecoveryEpisode e;
        e.cause = cause;
        e.first_missing_seq = first_missing;
        e.gap_detect_seq = observed;
        // Never negative, and never anything other than this difference: the
        // book's cursor cannot be ahead of the sequence that just jumped past
        // it, and content loss is detected on the in-order message itself.
        e.lost_span = observed > first_missing ? observed - first_missing : 0;
        e.max_seq_seen = observed;
        open_episode_ = e;
        ++counters_.outages;
    }

    void close_episode(std::uint64_t end_seq) {
        if (!open_episode_) return;
        open_episode_->recovered = true;
        open_episode_->recovery_end_seq = end_seq;
        episodes_.push_back(*open_episode_);
        open_episode_.reset();
        ++counters_.recovered_outages;
    }

    // The view is gone. `observed` is the sequence that revealed it.
    //
    // `first_missing` is NOT a parameter. It is always the book's own
    // `next_expected_seq()` at this instant, and deriving it here rather than
    // at each call site means no caller can pass the bracket's cursor by
    // mistake. Nothing between a loss and this call moves the book's cursor:
    // `apply()` leaves the sequence unconsumed on both GapDetected and
    // InvalidUpdate, and staging never touches the book at all.
    //
    // An outage that is already open is never reopened — it keeps the position
    // and cause of the loss that opened it, and only accumulates state.
    void lose_view(LossCause cause, std::uint64_t observed) {
        if (!ever_synced_) {
            state_ = MdState::NotSynced;
            ++counters_.failed_initial_syncs;
            return;
        }
        state_ = MdState::Gap;
        if (!open_episode_) {
            open_episode(cause, book_.next_expected_seq(), observed);
        }
        // No attempt is counted here. Every path that reaches this with an
        // outage already open came from abandoning a bracket that `on_begin`
        // had already counted, so counting again would report more brackets
        // than the stream ever sent.
    }

    MdResult abandon_snapshot(LossCause cause, std::uint64_t observed) {
        // The staged levels of the run being thrown away are part of what the
        // outage cost. Charged AFTER lose_view so they land on whichever
        // episode is open then — an outage that was already running, or the one
        // this very abandonment opens (a failed mid-stream refresh).
        const std::size_t staged = staged_level_count();
        reset_staging();
        lose_view(cause, observed);
        if (open_episode_) open_episode_->discarded += staged;
        return note(MdOutcome::SnapshotAbandoned);
    }

    // ---- Bracket open ------------------------------------------------------

    MdResult on_begin(const MdMessage& m) {
        if (!snapshot_fresh(m.seq)) {
            ++counters_.stale_snapshot_begins;
            return note(MdOutcome::Stale);
        }
        if (state_ == MdState::Snapshot) {
            // A nested bracket abandons the one in progress. The message's own
            // outcome is `Staged` — one message, one outcome — and `note()`
            // counts that. The abandoned bracket is counted SEPARATELY here,
            // because it is a second real event: two brackets existed and one
            // was thrown away. Its levels are charged to the outage if one is
            // open.
            //
            // Counted as a DIAGNOSTIC, not as an outcome: the two brackets are
            // one message's worth of stream, and `note()` has already counted
            // this message once.
            ++counters_.nested_brackets_discarded;
            if (open_episode_) {
                open_episode_->discarded += staged_level_count();
                ++open_episode_->attempts;
            }
        } else if (open_episode_) {
            // A repair attempt during an open outage. `recovery_begin_seq`
            // records the most recent attempt's Begin; a later attempt that
            // commits overwrites it, so the pair always describes one bracket.
            ++open_episode_->attempts;
            open_episode_->recovery_begin_seq = m.seq;
        }
        start_snapshot(m.seq);
        return note(MdOutcome::Staged);
    }

    // ---- Bracket close -----------------------------------------------------

    MdResult on_end(const MdMessage& m) {
        if (state_ != MdState::Snapshot) {
            // A close with nothing open. Note what this does NOT do: it does
            // not invalidate a healthy book. The level stream may be perfectly
            // fine, and discarding a synced view over one stray message is a
            // self-inflicted outage.
            return note(MdOutcome::ProtocolViolation);
        }
        if (m.seq < snap_expected_) {
            return note(MdOutcome::Stale); // includes a zero-width bracket
        }
        if (m.seq > snap_expected_) {
            return abandon_snapshot(LossCause::SeqJumpInSnapshot, m.seq);
        }

        // In order: publish. `staging_.seq` is the sequence the book will carry,
        // and setting it BEFORE load_snapshot is what keeps the book's own
        // next_expected_seq() in lockstep with the pipeline for free.
        staging_.seq = m.seq;
        if (!book_.load_snapshot(staging_)) {
            // validate_snapshot refused the assembled run: a repeated price, a
            // negative qty or a price outside the domain. The book is left
            // completely unchanged, which is load_snapshot's own guarantee.
            ++counters_.malformed_snapshots;
            return abandon_snapshot(LossCause::MalformedSnapshot, m.seq);
        }

        if (staging_.bids.prices.empty() && staging_.asks.prices.empty()) {
            // A legal and meaningful snapshot: the instrument is empty. The
            // book is now SYNCED AND EMPTY, which is not the same as unsynced.
            ++counters_.empty_snapshots_committed;
        }
        reset_staging();
        state_ = MdState::Live;
        ever_synced_ = true;
        close_episode(m.seq);
        return note(MdOutcome::SnapshotCommitted);
    }

    // ---- Levels ------------------------------------------------------------

    MdResult on_level(const MdMessage& m) {
        switch (state_) {
            case MdState::Snapshot: return on_level_staging(m);
            case MdState::Live:     return on_level_live(m);
            case MdState::NotSynced:
            case MdState::Gap:
                break;
        }
        // No view to apply it to. The message is not queued and not replayed
        // later: a level that arrives out of sync is gone.
        if (open_episode_) ++open_episode_->discarded;
        return note(MdOutcome::Rejected);
    }

    MdResult on_level_staging(const MdMessage& m) {
        if (m.seq < snap_expected_) {
            return note(MdOutcome::Stale); // staging survives a duplicate
        }
        if (m.seq > snap_expected_) {
            return abandon_snapshot(LossCause::SeqJumpInSnapshot, m.seq);
        }
        if (staged_levels(m.side) >= cfg_.max_staged_levels_per_side) {
            ++counters_.staging_limit_hits;
            return abandon_snapshot(LossCause::MalformedSnapshot, m.seq);
        }

        auto& side = llob::is_bid(m.side) ? staging_.bids : staging_.asks;
        side.prices.push_back(m.price);
        side.qtys.push_back(m.qty);
        ++snap_expected_;
        return note(MdOutcome::Staged);
    }

    MdResult on_level_live(const MdMessage& m) {
        // Ask the book, do not second-guess it. No sequence is read before the
        // call: both loss outcomes leave the book's cursor exactly where it
        // was, so `lose_view` can read it afterwards and get the same answer.
        const llob::ApplyResult r =
            book_.apply(llob::L2Update{m.seq, m.price, m.qty, m.side});

        switch (r) {
            case llob::ApplyResult::Applied:
                return note(MdOutcome::Applied);

            case llob::ApplyResult::OutOfRange:
                // Consumed, not stored, and the book stays synced: a banded book
                // deliberately does not cover every price. Not a loss.
                return note(MdOutcome::OutOfRange);

            case llob::ApplyResult::Stale:
                // Only reachable as a genuine replay: we are Live, so the book
                // is synced, and apply()'s first rule caught seq <= cursor.
                return note(MdOutcome::Stale);

            case llob::ApplyResult::GapDetected:
                // apply() has already set the book unsynced WITHOUT consuming
                // the sequence, so the missed range is never silently skipped.
                lose_view(LossCause::SeqJumpLive, m.seq);
                return note(MdOutcome::GapDetected);

            case llob::ApplyResult::InvalidUpdate:
                lose_view(LossCause::MalformedContentLive, m.seq);
                return note(MdOutcome::Malformed);
        }
        return note(MdOutcome::Rejected); // unreachable: ApplyResult is exhaustive
    }

    Book     book_;
    Config   cfg_{};
    MdState  state_ = MdState::NotSynced;
    bool     ever_synced_ = false;

    // Meaningful only while state_ == MdState::Snapshot. This is the ONLY
    // sequence counter the pipeline owns; every other sequence question is
    // answered by the book.
    std::uint64_t snap_expected_ = 0;

    llob::BookSnapshot staging_;
    MdCounters         counters_;
    std::vector<MdRecoveryEpisode> episodes_;
    std::optional<MdRecoveryEpisode> open_episode_;
};

} // namespace llmd
