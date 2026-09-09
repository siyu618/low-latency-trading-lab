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
// code and caches are warm but the measured run still starts from the original
// full snapshot — no part of the measured stream is consumed early.
//
// Partial final batch: if updates % batch_size is nonzero, the trailing partial
// batch is applied and timed as its OWN sample so it is recorded with its ACTUAL
// operation count (it is NEVER normalized by the full batch size). It is
// EXCLUDED from the percentile distribution — a short batch would carry
// disproportionate timer-boundary noise into the tail. It appears in the raw CSV
// as the final row (batch_operations < batch_size) and in the summary as explicit
// partial_* keys. All updates are always applied, so the measured book's final
// state is the true post-stream state.
//
// Timer-boundary calibration (--calibrate): measures the same timing skeleton
// with NO book work, many times, to estimate the fixed steady_clock boundary
// overhead and to check the batch is large enough. Reported: median and p99
// boundary overhead, and the overhead / typical-batch-duration ratio. The
// overhead is NOT auto-subtracted from samples (that would be an unverified
// correction); if it is material for flat at batch_size=512, the docs recommend
// a larger batch.
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
//     --workload A..E        which workload   (REQUIRED)
//     --levels N             scale in price levels (REQUIRED)
//     --updates N            total steady ops applied (default 10000000)
//     --batch-size N         updates per timed batch (default 512); updates must
//                            divide evenly by batch_size
//     --seed N               stream seed (default = Phase 2 seed; reproduces the
//                            exact Phase 2 stream for the same wl/levels/updates)
//     --samples-out FILE     write raw CSV samples here after measurement
//                            (default: no file)
//     --stats-out FILE       write a key:value summary here after measurement
//                            (default: stdout)
//     --calibrate            run the timer-boundary calibration experiment and
//                            exit (no book, no stream, no distribution)
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
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Phase 4 percentile / normalization helpers (shared with the unit tests).
using llob_tail::percentile_rank;
using llob_tail::sorted_samples;
using llob_tail::normalized_ns_per_update;

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
// Timer-boundary calibration: measure the same timing skeleton with no book
// work, `samples` times. Each pass reads the clock twice around an empty loop of
// `batch_size` "iterations" (the book-apply site is replaced by an opaque sink
// increment so the loop body still exists), then records the elapsed ns. The
// returned samples are the per-batch boundary overhead estimate.
// ---------------------------------------------------------------------------
std::vector<int64_t> calibrate_boundary(uint64_t batch_size, uint64_t samples,
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
        "  --calibrate [--cal-samples N]  timer-boundary calibration, then exit\n"
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

bool parse_u64(const char* s, const char* what, uint64_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        std::fprintf(stderr, "invalid %s: '%s'\n", what, s);
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

bool parse_i64(const char* s, const char* what, int64_t& out) {
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (end == s || *end != '\0') {
        std::fprintf(stderr, "invalid %s: '%s'\n", what, s);
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
            const char wl = v[0];
            if (wl == 'A') c.wl = Workload::A;
            else if (wl == 'B') c.wl = Workload::B;
            else if (wl == 'C') c.wl = Workload::C;
            else if (wl == 'D') c.wl = Workload::D;
            else if (wl == 'E') c.wl = Workload::E;
            else { std::fprintf(stderr, "--workload must be A..E (got '%s')\n", v); return false; }
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

// One timed batch. `ops` is the number of updates applied in it: c.batch for a
// full batch, or the ACTUAL trailing remainder (updates % batch) for a partial
// final batch. elapsed_ns is the raw integer steady_clock duration (source of
// truth; never rounded before analysis).
struct BatchSample {
    uint64_t ops;
    int64_t  elapsed_ns;
};

// ---------------------------------------------------------------------------
// One full distribution run for one book over one stream.
// ---------------------------------------------------------------------------
template <typename Book>
int run_distribution(const Config& c) {
    const char* impl = c.map ? "map" : "flat";
    const int64_t tick_max = 2 * c.levels;

    // Setup — entirely OUTSIDE measurement.
    const BookSnapshot snap = StreamGen::fill_snapshot(c.levels);
    const std::vector<L2Update> ops =
        StreamGen::steady_ops(c.wl, c.levels, c.updates, c.seed);

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
    std::vector<BatchSample> samples;
    samples.reserve(full_batches + (rem ? 1u : 0u));

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

    // End-state reads (off the clock): make the final state observable and
    // validate it is a legal, synced book (the same guarantees Phase 2 keeps).
    const bool synced   = book.synced();
    const uint64_t seq  = book.last_applied_seq();
    const int64_t  bb    = book.best_bid();
    const int64_t  bq    = book.best_bid_qty();
    const int64_t  ba    = book.best_ask();
    const int64_t  aq    = book.best_ask_qty();
    const size_t   lvl   = book.level_count();

    // The final observable read of the accumulator: never taken on a real
    // stream, but makes every apply() genuinely influence program behavior so
    // the optimizer cannot drop the timed loop.
    if (sink == 0x9E37'79B9'7F4A'7C15ULL) {
        std::fprintf(stderr, "unreachable sink guard tripped\n");
        return 2;
    }

    // ---- Post-measurement analysis (no timing here) ----
    // Distribution metrics are computed over FULL batches only (batch_size ops
    // each). A trailing partial batch — if any — is recorded in the CSV and
    // surfaced in the summary as explicit partial_* keys, but EXCLUDED here: a
    // short batch would carry disproportionate timer-boundary noise into the
    // tail. The raw elapsed_ns values are the source of truth and are normalized
    // only by this derived step, never prematurely rounded.
    const double batch_ns = static_cast<double>(c.batch); // normalization factor
    std::vector<int64_t> full_elapsed;
    full_elapsed.reserve(full_batches);
    for (const BatchSample& s : samples) full_elapsed.push_back(s.elapsed_ns);
    // (samples.size() == full_batches + (rem ? 1 : 0); when rem, the last sample
    //  is the partial one and is not copied into full_elapsed.)

    const std::vector<int64_t> srt = sorted_samples(full_elapsed);
    const double p50 = static_cast<double>(percentile_rank(srt, 0.50)) / batch_ns;
    const double p90 = static_cast<double>(percentile_rank(srt, 0.90)) / batch_ns;
    const double p99 = static_cast<double>(percentile_rank(srt, 0.99)) / batch_ns;
    const double p999 = static_cast<double>(percentile_rank(srt, 0.999)) / batch_ns;

    double sum = 0.0;
    for (int64_t s : full_elapsed) sum += static_cast<double>(s); // FULL batches only
    const double mean_ns =
        sum / static_cast<double>(full_batches) / batch_ns; // over full batches only
    const double min_ns = static_cast<double>(srt.front()) / batch_ns;
    const double max_ns = static_cast<double>(srt.back()) / batch_ns;

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
                     "partial batch, which is EXCLUDED from the percentile\n"
                     "# distribution in the summary).\n"
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
                 "final_synced=%d\nfinal_seq=%" PRIu64
                 "\nfinal_best_bid=%" PRId64 "/%" PRId64
                 "\nfinal_best_ask=%" PRId64 "/%" PRId64
                 "\nfinal_level_count=%zu\n"
                 "percentile_definition=nearest-rank on sorted FULL-batch durations "
                 "(ceil(q*N)-th smallest); a trailing partial batch is recorded in "
                 "the raw CSV but excluded from the distribution\n",
                 c.batch, impl, workload_tag(c.wl), workload_desc(c.wl), c.levels,
                 c.updates, c.batch, c.seed, impl, workload_tag(c.wl),
                 workload_desc(c.wl), c.levels, c.updates, c.batch, c.seed,
                 samples.size(), full_batches, rem ? 1u : 0u, rem,
                 partial_ns_per_update, mean_ns, min_ns, p50, p90, p99, p999,
                 max_ns, ratio_p99_p50, ratio_p999_p50, ratio_max_p50,
                 synced ? 1 : 0, seq, bb, bq, ba, aq, lvl);
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
        // Timer-boundary calibration: the same timing skeleton with no book
        // work. Reports the median / p99 boundary overhead per batch and, when a
        // batch size is given, the overhead-relative-to-a-batch estimate. This
        // does NOT auto-subtract from latency samples.
        uint64_t sink = 0;
        const std::vector<int64_t> cal = calibrate_boundary(c.batch, c.cal_samples, sink);
        const std::vector<int64_t> srt = sorted_samples(cal);
        const double med = static_cast<double>(percentile_rank(srt, 0.50));
        const double p99 = static_cast<double>(percentile_rank(srt, 0.99));
        std::printf(
            "# Phase 4 timer-boundary calibration (empty timing skeleton, no book).\n"
            "# Each sample: clock-read, %" PRIu64
            " empty iterations with a barrier, clock-read.\n"
            "calibration_samples=%zu\n"
            "boundary_median_ns=%.3f\nboundary_p99_ns=%.3f\n"
            "boundary_max_ns=%.3f\n"
            "note=overhead is NOT auto-subtracted from latency samples; if it is "
            "material vs a typical batch duration, use a larger batch (docs).\n",
            c.batch, cal.size(), med, p99,
            static_cast<double>(srt.back()));
        // Consume the sink so the loop body cannot be optimized away.
        return sink == 0x9E37'79B9'7F4A'7C15ULL ? 1 : 0;
    }

    if (c.map) return run_distribution<MapOrderBook>(c);
    return run_distribution<FlatOrderBook>(c);
}
