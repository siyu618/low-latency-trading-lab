// Deterministic order-book benchmark (Experiment 01, Phase 2).
//
// What this measures: steady-state apply() throughput of ONE L2 book
// implementation over pre-recorded, well-formed update streams. It does NOT
// measure RNG cost, stream construction, book cold-start, or best-price reads —
// all of that happens before the timed region or is excluded by construction
// (the full update stream is generated up front, once, off the clock, by a
// fixed-seed generator).
//
// Methodology (deliberate — matches the Phase 2 contract):
//   * The whole stream for (impl, workload, scale) is generated BEFORE any
//     timer starts. The timed path does not allocate, and the generator is not
//     on the clock.
//   * Each timed block has two phases on a FRESH book:
//       - cold start: load_snapshot() of a full N-levels-per-side snapshot,
//                     untimed. (A fresh book is unsynced and rejects every
//                     apply() until a snapshot, so an incremental warm-up is
//                     impossible.) This is the state each measurement starts from.
//       - timed:      replay the pre-built steady op stream. The stream's
//                     sequence numbers continue exactly after the snapshot's
//                     (seq 2N), so every timed op is Applied (validated by
//                     --check).
//   * One block = (impl, workload, scale, rep). The identical op stream is
//     replayed for every rep of a given impl, so any time difference across
//     implementations is purely the book code. Reported time is the best (min)
//     of the reps blocks — a common low-latency summary that discounts
//     scheduling noise (which only ever adds latency).
//   * CANONICAL measurements run ONE implementation per process
//     (`orderbook_bench map …` / `orderbook_bench flat …`), never the two
//     interleaved in a shared address space. What per-process runs buy is clean
//     process/address-space isolation, no mixed implementation state, and clean
//     profiling/perf attribution (Phase 3). They do NOT buy thermal isolation:
//     thermal state and system-level load survive process exit, so back-to-back
//     long, CPU-saturating runs can still drift. The `both` mode (map then flat
//     in one process) exists only as a quick local sanity check and is NOT used
//     for reported results.
//   * OPTIONAL profiling gate (Phase 3.1): when the env var LLOB_PERF_CONTROL
//     is set to a named control fifo that perf is listening on (see the Phase 3
//     harness, scripts/perf-profile.sh), each timed block is bracketed by a perf
//     enable/ack handshake (before t0) and a disable/ack handshake (immediately
//     after t1, before the end-state reads), so perf stat counters cover exactly
//     the same apply() block that the wall clock times — not the untimed
//     cold-start. Perf's ack is the literal "ack\n" (tools/perf util/evlist.h).
//     The gate is a strict no-op when the env var is unset, so normal Phase 2
//     runs are byte-for-byte unchanged, and the reported best_ns_per_update is
//     unchanged in meaning (only the fifo handshake adds time, before t0 and
//     after t1, outside both the chrono and the PMU windows).
//   * OPTIONAL macOS signpost interval (Phase 3M): when the env var
//     LLOB_SIGNPOSTS=1 is set (Apple platforms only), the SAME timed block is
//     bracketed by os_signpost_interval_begin/end on a "llob.apply.block"
//     interval so macOS Instruments / the `log` CLI can show the timed apply()
//     region. The interval TIGHTLY BRACKETS the chrono-measured block: signpost
//     begin, a small fixed boundary cost, t0, apply x N, t1, a small fixed
//     boundary cost, signpost end. The marker lives OUTSIDE the per-update
//     loop, just outside t0/t1, and is compiled only on __APPLE__. The small
//     boundary overhead is NOT part of the chrono interval and is amortized over
//     the large update count; the profile must read the marker as the region of
//     interest, not as extra per-update work. When LLOB_SIGNPOSTS is unset (the
//     default) the interval is not emitted and normal runs are unchanged.
//   * The stream never contains a negative qty or an out-of-domain price, and
//     seq advances by exactly 1, so it is legal for both books.
//
// Price-domain model: domain is [1, 2N] where N is the requested scale in price
// levels. Each side starts with N live levels:
//   * bids occupy N+1 .. 2N   (best bid = 2N initially)
//   * asks occupy  1 .. N     (best ask = 1 initially)
// A candidate level `idx` on a side is idx steps from the touch (idx 0 == the
// best price), so both sides share one bookkeeping model. Workloads differ only
// in WHICH levels they touch and how often they delete the best. Live-level
// count over a run: A never changes the level set (stays exactly N); B/D
// conserve levels (each delete is later restored, so the count is N at every
// prefix where a restore has caught up); C conserves levels (refill rate >=
// delete rate, count returns to N). E does NOT conserve levels — it deletes and
// adds at random, so occupancy may drift below the starting N; the exact
// finite-run value depends on scale, update count, the RNG stream, and the
// generator's retry/fallback behavior, and is NOT hard-coded here (--check
// prints the actual ending level counts of the generated stream). Every
// workload keeps the side far from empty, so each cell measures steady state,
// never a draining book.
//
//   A  update-only               every op re-quantifies a random present level;
//                                the level set never changes (exactly N)
//   B  10% deletes               each op picks a side; with 10% probability it
//                                deletes a random present level, otherwise it
//                                adds at a random ABSENT level (restoring the
//                                one that was deleted); level count stays N
//   C  frequent best deletion    ~45% of ops delete the CURRENT best level
//                                (forcing the inward best re-scan); the rest
//                                refill the most recently vacated level, which
//                                restores it just below the current best. The
//                                best churns across a few adjacent prices while
//                                the level count stays exactly N
//   D  concentrated top-of-book  ops touch only a small window [0, N/128) at
//                                the best end; ~15% of ops delete a present
//                                window level, ~85% add at an absent window
//                                level (level count inside the window is
//                                conserved); levels below the window never move
//   E  uniformly random          fair side coin; price uniform over that side's
//                                whole region; 50% delete a present level, 50%
//                                add at an absent one; occupancy may drift
//                                below the starting N (not hard-coded)
//
// Usage:
//   orderbook_bench [impl] [workload] [scale] [updates=N] [reps=N] [--check]
//     impl      map | flat | both      (default both)
//     workload  A B C D E | all        (default all)
//     scale     1000 | 10000 | 100000 | 1000000 | all   (default all)
//     updates=N   steady ops per timed block            (default 2000000)
//     reps=N      timed blocks per cell; best is kept   (default 3)
//     --check     replay one stream through BOTH books and verify they agree;
//                 no timing
//
// Canonical / reported runs: ONE implementation per process, e.g.
//   ./orderbook_bench map all all      then separately
//   ./orderbook_bench flat all all
// `both` runs map then flat back-to-back in one process and is only a quick
// local sanity check: it shares one address space between the two designs and
// the second run follows a long CPU-saturating first run, so it is NOT used for
// reported numbers.
//
// Release config is set in CMakeLists: -O3 -DNDEBUG. CPU arch tuning is OPT-IN
// and free-form via -DBENCH_ARCH_FLAGS="<flags>" (e.g. -march=native,
// -mcpu=apple-m3); the canonical M3 numbers in the README were built WITHOUT
// any arch flag.

#include "flat_order_book.h"
#include "map_order_book.h"
#include "stream_gen.h"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>    // open
#include <poll.h>     // poll (wait for perf to open the fifo)
#include <sys/stat.h> // mkfifo
#include <unistd.h>   // read, write, close
#elif defined(__APPLE__)
#include <os/log.h>      // os_log_create (log-handle for signpost emission)
#include <os/signpost.h> // os_signpost_interval_begin/end, os_signpost_id_t,
                         //   OS_LOG_CATEGORY_POINTS_OF_INTEREST
#endif

using llob::ApplyResult;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::MapOrderBook;
using llob::Side;

// Workload vocabulary and the deterministic stream generator now live in
// stream_gen.h, the single source shared with the Phase 4 tail benchmark.
using llob_bench::Workload;
using llob_bench::StreamGen;
using llob_bench::kDefaultSeed;
using llob_bench::kWorkloadCount;
using llob_bench::workload_desc;
using llob_bench::workload_tag;

namespace {

constexpr uint64_t kDefaultUpdates = 2'000'000;
constexpr int      kDefaultReps    = 3;

constexpr int64_t kScaleLevels[4] = {1'000, 10'000, 100'000, 1'000'000};

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;

// Compiler memory barrier: an opaque empty asm at the level of the optimizer.
// In the timed loop it forces every apply() to be treated as an opaque
// memory-touching operation (its loads and stores cannot be reordered, CSE'd,
// or eliminated across the barrier). This matters because both books are
// header-only: without the barrier the compiler could legally prove parts of
// the flat book's stores dead and remove them, and could otherwise optimize
// more aggressively across apply() calls than a real caller in another
// translation unit would allow.
//
// Cost: it typically emits NO machine instruction (the asm body is empty), but
// that is not the same as being free — as an optimizer barrier it can prevent
// code motion across it and increase register pressure / spill, so it is part
// of the benchmark methodology, not a neutral no-op. It is deliberately kept
// IDENTICAL in the MapOrderBook and FlatOrderBook timed loops so any such
// effect applies equally to both; it must never be removed from one loop only.
inline void memory_barrier() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#else
    (void)0;
#endif
}

// ---------------------------------------------------------------------------
// Optional perf-control gate (Phase 3.1).
//
// perf can be told to keep its counters disabled until an explicit `enable`
// arrives, then disabled again, via the perf control interface
// (perf man page, `--control=fifo:<ctl>[,<ack>]` together with `--delay=-1`):
//   perf stat -D -1 --control=fifo:OUTDIR/gate_ctl,OUTDIR/gate_ack ...
// That lets a profiler measure ONLY the timed steady-state apply() loop instead
// of the whole process (which also contains the untimed stream generation and
// snapshot cold-start — enormous for map at 1M levels, and not what Phase 2
// measured). This gate is the benchmark half of that handshake.
//
// Roles and ownership (so the two halves agree):
//   * The HARNESS (scripts/perf-profile.sh) creates both fifos with mkfifo and
//     hands their paths to perf via --control=fifo:<ctl>,<ack>. perf opens its
//     ends at startup. The harness also exports LLOB_PERF_CONTROL=<base> to the
//     benchmark child, where <base> names the fifo pair.
//   * The BENCHMARK only OPENS those fifos. It never mkfifos. In time_book it
//     writes "enable\n" to <ctl> and waits for perf's ack, runs the timed
//     apply() loop, then writes "disable\n" and waits for the second ack. perf's
//     counters therefore span exactly the measured block(s).
//   * Fifo path = <base> + "_ctl" / "_ack" (no per-block suffix): ONE stable
//     pair per benchmark process, matching the harness's single --control pair.
//     The profiling harness runs reps=1, so the counted window is exactly the
//     one measured block.
//   * Open semantics follow perf's own documented example: each fifo is opened
//     O_RDWR (never blocks, regardless of which side connected first), then the
//     ack is read with a bounded poll so a missing perf cannot hang the run.
//
// Normal Phase 2 runs are UNAFFECTED: with LLOB_PERF_CONTROL unset, the gate is
// never entered and the timed loop is byte-for-byte identical. When the gate is
// active but a handshake fails (no perf listening, fifo gone, no ack), the run
// FAILS LOUDLY: a counter set that silently never enabled would look like an
// empty measured region — an empty (not zero) window is strictly worse than an
// error the operator can see.
// ---------------------------------------------------------------------------
#if defined(__linux__)
namespace {

constexpr const char* kPerfControlVar = "LLOB_PERF_CONTROL";

// Send one perf control command ("enable" or "disable") and wait for perf's
// acknowledgement. Returns false (and prints why) when the handshake cannot
// complete. ctl/ack are fifo paths.
//
// Perf's ack protocol (Linux tools/perf/util/evlist.h):
//   #define EVLIST_CTL_CMD_ACK_TAG "ack\n"
// and perf writes sizeof(EVLIST_CTL_CMD_ACK_TAG) bytes to the ack fifo after a
// control command completes — i.e. the literal "ack\n" (4 bytes: "ack", '\n',
// and a trailing NUL), REGARDLESS of which command was sent. The ack does NOT
// echo the command. A non-"ack" response means the handshake is out of sync, so
// we treat it as a failure.
bool perf_ctrl(const char* base, const char* cmd, int timeout_ms) {
    char ctl[1024];
    char ack[1024];
    std::snprintf(ctl, sizeof(ctl), "%s_ctl", base);
    std::snprintf(ack, sizeof(ack), "%s_ack", base);

    // O_RDWR: opening a fifo write-only or read-only blocks until the other
    // side connects; read-write never blocks (perf's documented example).
    const int ctl_fd = ::open(ctl, O_RDWR);
    if (ctl_fd < 0) {
        std::fprintf(stderr, "perf-control: cannot open control fifo %s "
                             "(%s)\n",
                     ctl, std::strerror(errno));
        return false;
    }
    const int ack_fd = ::open(ack, O_RDWR);
    if (ack_fd < 0) {
        std::fprintf(stderr, "perf-control: cannot open ack fifo %s (%s)\n",
                     ack, std::strerror(errno));
        ::close(ctl_fd);
        return false;
    }

    char out[16];
    const int n = std::snprintf(out, sizeof(out), "%s\n", cmd);
    const ssize_t wrote = ::write(ctl_fd, out, static_cast<size_t>(n));
    if (wrote != n) {
        std::fprintf(stderr, "perf-control: write '%s' to %s failed\n", cmd,
                     ctl);
        ::close(ack_fd);
        ::close(ctl_fd);
        return false;
    }

    // perf writes the ack only after acting on the command; bound the wait so a
    // wedged handshake fails loudly instead of hanging the whole run.
    struct pollfd p = {ack_fd, POLLIN, 0};
    const int pr = ::poll(&p, 1, timeout_ms);
    if (pr <= 0) {
        std::fprintf(stderr, "perf-control: no ack for '%s' within %d ms\n",
                     cmd, timeout_ms);
        ::close(ack_fd);
        ::close(ctl_fd);
        return false;
    }
    // The ack is the literal "ack\n" (plus a trailing NUL in perf's write) for
    // BOTH enable and disable — it never echoes the command. Accept the prefix
    // "ack"; anything else means the controller is not perf / out of sync.
    char buf[16];
    const ssize_t got = ::read(ack_fd, buf, sizeof(buf) - 1);
    ::close(ack_fd);
    ::close(ctl_fd);
    if (got <= 0) {
        std::fprintf(stderr, "perf-control: empty ack for '%s'\n", cmd);
        return false;
    }
    buf[got] = '\0';
    if (std::strncmp(buf, "ack", 3) != 0) {
        std::fprintf(stderr, "perf-control: ack for '%s' is '%s', expected "
                             "'ack\\n' (perf protocol)\n",
                     cmd, buf);
        return false;
    }
    return true;
}

// On Linux the gate must FAIL when the env var is set but the fifos cannot be
// opened (e.g. the harness is not actually running perf): a profile whose
// counters never enabled would be an empty window presented as data. Runs the
// check once, up front, before any block is timed.
void perf_gate_probe(const char* base) {
    char ctl[1024];
    std::snprintf(ctl, sizeof(ctl), "%s_ctl", base);
    if (::access(ctl, F_OK) != 0) {
        std::fprintf(stderr,
                     "LLOB_PERF_CONTROL=%s but control fifo %s is missing — "
                     "perf gate cannot arm. Refusing to run.\n",
                     base, ctl);
        std::exit(2);
    }
}

} // namespace
#endif // defined(__linux__)

// ---------------------------------------------------------------------------
// Optional macOS signpost interval (Phase 3M).
//
// os_signpost intervals are the Apple Instruments-native way to mark a region
// of interest in a standalone executable: Instruments' Time Profiler / CPU
// Counters / the `log` CLI can show "llob.apply.block" as one interval with a
// start and an end. The region it marks tightly brackets the same steady-state
// apply() block that the wall clock times — a separate mechanism from the Linux
// perf gate above, and macOS-only.
//
//   * Opt-in via the env var LLOB_SIGNPOSTS=1. Default (unset) emits nothing,
//     so normal Phase 2 runs are byte-for-byte unchanged.
//   * The interval is emitted once per TIMED BLOCK (not once per update): begin
//     just before t0, end just after t1 — the tightest practical wrapper around
//     the apply() loop that does not put a call inside the loop. The signpost
//     interval tightly brackets the chrono-measured block; a small fixed
//     boundary overhead (the os_signpost_enabled() check and the begin/end
//     emission) remains outside the chrono interval and is amortized over the
//     large update count. It is NOT part of the measured ns/update; a profile
//     must be read at the interval granularity, not as per-update cost.
//   * Compiled only on Apple platforms (__APPLE__), never on Linux.
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
namespace {

constexpr const char* kSignpostVar = "LLOB_SIGNPOSTS";

// Non-zero when the caller requested signposts (env LLOB_SIGNPOSTS=1). Read
// once so the marker never re-checks the environment inside a timed block.
bool signposts_requested() noexcept {
    const char* e = std::getenv(kSignpostVar);
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}

// Dedicated signpost log handle for this standalone binary.
//
// The C os_signpost API takes an os_log_t. For a plain C++ executable we create
// our own log under a fixed subsystem/category rather than emitting to
// OS_LOG_DEFAULT, so signposts are attributed to this benchmark and surface as
// a clean "Points of Interest" stream in Instruments. The C API (os_log_create
// / os_signpost_*) is considered legacy and is deprecated in newer Apple SDKs,
// but keeping it here is deliberate for this small pure-C++ profiling hook: it
// needs no Swift/Objective-C++, no OSSignposter, and links against the system
// log directly. The handle is created lazily on first use and never released
// (the process lifetime matches the benchmark, so a leak is meaningless here).
inline os_log_t signpost_log() noexcept {
    static os_log_t h =
        os_log_create("com.siyu.lowlatencytradinglab",
                      OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    return h;
}

} // namespace
#endif // defined(__APPLE__)

struct TimedResult {
    double best_ns_total      = 0.0; // best-of-reps wall time of the timed loop
    double best_ns_per_update = 0.0; // best_ns_total / updates
};

// Time `ops` steady updates against one Book over `reps` blocks. A FRESH book is
// cold-started from `snap` (a load_snapshot, untimed) at the start of every
// block so each rep is independent and begins from the same full, synced state.
// `tick_max` is the shared [1, tick_max] domain of the books under test (for our
// scale-N snapshots it is 2N, covering bids up to 2N and asks up to N). Returns
// the best-of-reps ns/update over the timed portion only.
template <typename Book>
TimedResult time_book(const BookSnapshot& snap,
                      const std::vector<L2Update>& ops,
                      int64_t tick_max, int reps) {
    TimedResult best;
    best.best_ns_per_update = 1e300;

    // One perf enable/disable window (Linux, Phase 3L) and one os_signpost
    // interval (macOS, Phase 3M) per timed block. Both are no-ops when unset;
    // when set, each tightly brackets the timed apply() loop: enable/begin
    // before t0, disable/end after t1, and the end-state reads AFTER. Each
    // tool's window therefore covers the same apply() block as the Clock::now()
    // window, with a small fixed boundary overhead (the handshake / the signpost
    // check) between each marker and t0/t1, outside the timed region and
    // amortized over the update count.
#if defined(__linux__)
    const char* gate_base = std::getenv(kPerfControlVar);
    const bool  gated     = gate_base != nullptr && gate_base[0] != '\0';
#elif defined(__APPLE__)
    const bool  signposts = signposts_requested();
#endif

    uint64_t sink = 0;

    for (int rep = 0; rep < reps; ++rep) {
        Book book(1, tick_max);
        if (!book.load_snapshot(snap)) { // cold start; untimed, like the old fill
            std::fprintf(stderr, "time_book: snapshot rejected (N=%lld) — aborting\n",
                         static_cast<long long>(tick_max));
            std::exit(2);
        }

        // Bring perf's counters (Linux) / begin the signpost interval (macOS)
        // up just before t0.
#if defined(__linux__)
        if (gated && !perf_ctrl(gate_base, "enable", 5000)) std::exit(2);
#elif defined(__APPLE__)
        if (signposts) {
            os_signpost_interval_begin(signpost_log(), OS_SIGNPOST_ID_EXCLUSIVE,
                                       "llob.apply.block");
        }
#endif
        const auto t0 = Clock::now();
        for (const L2Update& u : ops) {
            sink ^= static_cast<uint64_t>(book.apply(u));
            memory_barrier(); // optimizer barrier; identical in both loops (see above)
        }
        const auto t1 = Clock::now();

        // Drop perf's counters (Linux) / end the signpost interval (macOS)
        // immediately after t1, BEFORE the end-state reads: the tool window
        // [enable..disable] / [begin..end] and the chrono window [t0..t1] must
        // cover exactly the same apply() block. The end-state reads are off the
        // clock AND off the observed window; they only make the timed writes
        // observable.
#if defined(__linux__)
        if (gated && !perf_ctrl(gate_base, "disable", 5000)) std::exit(2);
#elif defined(__APPLE__)
        if (signposts) {
            os_signpost_interval_end(signpost_log(), OS_SIGNPOST_ID_EXCLUSIVE,
                                     "llob.apply.block");
        }
#endif

        // End-state reads (off the clock). Common API across both books; pin
        // best_bid/best_ask (which reflect cache writes made during the timed
        // loop) and the live-level count so the final state is observable.
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

    // Final observable read of the accumulator: the branch is never taken on any
    // real stream, but it makes every apply() result and end-state read above
    // genuinely influence program behavior, so the optimizer cannot drop them.
    if (sink == 0x9E37'79B9'7F4A'7C15ULL) {
        best.best_ns_per_update = -1.0; // unreachable; marks the writes as live
    }
    return best;
}

// ---------------------------------------------------------------------------
// Validation pass (--check). Replays the SAME deterministic stream through BOTH
// books and verifies they produce identical externally visible state at every
// step. The map book is the reference: flat must match it exactly, because the
// Phase 1 differential unit test already establishes they implement the same
// contract. Divergence here would mean a generator op is exercising a path the
// unit tests never hit, so it FAILS loudly (non-zero exit) instead of being
// measured. This pass is entirely OUTSIDE the timed benchmark path.
// ---------------------------------------------------------------------------
int check_streams(uint64_t updates) {
    int failures = 0;
    std::printf("--check: differential MapOrderBook vs FlatOrderBook "
                "(identical stream, identical domain)\n");
    for (int64_t n : kScaleLevels) {
        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const auto ops = StreamGen::steady_ops(wl, n, updates);

            MapOrderBook  mbook(1, 2 * n);
            FlatOrderBook fbook(1, 2 * n);
            const bool loaded_m = mbook.load_snapshot(snap);
            const bool loaded_f = fbook.load_snapshot(snap);
            if (!(loaded_m && loaded_f)) {
                ++failures;
                std::printf("  [FAIL] N=%-9" PRId64 " wl=%s snapshot rejected "
                            "(map=%d flat=%d)\n",
                            n, workload_tag(wl), loaded_m ? 1 : 0,
                            loaded_f ? 1 : 0);
                continue;
            }

            uint64_t applied = 0, other = 0;
            for (const L2Update& u : ops) {
                const ApplyResult rm = mbook.apply(u);
                const ApplyResult rf = fbook.apply(u);
                // Compare the requested externally visible state after each op.
                const bool same = rm == rf &&
                                  mbook.synced() == fbook.synced() &&
                                  mbook.last_applied_seq() == fbook.last_applied_seq() &&
                                  mbook.best_bid() == fbook.best_bid() &&
                                  mbook.best_ask() == fbook.best_ask() &&
                                  mbook.best_bid_qty() == fbook.best_bid_qty() &&
                                  mbook.best_ask_qty() == fbook.best_ask_qty();
                if (!same) {
                    if (failures < 10) {
                        std::printf("  [DIVERGE] N=%-9" PRId64 " wl=%s seq=%" PRIu64
                                    " map(rs=%d sync=%d seq=%" PRIu64 " bb=%" PRId64 "/%" PRId64
                                    " ba=%" PRId64 "/%" PRId64 ") vs flat(rs=%d sync=%d seq=%" PRIu64
                                    " bb=%" PRId64 "/%" PRId64 " ba=%" PRId64 "/%" PRId64 ")\n",
                                    n, workload_tag(wl), u.seq,
                                    static_cast<int>(rm), mbook.synced() ? 1 : 0,
                                    mbook.last_applied_seq(), mbook.best_bid(),
                                    mbook.best_bid_qty(), mbook.best_ask(),
                                    mbook.best_ask_qty(), static_cast<int>(rf),
                                    fbook.synced() ? 1 : 0, fbook.last_applied_seq(),
                                    fbook.best_bid(), fbook.best_bid_qty(),
                                    fbook.best_ask(), fbook.best_ask_qty());
                    }
                    ++failures;
                    break; // this stream is broken; report the cell and move on
                }
                if (rm == ApplyResult::Applied) ++applied;
                else ++other;
            }
            std::printf("  N=%-9" PRId64 " wl=%s %-28s ok  applied=%" PRIu64
                        " non_applied=%" PRIu64 " end_levels(bid/ask)=%zu/%zu"
                        " best=%" PRId64 "/%" PRId64 "\n",
                        n, workload_tag(wl), workload_desc(wl), applied, other,
                        mbook.bids().size(), mbook.asks().size(),
                        mbook.best_bid(), mbook.best_ask());
        }
    }
    if (failures == 0) {
        std::printf("--check PASSED: Map and Flat agree on every op of every "
                    "stream (no divergence)\n");
    } else {
        std::printf("--check FAILED: %d stream(s) diverged between Map and Flat\n",
                    failures);
    }
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void print_header(uint64_t updates, int reps) {
    std::printf("# orderbook_bench - deterministic steady-state apply() throughput\n");
    std::printf("# domain [1, 2N]; starts with N live levels/side; fill untimed; "
                "best of %d reps; %" PRIu64 " steady ops per block\n",
                reps, updates);
    std::printf("# impl,wl,scale_n,updates,best_ms,best_ns_per_update,best_updates_per_s\n");
}

template <typename Book>
void run_impl(const char* impl_name, const char* wl_sel, const char* scale_sel,
              uint64_t updates, int reps) {
    for (int si = 0; si < 4; ++si) {
        const int64_t n = kScaleLevels[si];
        const bool want_scale = std::strcmp(scale_sel, "all") == 0 ||
                                std::strtoll(scale_sel, nullptr, 10) == n;
        if (!want_scale) continue;

        const auto snap = StreamGen::fill_snapshot(n);
        for (int w = 0; w < kWorkloadCount; ++w) {
            const Workload wl = static_cast<Workload>(w);
            const bool want_wl = std::strcmp(wl_sel, "all") == 0 ||
                                 workload_tag(wl)[0] == wl_sel[0];
            if (!want_wl) continue;

            const auto ops       = StreamGen::steady_ops(wl, n, updates);
            const TimedResult r  = time_book<Book>(snap, ops, 2 * n, reps);
            std::printf("%s,%s,%.0f,%" PRIu64 ",%.3f,%.3f,%.0f\n",
                        impl_name, workload_tag(wl), static_cast<double>(n),
                        updates, r.best_ns_total / 1e6, r.best_ns_per_update,
                        1e9 / r.best_ns_per_update);
            std::fflush(stdout);
        }
    }
    std::printf("# done %s\n", impl_name);
}

void usage(const char* argv0) {
    std::printf(
        "usage: %s [impl] [workload] [scale] [updates=N] [reps=N] [--check]\n"
        "  impl      map | flat | both        (default both)\n"
        "  workload  A B C D E | all          (default all)\n"
        "  scale     1000 | 10000 | 100000 | 1000000 | all   (default all)\n"
        "  updates=N   steady ops per timed block            (default %" PRIu64 ")\n"
        "  reps=N      timed blocks per cell; best is kept   (default %d)\n"
        "  --check     replay streams through both books, require agreement; no timing\n"
        "  env LLOB_PERF_CONTROL=<base>   (Linux, profiling only) handshake with perf via\n"
        "                   <base>_ctl / <base>_ack fifos; see scripts/perf-profile.sh\n"
        "  env LLOB_SIGNPOSTS=1           (macOS, profiling only) emit os_signpost interval\n"
        "                   'llob.apply.block' around the timed apply loop (Instruments/log)\n",
        argv0, kDefaultUpdates, kDefaultReps);
}

} // namespace

int main(int argc, char** argv) {
    const char* impl_sel  = "both";
    const char* wl_sel    = "all";
    const char* scale_sel = "all";
    uint64_t    updates   = kDefaultUpdates;
    int         reps      = kDefaultReps;
    bool        check     = false;

    // First three positional args are impl, workload, scale, in that order.
    int pos = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--check") == 0) {
            check = true;
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

    if (check) {
        return check_streams(updates); // 0 == both books agree, 1 == divergence
    }

    const bool want_map  = std::strcmp(impl_sel, "map") == 0 ||
                          std::strcmp(impl_sel, "both") == 0;
    const bool want_flat = std::strcmp(impl_sel, "flat") == 0 ||
                           std::strcmp(impl_sel, "both") == 0;
    if (!want_map && !want_flat) {
        std::fprintf(stderr, "unknown impl '%s' (want map|flat|both)\n", impl_sel);
        usage(argv[0]);
        return 2;
    }

#if defined(__linux__)
    // Phase 3.1 profiling gate: if the env var is set, the perf control fifos
    // must exist before we print anything or time anything. Probe once, up
    // front — a profile whose counters never enabled would be an empty window
    // presented as data, so the run refuses loudly instead.
    {
        const char* g = std::getenv(kPerfControlVar);
        if (g != nullptr && g[0] != '\0') perf_gate_probe(g);
    }
#endif

    print_header(updates, reps);
    if (want_map) run_impl<MapOrderBook>("map", wl_sel, scale_sel, updates, reps);
    if (want_flat) run_impl<FlatOrderBook>("flat", wl_sel, scale_sel, updates, reps);
    return 0;
}
