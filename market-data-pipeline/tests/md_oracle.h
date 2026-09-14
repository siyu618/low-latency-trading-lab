#pragma once

#include "market_data_pipeline.h"
#include "md_message.h"
#include "types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// An INDEPENDENT reference implementation of the Experiment 03 state machine.
//
// The point of this file is that it shares no code with the pipeline. It shares
// the VOCABULARY — `MdOutcome`, `MdState`, `MdCounters`, `MdRecoveryEpisode` are
// the things being talked about, and re-declaring them would only make the two
// implementations disagree about spelling. It shares no LOGIC:
//
//   * a different book, built from a flat vector of levels with linear scans,
//     rather than the repository's map or flat books;
//   * staging as one interleaved list of (side, price, qty), rather than the
//     per-side parallel arrays of `BookSnapshot`;
//   * snapshot validity re-derived by hand from the documented rules, rather
//     than delegated to `validate_snapshot()`;
//   * the apply() precedence chain written out inline in the order types.h
//     states it, rather than reached through the book's own `apply()`;
//   * one flat `if`/`else if` chain, rather than the pipeline's dispatch to
//     per-kind helpers.
//
// A differential test against this catches IMPLEMENTATION DIVERGENCE: the
// pipeline disagreeing with an independently written reading of the same
// contract. It cannot catch a SPECIFICATION ERROR — if both implementations
// misread the contract they will agree and be wrong together. That is what the
// hand-written scenario vectors are for, and the doc says so rather than
// implying the fuzz corpus proves more than it does.
//
// Deliberately NOT fast. Linear scans, no caching, no reservations. Speed here
// would be a sign it had started sharing the pipeline's assumptions.
// ---------------------------------------------------------------------------

namespace llmd::oracle {

// ---------------------------------------------------------------------------
// A book, from first principles.
//
// Levels are kept as an unordered pair of vectors and every query scans them.
// `last_applied == 0` means "no sequence consumed yet", which is why a fresh
// book reports `next_expected() == 1`.
// ---------------------------------------------------------------------------
struct NaiveBook {
    std::vector<std::pair<std::int64_t, std::int64_t>> bids{};
    std::vector<std::pair<std::int64_t, std::int64_t>> asks{};
    bool          synced = false;
    std::uint64_t last_applied = 0;
    std::int64_t  tick_min = llob::kDefaultTickMin;
    std::int64_t  tick_max = llob::kDefaultTickMax;

    std::uint64_t next_expected() const noexcept { return last_applied + 1; }

    static std::int64_t* find(std::vector<std::pair<std::int64_t, std::int64_t>>& v,
                              std::int64_t price) {
        for (auto& lv : v) {
            if (lv.first == price) return &lv.second;
        }
        return nullptr;
    }

    // apply()'s own effect on the level store: qty > 0 sets, qty == 0 removes,
    // removing an absent price is a no-op.
    void set_level(llob::Side side, std::int64_t price, std::int64_t qty) {
        auto& v = llob::is_bid(side) ? bids : asks;
        std::int64_t* slot = find(v, price);
        if (qty == 0) {
            if (slot != nullptr) {
                for (std::size_t i = 0; i < v.size(); ++i) {
                    if (v[i].first == price) {
                        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                }
            }
            return;
        }
        if (slot != nullptr) {
            *slot = qty;
        } else {
            v.emplace_back(price, qty);
        }
    }

    std::int64_t best_bid() const {
        std::int64_t best = 0;
        for (const auto& lv : bids) best = (best == 0 || lv.first > best) ? lv.first : best;
        return best;
    }
    std::int64_t best_ask() const {
        std::int64_t best = 0;
        for (const auto& lv : asks) best = (best == 0 || lv.first < best) ? lv.first : best;
        return best;
    }
    std::size_t level_count() const noexcept { return bids.size() + asks.size(); }

    // Sorted the way MapOrderBook iterates: bids best-first descending, asks
    // ascending. Used for full level-set parity against the real book.
    std::vector<std::pair<std::int64_t, std::int64_t>> bids_desc() const {
        auto v = bids;
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        return v;
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> asks_asc() const {
        auto v = asks;
        std::sort(v.begin(), v.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return v;
    }
};

struct OracleResult {
    MdOutcome     outcome;
    MdState       state_after;
    std::uint64_t expected_after;
};

struct OracleConfig {
    std::size_t max_staged_levels_per_side = std::size_t{1} << 20;
    std::int64_t tick_min = llob::kDefaultTickMin;
    std::int64_t tick_max = llob::kDefaultTickMax;
};

class Oracle {
public:
    explicit Oracle(OracleConfig cfg = {}) : cfg_(cfg) {
        book_.tick_min = cfg.tick_min;
        book_.tick_max = cfg.tick_max;
    }

    // ---- The whole state machine, in one place -----------------------------

    OracleResult apply(const MdMessage& m) {
        ++counters_.messages;
        if (open_ && m.seq > open_->max_seq_seen) open_->max_seq_seen = m.seq;

        if (m.kind == MdKind::SnapshotBegin) {
            // Freshness gate. `load_snapshot` has no staleness check of its own
            // and would happily rewind the book, so the gate has to live here.
            //
            // Against the WATERMARK — `expected()` — and not against the last
            // sequence consumed. Those differ by one while Live, and the
            // difference is the whole point: gating on the last consumed
            // sequence accepts a Begin numbered at a sequence already used,
            // which is a frame behind the watermark and stale whatever its kind.
            if (m.seq < expected()) {
                ++counters_.stale_snapshot_begins;
                return result(MdOutcome::Stale);
            }
            if (state_ == MdState::Snapshot) {
                // Two brackets, one message: the discard is a diagnostic, and
                // this message's own outcome (`Staged`) is counted by result().
                ++counters_.nested_brackets_discarded;
                if (open_) {
                    open_->discarded += staged_.size();
                    ++open_->attempts;
                }
            } else if (open_) {
                ++open_->attempts;
                open_->recovery_begin_seq = m.seq;
            }
            clear_staging();
            snap_expected_ = m.seq + 1;
            state_ = MdState::Snapshot;
            ++counters_.sync_attempts;
            return result(MdOutcome::Staged);
        }

        if (m.kind == MdKind::SnapshotEnd) {
            if (state_ != MdState::Snapshot) {
                return result(MdOutcome::ProtocolViolation);
            }
            if (m.seq < snap_expected_) return result(MdOutcome::Stale);
            if (m.seq > snap_expected_) return abandon(LossCause::SeqJumpInSnapshot, m.seq);

            if (!valid_staging()) {
                ++counters_.malformed_snapshots;
                return abandon(LossCause::MalformedSnapshot, m.seq);
            }
            // Commit. The staged levels REPLACE the book's state; qty == 0 means
            // "no level at this price" and stores nothing.
            book_.bids.clear();
            book_.asks.clear();
            for (const Staged& s : staged_) {
                if (s.qty != 0) book_.set_level(s.side, s.price, s.qty);
            }
            book_.last_applied = m.seq;
            book_.synced = true;

            if (staged_.empty()) ++counters_.empty_snapshots_committed;
            clear_staging();
            state_ = MdState::Live;
            ever_synced_ = true;
            if (open_) {
                open_->recovered = true;
                open_->recovery_end_seq = m.seq;
                episodes_.push_back(*open_);
                open_.reset();
                ++counters_.recovered_outages;
            }
            return result(MdOutcome::SnapshotCommitted);
        }

        // ---- A level -------------------------------------------------------
        if (state_ == MdState::Snapshot) {
            // A duplicate inside a bracket is Stale and the staging SURVIVES it:
            // there is nothing ambiguous about a repeat, we simply already have
            // it. Only a jump forward destroys the run.
            if (m.seq < snap_expected_) return result(MdOutcome::Stale);
            if (m.seq > snap_expected_) return abandon(LossCause::SeqJumpInSnapshot, m.seq);
            if (staged_per_side(m.side) >= cfg_.max_staged_levels_per_side) {
                ++counters_.staging_limit_hits;
                return abandon(LossCause::MalformedSnapshot, m.seq);
            }
            staged_.push_back(Staged{m.side, m.price, m.qty});
            ++snap_expected_;
            return result(MdOutcome::Staged);
        }

        if (state_ == MdState::Live) {
            // apply()'s precedence, in the order types.h states it.
            if (m.seq <= book_.last_applied) {
                return result(MdOutcome::Stale);
            }
            if (!book_.synced) {
                // Unreachable: nothing unsyncs the book without moving the
                // pipeline out of Live. Written out anyway so this chain is a
                // faithful reading of the contract rather than of the reachable
                // subset of it.
                return result(MdOutcome::Stale);
            }
            if (m.seq != book_.last_applied + 1) {
                book_.synced = false; // sequence NOT consumed
                lose(LossCause::SeqJumpLive, m.seq);
                return result(MdOutcome::GapDetected);
            }
            if (m.qty < 0) {
                book_.synced = false; // sequence NOT consumed
                lose(LossCause::MalformedContentLive, m.seq);
                return result(MdOutcome::Malformed);
            }
            if (m.price < book_.tick_min || m.price > book_.tick_max) {
                book_.last_applied = m.seq; // consumed, nothing stored, still synced
                return result(MdOutcome::OutOfRange);
            }
            book_.last_applied = m.seq;
            book_.set_level(m.side, m.price, m.qty);
            return result(MdOutcome::Applied);
        }

        // NotSynced or Gap: no view exists to apply this to. The message is not
        // buffered and never replayed.
        if (open_) ++open_->discarded;
        return result(MdOutcome::Rejected);
    }

    // ---- Views -------------------------------------------------------------

    MdState state() const noexcept { return state_; }
    bool    live() const noexcept { return state_ == MdState::Live; }
    std::uint64_t expected() const noexcept {
        return state_ == MdState::Snapshot ? snap_expected_ : book_.next_expected();
    }
    std::uint64_t cursor() const noexcept { return book_.last_applied; }
    bool snapshot_in_progress() const noexcept { return state_ == MdState::Snapshot; }
    std::size_t staged_level_count() const noexcept { return staged_.size(); }
    std::size_t staged_levels(llob::Side s) const noexcept { return staged_per_side(s); }

    const NaiveBook&      book() const noexcept { return book_; }
    const MdCounters&     counters() const noexcept { return counters_; }
    const std::vector<MdRecoveryEpisode>& episodes() const noexcept { return episodes_; }
    const MdRecoveryEpisode* open_episode() const noexcept {
        return open_ ? &*open_ : nullptr;
    }

private:
    struct Staged {
        llob::Side    side;
        std::int64_t  price;
        std::int64_t  qty;
    };

    // The only place an outcome counter moves. Every return in this file goes
    // through here, so `messages == sum of the ten outcome counters` holds by
    // construction rather than by remembering to increment on each path.
    OracleResult result(MdOutcome o) noexcept {
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
        return OracleResult{o, state_, expected()};
    }

    std::size_t staged_per_side(llob::Side s) const {
        std::size_t n = 0;
        for (const Staged& st : staged_) {
            if (st.side == s) ++n;
        }
        return n;
    }

    // `validate_snapshot`'s three rules, re-derived over the interleaved list:
    // no negative qty, every price inside the domain, and no price repeated on
    // the SAME side (the same price on opposite sides is ordinary). A repeated
    // price is ambiguous state and refuses the WHOLE run.
    //
    // The duplicate scan deliberately includes qty == 0 entries, matching
    // side_snapshot_valid: a price listed twice, once as a delete and once as a
    // level, is still listed twice.
    bool valid_staging() const {
        for (std::size_t i = 0; i < staged_.size(); ++i) {
            const Staged& a = staged_[i];
            if (a.qty < 0) return false;
            if (a.price < cfg_.tick_min || a.price > cfg_.tick_max) return false;
            for (std::size_t j = i + 1; j < staged_.size(); ++j) {
                if (staged_[j].side == a.side && staged_[j].price == a.price) return false;
            }
        }
        return true;
    }

    void clear_staging() {
        staged_.clear();
        snap_expected_ = 0;
    }

    OracleResult abandon(LossCause cause, std::uint64_t observed) {
        const std::size_t staged = staged_.size();
        clear_staging();
        lose(cause, observed);
        if (open_) open_->discarded += staged;
        return result(MdOutcome::SnapshotAbandoned);
    }

    // `first_missing` comes from the BOOK, never from the bracket: a refresh
    // bracket's cursor runs ahead of the book's, and the book is the thing whose
    // continuity was lost.
    void lose(LossCause cause, std::uint64_t observed) {
        if (!ever_synced_) {
            state_ = MdState::NotSynced;
            ++counters_.failed_initial_syncs;
            return;
        }
        state_ = MdState::Gap;
        if (!open_) {
            MdRecoveryEpisode e;
            e.cause = cause;
            e.first_missing_seq = book_.next_expected();
            e.gap_detect_seq = observed;
            e.lost_span = observed > e.first_missing_seq ? observed - e.first_missing_seq : 0;
            e.max_seq_seen = observed;
            open_ = e;
            ++counters_.outages;
        }
    }

    OracleConfig  cfg_{};
    NaiveBook     book_{};
    MdState       state_ = MdState::NotSynced;
    bool          ever_synced_ = false;
    std::uint64_t snap_expected_ = 0;
    std::vector<Staged> staged_{};
    MdCounters    counters_{};
    std::vector<MdRecoveryEpisode> episodes_{};
    std::optional<MdRecoveryEpisode> open_{};
};

} // namespace llmd::oracle
