// Deterministic tail-latency / jitter benchmark (Experiment 01, Phase 4).
//
// What this measures: the DISTRIBUTION of apply() latency for ONE L2 book
// implementation over a pre-recorded, well-formed update stream, using
// fixed-size BATCH sampling. Throughput averages (Phase 2) hide the spread —
// the long tail of occasional slow updates is exactly what a latency-sensitive
// consumer cares about. Phase 4 characterizes that tail.
//
// WHY batch sampling (NOT per-update timing): a FlatOrderBook update can take a
// few nanoseconds. Per-update std::chrono reads are themselves many nanoseconds,
// so timer overhead would dominate the operation being measured. Instead we time
// fixed-size batches of `batch_size` contiguous updates, store the raw integer
// batch duration, and DERIVE a batch-normalized ns/update = batch_duration /
// batch_size.
//
//   batch-normalized ns/update is NOT a directly measured single-update latency.
//   It is a batch average. It understates true per-update tail spread (a single
//   slow update inside a batch is diluted by its fast neighbours) and it carries
//   the timer-read boundary cost divided across the batch. Use it to compare
//   latency DISTRIBUTIONS and jitter between designs/workloads, not as a
//   per-update latency number.
//
// Deterministic input: the stream uses the SAME generator semantics as Phase 2
// (benchmark/stream_gen.h — the single source of truth). Under the default seed
// a Phase 4 (workload, scale, updates) stream is EXACTLY the Phase 2 stream;
// --seed N yields a different-but-equally-deterministic stream of the SAME
// workload. The stream and the snapshot are generated BEFORE any timer starts.
//
// Setup is OUTSIDE measurement: construct book, load snapshot, generate stream,
// reserve sample storage, validate, warm up — none inside a timed batch. No file
// IO / formatting inside a timed batch. Raw samples are written to the
// preallocated vector only AFTER each batch's t1, and the CSV is written only
// after ALL measurement completes.
//
// Warmup: a SEPARATE book instance replays the SAME stream once, untimed, so the
// measured run starts with warm code/caches/allocator state but still starts from
// the original full snapshot — no part of the measured stream is consumed early.
// Warmup deliberately warms the process (caches, allocator arenas, thermal state
// drift toward a steady state); it is NOT a mechanism that removes scheduler or
// thermal noise, which can still move P99/P99.9/MAX (see the macOS limitations in
// the docs). No additional warmup dimensions are modeled.
//
// Partial final batch: updates are NOT required to divide evenly by batch_size. A
// trailing partial batch is SUPPORTED: if updates % batch_size is nonzero, the
// remaining updates are applied and timed as their OWN sample so they are
// recorded with their ACTUAL operation count (batch_operations = updates %
// batch_size; NEVER normalized by the full batch size). The partial sample is
// EXCLUDED from ALL distribution metrics — mean, min, percentiles, and max are
// computed over FULL batches only, because a short batch would carry
// disproportionate timer-boundary noise into every metric it touched. The partial
// appears in the raw CSV as the final row (batch_operations < batch_size) and in
// the summary as explicit partial_* keys. Every update is always applied, so the
// measured book's final state is the true post-stream state. The canonical
// 10,000,000 / 512 configuration has remainder 128: 19,531 full batches form the
// distribution and one 128-op partial batch is recorded and excluded.
//
// Calibration (--calibrate) reports TWO distinct groups — terminology is
// deliberate, neither is "subtracted" from the measured samples:
//   * clock_pair_*        pure clock cost: two steady_clock reads separated only
//                         by a compiler barrier. The timer floor of one read pair.
//   * empty_batch_harness_* the existing batch-sized empty-loop skeleton: clock
//                         read + batch_size empty iterations with a barrier +
//                         sink arithmetic + clock read. This is the whole timing
//                         skeleton WITH NO BOOK WORK — it is NOT just the clock,
//                         it also carries loop/barrier overhead. Use it to judge
//                         whether a batch is large enough that a full batch's
//                         duration dwarfs this fixed overhead.
// Neither group is auto-subtracted from book samples (an unverified correction);
// if empty_batch_harness is material vs a typical flat batch at batch_size=512,
// the docs recommend a larger batch.
//
// Distribution metrics are computed ONLY after measurement is complete, from the
// raw integer batch durations (source of truth, not prematurely rounded).
//
// Canonical methodology: ONE process, ONE book, ONE deterministic stream, ONE
// latency distribution. NO best-of-N. Map and flat canonical runs are SEPARATE
// processes. This does not eliminate thermal / scheduler noise; it is process
// isolation (no mixed state) plus clearer provenance. On Apple Silicon, macOS
// schedules across P/E cores freely and background activity can push P99/P99.9/
// MAX — a tail spike is NOT automatically attributed to order-book code.
//
// Usage (long flags, both --flag value and --flag=value):
//   orderbook_tail_bench --book map|flat --workload A|B|C|D|E --levels N
//       [--updates N] [--batch-size N] [--seed N] [--samples-out FILE]
//       [--stats-out FILE] [--calibrate [--cal-samples N]]
//     --book map|flat        which implementation (REQUIRED)
//     --workload A..E        which workload, exactly one of A/B/C/D/E (REQUIRED)
//     --levels N             scale in price levels (REQUIRED)
//     --updates N            total steady ops applied (default 10000000)
//     --batch-size N         updates per timed batch (default 512). Updates need
//                            NOT divide evenly: a trailing partial batch
//                            (updates % batch_size) is timed and recorded with
//                            its ACTUAL op count and excluded from the
//                            distribution (see the header note).
//     --seed N               stream seed (default = Phase 2 seed; reproduces the
//                            exact Phase 2 stream for the same wl/levels/updates)
//     --samples-out FILE     write raw CSV samples here after measurement
//                            (default: no file)
//     --stats-out FILE       write a key:value summary here after measurement
//                            (default: stdout)
//     --calibrate            run calibration and exit (no book, no stream, no
//                            distribution): reports clock_pair_* and
//                            empty_batch_harness_* groups (see the header note)
//     --cal-samples N        samples for --calibrate (default 1000000)
//     -h|--help              this text
//
// Example:
//   ./orderbook_tail_bench --book flat --workload C --levels 1000000 \
//       --updates 10000000 --batch-size 512 --samples-out raw.csv
//
// The canonical Phase 4 cells and reproduction commands are in
// docs/profiling/PHASE4_TAIL_LATENCY.md.

#include "flat_order_book.h"
#include "map_order_book.h"
#include "stream_gen.h"
#include "tail_stats.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

using llob::ApplyResult;
using llob::BookSnapshot;
using llob::FlatOrderBook;
using llob::L2Update;
using llob::MapOrderBook;
using llob::Side;

using llob_bench::StreamGen;
using llob_bench::Workload;
using llob_bench::kDefaultSeed;
using llob_bench::workload_desc;
using llob_bench::workload_tag;

// Phase 4 percentile / normalization / distribution helpers (shared with the
// unit tests). distribution_metrics() is the ONE production rule for "which
// samples form the distribution": FULL batches only, a trailing partial batch
// never leaks into any metric. The tests exercise this exact function.
using llob_tail::BatchSample;
using llob_tail::distribution_metrics;
using llob_tail::normalized_ns_per_update;
using llob_tail::percentile_rank;
using llob_tail::sorted_samples;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kDefaultUpdates = 10'000'000;
constexpr uint64_t kDefaultBatch   = 512;
constexpr int      kDefaultCalSamples = 1'000'000;

// Compiler memory barrier (identical helper and comment to the Phase 2
// benchmark): in a timed loop it forces every apply() to be treated as opaque
// memory-touching work so the optimizer cannot prove stores dead or reorder
// across apply() calls. Both books' loops use the same barrier.
inline void memory_barrier() noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#else
    (void)0;
#endif
}

// ---------------------------------------------------------------------------
// Calibration group A — CLOCK PAIR: the pure cost of reading steady_clock twice
// with only a compiler barrier between the reads. `samples` independent reads.
// This is the timer floor of one clock-read pair; it contains NO loop and NO
// book work.
// ---------------------------------------------------------------------------
std::vector<int64_t> calibrate_clock_pair(uint64_t samples) {
    std::vector<int64_t> out;
    out.reserve(static_cast<size_t>(samples));
    for (uint64_t s = 0; s < samples; ++s) {
        const auto t0 = Clock::now();
        memory_barrier();
        const auto t1 = Clock::now();
        out.push_back(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Calibration group B — EMPTY-BATCH HARNESS: the exact batch timing skeleton
// with NO book work, `samples` times. Each pass reads the clock twice around an
// empty loop of `batch_size` "iterations" (the book-apply site is replaced by an
// opaque sink increment so the loop body still exists), then records the elapsed
// ns. Each sample is therefore: clock read + batch_size loop iterations + sink
// arithmetic + compiler barriers + clock read. This is NOT pure clock cost — it
// carries the whole loop/barrier skeleton — so it is the honest per-batch fixed
// overhead to compare against a full batch's duration.
// ---------------------------------------------------------------------------
std::vector<int64_t> calibrate_empty_batch(uint64_t batch_size, uint64_t samples,
                                           uint64_t& sink_out) {
    std::vector<int64_t> out;
    out.reserve(static_cast<size_t>(samples));
    uint64_t sink = 0;
    for (uint64_t s = 0; s < samples; ++s) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < batch_size; ++i) {
            sink += i;      // no book; just enough body that the loop is real
            memory_barrier();
        }
        const auto t1 = Clock::now();
        out.push_back(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    sink_out = sink;
    return out;
}

// ---------------------------------------------------------------------------
// Phase 4 CLI
// ---------------------------------------------------------------------------
struct Config {
    bool         have_book = false;
    bool         map       = false; // false => flat when have_book
    Workload     wl        = Workload::A;
    int64_t      levels    = 0;
    uint64_t     updates   = kDefaultUpdates;
    uint64_t     batch     = kDefaultBatch;
    uint64_t     seed      = kDefaultSeed;
    bool         calibrate = false;
    uint64_t     cal_samples = kDefaultCalSamples;
    std::string  samples_out;
    std::string  stats_out;
};

void usage(const char* argv0) {
    std::printf(
        "usage: %s --book map|flat --workload A..E --levels N [options]\n"
        "  --book map|flat          implementation (required)\n"
        "  --workload A|B|C|D|E     workload (required)\n"
        "  --levels N               scale in price levels (required)\n"
        "  --updates N              total steady ops (default %" PRIu64 ")\n"
        "  --batch-size N           updates per timed batch (default %" PRIu64 ");\n"
        "                           a trailing partial batch is timed with its actual\n"
        "                           op count and excluded from the percentiles\n"
        "  --seed N                 stream seed (default = Phase 2 seed)\n"
        "  --samples-out FILE       write raw CSV samples after measurement\n"
        "  --stats-out FILE         write key:value summary (default: stdout)\n"
        "  --calibrate [--cal-samples N]  calibration (clock_pair_* + empty_batch_harness_*),\n"
        "                           then exit; nothing is auto-subtracted\n"
        "  -h|--help                this text\n"
        "\n"
        "batch-normalized ns/update = batch_duration / batch_size. This is a batch\n"
        "average, NOT a directly measured single-update latency.\n",
        argv0, kDefaultUpdates, kDefaultBatch);
}

// Consume one --flag value (or --flag=value). Returns false if the arg is a
// value-bearing flag that cannot be satisfied.
bool take_value(int argc, char** argv, int& i, const char* name,
                const char** out) {
    const size_t n = std::strlen(name);
    if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=') {
        *out = argv[i] + n + 1;
        return true;
    }
    if (std::strcmp(argv[i], name) == 0) {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s requires a value\n", name);
            return false;
        }
        *out = argv[++i];
        return true;
    }
    return false; // not this flag
}

// Parse an UNSIGNED integer option. Accepts only plain decimal digits (no sign,
// no surrounding whitespace), so a negative value like "-5" can never wrap into
// a huge uint64_t; ERANGE (integer overflow) is rejected.
bool parse_u64(const char* s, const char* what, uint64_t& out) {
    if (s[0] < '0' || s[0] > '9') {
        std::fprintf(stderr,
                     "invalid %s: '%s' (expected a non-negative integer)\n",
                     what, s);
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (errno == ERANGE || *end != '\0') {
        std::fprintf(stderr, "invalid %s: '%s' (out of range)\n", what, s);
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

// Parse a SIGNED integer option (only --levels uses this; negative values still
// fail the caller's positivity check). ERANGE overflow is rejected.
bool parse_i64(const char* s, const char* what, int64_t& out) {
    if (*s == '\0' || (*s != '-' && (s[0] < '0' || s[0] > '9'))) {
        std::fprintf(stderr, "invalid %s: '%s'\n", what, s);
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s, &end, 10);
    if (errno == ERANGE || *end != '\0') {
        std::fprintf(stderr, "invalid %s: '%s' (out of range)\n", what, s);
        return false;
    }
    out = static_cast<int64_t>(v);
    return true;
}

bool parse_args(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = nullptr;
        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            usage(argv[0]);
            std::exit(0);
        } else if (std::strcmp(a, "--calibrate") == 0) {
            c.calibrate = true;
        } else if (take_value(argc, argv, i, "--book", &v)) {
            if (std::strcmp(v, "map") == 0) { c.have_book = true; c.map = true; }
            else if (std::strcmp(v, "flat") == 0) { c.have_book = true; c.map = false; }
            else { std::fprintf(stderr, "--book must be map|flat (got '%s')\n", v); return false; }
        } else if (take_value(argc, argv, i, "--workload", &v)) {
            // Must be EXACTLY one of A..E: a value merely beginning with A..E
            // (e.g. "A2", "AB") or empty is rejected, not accepted by prefix.
            if (v[0] < 'A' || v[0] > 'E' || v[1] != '\0') {
                std::fprintf(stderr, "--workload must be exactly A|B|C|D|E (got '%s')\n", v);
                return false;
            }
            switch (v[0]) {
                case 'A': c.wl = Workload::A; break;
                case 'B': c.wl = Workload::B; break;
                case 'C': c.wl = Workload::C; break;
                case 'D': c.wl = Workload::D; break;
                case 'E': c.wl = Workload::E; break;
            }
        } else if (take_value(argc, argv, i, "--levels", &v)) {
            if (!parse_i64(v, "--levels", c.levels)) return false;
        } else if (take_value(argc, argv, i, "--updates", &v)) {
            if (!parse_u64(v, "--updates", c.updates)) return false;
        } else if (take_value(argc, argv, i, "--batch-size", &v)) {
            if (!parse_u64(v, "--batch-size", c.batch)) return false;
        } else if (take_value(argc, argv, i, "--seed", &v)) {
            if (!parse_u64(v, "--seed", c.seed)) return false;
        } else if (take_value(argc, argv, i, "--cal-samples", &v)) {
            if (!parse_u64(v, "--cal-samples", c.cal_samples)) return false;
        } else if (take_value(argc, argv, i, "--samples-out", &v)) {
            c.samples_out = v;
        } else if (take_value(argc, argv, i, "--stats-out", &v)) {
            c.stats_out = v;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n", a);
            return false;
        }
    }

    if (c.calibrate) {
        if (c.cal_samples == 0) {
            std::fprintf(stderr, "--cal-samples must be >= 1\n");
            return false;
        }
        return true; // calibration needs no book/workload/levels
    }

    if (!c.have_book) {
        std::fprintf(stderr, "--book (map|flat) is required\n");
        return false;
    }
    if (c.levels <= 0) {
        std::fprintf(stderr, "--levels must be a positive integer\n");
        return false;
    }
    if (c.updates == 0) {
        std::fprintf(stderr, "--updates must be >= 1\n");
        return false;
    }
    if (c.batch == 0) {
        std::fprintf(stderr, "--batch-size must be >= 1\n");
        return false;
    }
    // A final partial batch (updates % batch != 0) is allowed: it is measured
    // with its ACTUAL operation count and recorded in the raw CSV, but EXCLUDED
    // from the percentile distribution (a short batch would carry disproportionate
    // timer-boundary noise into the tail). If there are not enough updates for
    // even one full batch there is no distribution to report.
    if (c.updates < c.batch) {
        std::fprintf(stderr,
                     "--updates (%" PRIu64 ") must be >= --batch-size (%" PRIu64
                     ") so at least one full batch exists.\n",
                     c.updates, c.batch);
        return false;
    }
    return true;
}

// BatchSample is shared from tail_stats.h (llob_tail::BatchSample): the same
// struct the unit tests and distribution_metrics() use.

// ---------------------------------------------------------------------------
// One full distribution run for one book over one stream.
// ---------------------------------------------------------------------------
template <typename Book>
int run_distribution(const Config& c) {
    const char* impl = c.map ? "map" : "flat";
    const int64_t tick_max = 2 * c.levels;

    // Setup — entirely OUTSIDE measurement.
    const BookSnapshot snap = StreamGen::fill_snapshot(c.levels);
    // A huge but parse-valid --updates (up to 2^64-1) cannot be materialized as
    // one stream vector; catch the allocation failure so the tool reports a clean
    // error instead of terminating on an uncaught length_error/bad_alloc.
    std::vector<L2Update> ops;
    try {
        ops = StreamGen::steady_ops(c.wl, c.levels, c.updates, c.seed);
    } catch (const std::length_error&) {
        std::fprintf(stderr,
                     "--updates %" PRIu64 " is too large to materialize as one "
                     "update stream (vector length limit)\n", c.updates);
        return 2;
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr,
                     "--updates %" PRIu64 ": out of memory materializing the "
                     "update stream\n", c.updates);
        return 2;
    }

    // Warmup on a SEPARATE book instance (the measured run still starts from the
    // original snapshot). Untimed.
    {
        Book warm(1, tick_max);
        if (!warm.load_snapshot(snap)) {
            std::fprintf(stderr, "warmup: snapshot rejected (N=%lld)\n",
                         static_cast<long long>(c.levels));
            return 2;
        }
        uint64_t sink = 0;
        for (const L2Update& u : ops) {
            sink ^= static_cast<uint64_t>(warm.apply(u));
            memory_barrier();
        }
        (void)sink;
    }

    // The measured book, cold-started from the same snapshot.
    Book book(1, tick_max);
    if (!book.load_snapshot(snap)) {
        std::fprintf(stderr, "snapshot rejected (N=%lld)\n",
                     static_cast<long long>(c.levels));
        return 2;
    }

    // Pre-reserve ALL sample storage up front: no reallocation mid-measurement.
    // Every update is applied: `full` full batches plus an optional final partial
    // batch of `rem` updates (timed separately, recorded with its ACTUAL op
    // count, excluded from the distribution — see split_batches).
    const llob_tail::BatchSplit bs = llob_tail::split_batches(c.updates, c.batch);
    const size_t   full_batches = bs.full;
    const uint64_t rem          = bs.remainder; // < c.batch
    const size_t   sample_count = full_batches + (rem ? 1u : 0u);
    std::vector<BatchSample> samples;
    try {
        samples.reserve(sample_count);
    } catch (const std::length_error&) {
        std::fprintf(stderr,
                     "%zu batch samples is too many to preallocate (vector length "
                     "limit); reduce --updates or raise --batch-size\n",
                     sample_count);
        return 2;
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr,
                     "out of memory preallocating %zu batch samples; reduce "
                     "--updates or raise --batch-size\n",
                     sample_count);
        return 2;
    }

    // Measure. Each batch reads the clock twice, applies its contiguous updates,
    // and stores the integer batch duration AFTER t1 (never inside the timed
    // region). The stream is contiguous and starts from a full snapshot, so
    // every update is Applied (see --check in Phase 2).
    uint64_t sink = 0;
    const L2Update* it = ops.data();
    for (size_t b = 0; b < full_batches; ++b) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < c.batch; ++i, ++it) {
            sink ^= static_cast<uint64_t>(book.apply(*it));
            memory_barrier();
        }
        const auto t1 = Clock::now();
        const int64_t d = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        samples.push_back(BatchSample{c.batch, d});
    }
    if (rem) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < rem; ++i, ++it) {
            sink ^= static_cast<uint64_t>(book.apply(*it));
            memory_barrier();
        }
        const auto t1 = Clock::now();
        const int64_t d = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        samples.push_back(BatchSample{rem, d}); // ACTUAL op count, not batch
    }
    // Every stream update has now been applied once, in order.
    // assert(it == ops.data() + ops.size());

    // The final observable read of the accumulator: never taken on a real
    // stream, but makes every apply() genuinely influence program behavior so
    // the optimizer cannot drop the timed loop.
    if (sink == 0x9E37'79B9'7F4A'7C15ULL) {
        std::fprintf(stderr, "unreachable sink guard tripped\n");
        return 2;
    }

    // ---- Post-measurement correctness check (off the clock) ----
    // Without paying for a per-update assertion inside the timed loop, require
    // the cheap end-state invariants that prove every update was applied in
    // order: the book is still synced and its sequence advanced by exactly the
    // number of updates replayed (snapshot ends at snap.seq, the steady stream
    // starts at snap.seq+1 and applies c.updates ops). If either fails, the run
    // is NOT a valid distribution: print an error and return nonzero WITHOUT
    // writing the raw CSV or summary.
    const bool     synced = book.synced();
    const uint64_t seq    = book.last_applied_seq();
    const uint64_t expect_seq = snap.seq + c.updates;
    if (!synced || seq != expect_seq) {
        std::fprintf(stderr,
                     "orderbook_tail_bench: post-measurement validation FAILED "
                     "(impl=%s wl=%s levels=%lld updates=%llu): synced=%d "
                     "last_applied_seq=%llu expected=%llu (snapshot.seq=%llu + "
                     "updates). Refusing to present this run as a valid "
                     "distribution.\n",
                     impl, workload_tag(c.wl), static_cast<long long>(c.levels),
                     static_cast<unsigned long long>(c.updates), synced ? 1 : 0,
                     static_cast<unsigned long long>(seq),
                     static_cast<unsigned long long>(expect_seq),
                     static_cast<unsigned long long>(snap.seq));
        return 2;
    }

    // End-state reads (off the clock) for the summary, now that the run is
    // known valid. best_bid/best_ask/level_count are never inside a timed batch.
    const int64_t bb = book.best_bid();
    const int64_t bq = book.best_bid_qty();
    const int64_t ba = book.best_ask();
    const int64_t aq = book.best_ask_qty();
    const size_t  lvl = book.level_count();

    // ---- Post-measurement analysis (no timing here) ----
    // distribution_metrics() computes mean/percentiles/max over FULL batches
    // only (batch_size ops each), from the FIRST `full_batches` samples. A
    // trailing partial batch — if any — is recorded in the CSV and surfaced in
    // the summary as explicit partial_* keys, but it is EXCLUDED here: a short
    // batch would carry disproportionate timer-boundary noise into every metric
    // it touched. The raw elapsed_ns values are the source of truth and are
    // normalized only by this derived step, never prematurely rounded.
    const llob_tail::DistributionMetrics m =
        distribution_metrics(samples, full_batches, c.batch);
    const double mean_ns = m.mean_ns_per_update;
    const double min_ns  = m.min_ns_per_update;
    const double p50     = m.p50_ns_per_update;
    const double p90     = m.p90_ns_per_update;
    const double p99     = m.p99_ns_per_update;
    const double p999    = m.p99_9_ns_per_update;
    const double max_ns  = m.max_ns_per_update;

    const double ratio_p99_p50  = p50 > 0 ? p99 / p50 : 0.0;
    const double ratio_p999_p50 = p50 > 0 ? p999 / p50 : 0.0;
    const double ratio_max_p50  = p50 > 0 ? max_ns / p50 : 0.0;

    // Partial-final-batch metrics (zero when updates % batch == 0): its own
    // normalized ns/update is elapsed_ns / (updates % batch) — the ACTUAL op
    // count, never the full batch size.
    const double partial_ns_per_update =
        rem ? normalized_ns_per_update(samples.back().elapsed_ns, rem) : 0.0;

    // ---- Output ----
    // Raw CSV first (source of truth preserved before the summary).
    if (!c.samples_out.empty()) {
        std::FILE* f = std::fopen(c.samples_out.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open --samples-out %s\n",
                         c.samples_out.c_str());
            return 2;
        }
        std::fprintf(f,
                     "# Phase 4 raw batch samples. batch-normalized ns/update = "
                     "elapsed_ns / batch_operations (a batch average, NOT a directly\n"
                     "# measured single-update latency). batch_operations is the "
                     "ACTUAL op count of the batch: batch_size for every row except\n"
                     "# the final row when updates %% batch_size is nonzero (a "
                     "partial batch, which is EXCLUDED from every distribution\n"
                     "# metric in the summary).\n"
                     "# impl=%s wl=%s levels=%" PRId64 " updates=%" PRIu64
                     " batch_size=%" PRIu64 " seed=%" PRIu64 " partial_rows=%u\n"
                     "sample_index,batch_operations,elapsed_ns,normalized_ns_per_update\n",
                     impl, workload_tag(c.wl), c.levels, c.updates, c.batch, c.seed,
                     rem ? 1u : 0u);
        for (size_t i = 0; i < samples.size(); ++i) {
            std::fprintf(f, "%zu,%" PRIu64 ",%" PRId64 ",%.6f\n", i,
                         samples[i].ops, samples[i].elapsed_ns,
                         normalized_ns_per_update(samples[i].elapsed_ns,
                                                  samples[i].ops));
        }
        std::fclose(f);
    }

    // Summary. Write to the requested file or stdout.
    const bool to_file = !c.stats_out.empty();
    std::FILE* out = to_file ? std::fopen(c.stats_out.c_str(), "w") : stdout;
    if (to_file && !out) {
        std::fprintf(stderr, "cannot open --stats-out %s\n", c.stats_out.c_str());
        return 2;
    }
    std::FILE* f = out;
    std::fprintf(f,
                 "# Phase 4 tail-latency summary. Units: ns per update, "
                 "batch-normalized over %" PRIu64 " updates/batch.\n"
                 "# Distribution metrics (mean/percentiles/ratios) cover FULL "
                 "batches only; see distribution_samples and the partial_batch_* keys.\n"
                 "# impl=%s wl=%s(%s) levels=%" PRId64 " updates=%" PRIu64
                 " batch_size=%" PRIu64 " seed=%" PRIu64 "\n"
                 "impl=%s\nwl=%s\nwl_desc=%s\nlevels=%" PRId64
                 "\nupdates=%" PRIu64 "\nbatch_size=%" PRIu64 "\nseed=%" PRIu64 "\n"
                 "total_samples=%zu\n"         // includes the partial row when present
                 "distribution_samples=%zu\n"  // FULL batches only — the percentile basis
                 "partial_batch_present=%u\n"
                 "partial_batch_ops=%" PRIu64 "\n"
                 "partial_ns_per_update=%.6f\n"
                 "mean_batch_normalized_ns_per_update=%.6f\n"
                 "min_batch_normalized_ns_per_update=%.6f\n"
                 "p50_batch_normalized_ns_per_update=%.6f\n"
                 "p90_batch_normalized_ns_per_update=%.6f\n"
                 "p99_batch_normalized_ns_per_update=%.6f\n"
                 "p99_9_batch_normalized_ns_per_update=%.6f\n"
                 "max_batch_normalized_ns_per_update=%.6f\n"
                 "ratio_p99_p50=%.6f\nratio_p99_9_p50=%.6f\nratio_max_p50=%.6f\n"
                 "final_synced=%d\nfinal_seq=%" PRIu64 "\nfinal_seq_expected=%" PRIu64
                 "\nfinal_best_bid=%" PRId64 "/%" PRId64
                 "\nfinal_best_ask=%" PRId64 "/%" PRId64
                 "\nfinal_level_count=%zu\n"
                 "percentile_definition=nearest-rank on sorted FULL-batch durations "
                 "(ceil(q*N)-th smallest); a trailing partial batch is recorded in "
                 "the raw CSV but excluded from the distribution\n",
                 c.batch, impl, workload_tag(c.wl), workload_desc(c.wl), c.levels,
                 c.updates, c.batch, c.seed, impl, workload_tag(c.wl),
                 workload_desc(c.wl), c.levels, c.updates, c.batch, c.seed,
                 samples.size(), m.distribution_samples, rem ? 1u : 0u, rem,
                 partial_ns_per_update, mean_ns, min_ns, p50, p90, p99, p999,
                 max_ns, ratio_p99_p50, ratio_p999_p50, ratio_max_p50,
                 synced ? 1 : 0, seq, expect_seq, bb, bq, ba, aq, lvl);
    if (to_file) std::fclose(f);

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    if (!parse_args(argc, argv, c)) {
        usage(argv[0]);
        return 2;
    }

    if (c.calibrate) {
        // Calibration: TWO deliberately-distinct groups, each reported as a
        // median / p99 / max over raw integer ns samples. Terminology matches
        // what each actually measures (see the calibrate_* functions):
        //   clock_pair_*            pure cost of a steady_clock read pair
        //   empty_batch_harness_*   the full batch-sized empty timing skeleton
        //                           (clock + loop + sink + barriers + clock)
        // Neither is auto-subtracted from latency samples; if the empty-batch
        // harness cost is material vs a typical flat batch, use a larger batch
        // (docs).
        uint64_t sink = 0;
        std::vector<int64_t> pair, empty;
        try {
            pair = calibrate_clock_pair(c.cal_samples);
            empty = calibrate_empty_batch(c.batch, c.cal_samples, sink);
        } catch (const std::length_error&) {
            std::fprintf(stderr, "--cal-samples %" PRIu64 " is too large to "
                         "preallocate (vector length limit)\n", c.cal_samples);
            return 2;
        } catch (const std::bad_alloc&) {
            std::fprintf(stderr, "--cal-samples %" PRIu64 ": out of memory "
                         "preallocating calibration samples\n", c.cal_samples);
            return 2;
        }
        const std::vector<int64_t> srt_pair = sorted_samples(pair);
        const std::vector<int64_t> srt_empty = sorted_samples(empty);
        const double pair_med =
            static_cast<double>(percentile_rank(srt_pair, 0.50));
        const double pair_p99 =
            static_cast<double>(percentile_rank(srt_pair, 0.99));
        const double empty_med =
            static_cast<double>(percentile_rank(srt_empty, 0.50));
        const double empty_p99 =
            static_cast<double>(percentile_rank(srt_empty, 0.99));
        std::printf(
            "# Phase 4 calibration (no book work; NOT auto-subtracted from samples).\n"
            "# group clock_pair_*: two steady_clock reads separated by a compiler\n"
            "# barrier — the pure timer floor of one clock-read pair.\n"
            "clock_pair_samples=%zu\n"
            "clock_pair_median_ns=%.3f\nclock_pair_p99_ns=%.3f\n"
            "clock_pair_max_ns=%.3f\n"
            "# group empty_batch_harness_*: clock-read, %" PRIu64
            " empty iterations with a barrier + sink, clock-read — the whole batch\n"
            "# timing skeleton with NO book work (NOT just the clock cost).\n"
            "empty_batch_harness_samples=%zu\n"
            "empty_batch_harness_median_ns=%.3f\n"
            "empty_batch_harness_p99_ns=%.3f\n"
            "empty_batch_harness_max_ns=%.3f\n"
            "note=neither group is auto-subtracted from latency samples; if "
            "empty_batch_harness is material vs a typical batch duration, use a "
            "larger batch (docs).\n",
            pair.size(), pair_med, pair_p99,
            static_cast<double>(srt_pair.back()), c.batch, empty.size(), empty_med,
            empty_p99, static_cast<double>(srt_empty.back()));
        // Consume the sink so the loop body cannot be optimized away.
        return sink == 0x9E37'79B9'7F4A'7C15ULL ? 1 : 0;
    }

    if (c.map) return run_distribution<MapOrderBook>(c);
    return run_distribution<FlatOrderBook>(c);
}
