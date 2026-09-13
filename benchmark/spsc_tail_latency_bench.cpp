// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 4: TAIL LATENCY / JITTER.
//
// A SEPARATE executable from every earlier phase. It does not overload the
// Experiment-01 Phase-4 tail benchmark (orderbook_tail_bench) and it does not
// overload the Phase-2 throughput or the Phase-3A/3B cursor benches. Nothing
// here modifies a frozen queue implementation, benchmark dataset or historical
// result, and it introduces NO new queue optimization.
//
// The PURE harness logic — sampling schedule, derived sample count, tick->ns
// conversion, payload validators, nearest-rank summary and the PASS invariants —
// lives in benchmark/spsc_tail_harness.h so that it can be tested directly
// instead of being trusted inside main(). What stays here is the part that
// cannot be unit-tested: the threads, the real queue, the measured interval and
// the file output.
//
// ---------------------------------------------------------------------------
// WHICH QUEUE, AND WHY ONLY ONE
// ---------------------------------------------------------------------------
// The measured queue is the SEPARATED-CURSOR BASELINE SPSC ring buffer:
//
//     lltl::SpscSeparatedBaselineRingBuffer<Msg, Capacity>
//
// acquire/release publication, separated head/tail cache lines, baseline remote
// cursor loads, NO remote-cursor cache. The Phase-3B cached variant is NOT the
// production candidate and is deliberately absent from this matrix; so are the
// mutex queue and the Phase-3A same-line layout. A single-queue matrix keeps
// every Phase-4 number attributable to one queue.
//
// ---------------------------------------------------------------------------
// WHAT THE MEASURED QUANTITY IS, EXACTLY
// ---------------------------------------------------------------------------
// `producer_ready -> consumer_received`. Precisely:
//
//   * the producer stamps the clock IMMEDIATELY BEFORE entering its try_push
//     retry loop for message s, so the stamp precedes every push attempt for
//     that message, including all of its failed attempts;
//   * the consumer stamps the clock IMMEDIATELY AFTER a successful try_pop
//     returns, BEFORE any sequence check, checksum fold or payload validation.
//
// It is therefore NOT "pure queue residence time" and must never be described as
// such. The number INCLUDES the producer's full retry/backpressure wait, the
// queue's release/acquire synchronization, the payload copy, the consumer's own
// empty-retry loop, and both clock reads. It is also NOT a per-call queue cost.
//
// HOW THE TIMESTAMP CROSSES THE THREAD BOUNDARY. The stamp is written to a
// `Capacity`-entry array indexed by `s & (Capacity - 1)`, BEFORE the push that
// publishes the message. The consumer reads that entry only after a successful
// pop, i.e. after an acquire observation of `head`. The release store to `head`
// that publishes the payload therefore also publishes the stamp, so the consumer
// cannot read a stamp that was not yet written. Slot reuse is bounded by ring
// semantics: the producer cannot rewrite slot k until the consumer has advanced
// past it. The array is allocated BEFORE the timed transfer and never resized.
// It is measurement scaffolding, not part of the queue.
//
// ONE CLOCK DOMAIN. std::chrono::steady_clock only. std::chrono::system_clock is
// NEVER used, not even for labelling: it is not monotonic, and a single backward
// step would silently corrupt a sample. Latencies are stored as RAW duration
// counts (ticks) and converted to nanoseconds outside the timed transfer.
//
// ---------------------------------------------------------------------------
// SAMPLING, SETTLING AND WHAT IS PREALLOCATED
// ---------------------------------------------------------------------------
// Sampling is a deterministic ODD-interval countdown (default 1021, not 1024),
// so it is coprime with every power-of-two capacity and rotates through all ring
// positions. A `--settling` prefix runs through the REAL queue but is excluded
// from the distribution. The expected sample count is DERIVED, not observed, and
// a repetition that observes a different count FAILS.
//
// The clock is read UNCONDITIONALLY on every message, settled or sampled: the
// sampling decision decides only whether a latency is RECORDED, never whether it
// is MEASURED, so the measurement point is identical for every message.
//
// All sample storage is sized exactly, before the transfer starts. Inside the
// transfer there is NO allocation, NO vector growth, NO sorting and NO file or
// console I/O. Sorting happens after the threads join; files after that.
//
// ---------------------------------------------------------------------------
// WHAT THIS BENCHMARK DOES NOT DO
// ---------------------------------------------------------------------------
//   * It does not subtract the timer calibration. The calibration is reported
//     for scale and context only; it is not a correction factor.
//   * It does not collect hardware performance counters, and nothing in its
//     output may be described in terms of cache misses, coherence events,
//     preemption or core type. Those are not measured here.
//   * It does not pool repetitions into one distribution. Each repetition keeps
//     its own raw samples.
// ---------------------------------------------------------------------------

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "spsc_remote_cursor_ring_buffer.h"
#include "spsc_tail_harness.h"

namespace {

using Clock = std::chrono::steady_clock;
namespace h = lltl_tail;

static_assert(Clock::is_steady,
              "Phase 4 requires a monotonic clock; steady_clock is not steady "
              "on this implementation, so Phase-4 latencies would not be "
              "measurable here");

// Exact tick -> nanosecond conversion. If the platform's steady_clock period
// does not divide into whole nanoseconds this fails the BUILD rather than
// silently rounding every sample: rounding in the conversion would move tail
// values for a reason that has nothing to do with the queue.
constexpr std::int64_t kNsPerTick =
    h::ns_per_tick(Clock::duration::period::num, Clock::duration::period::den);
static_assert(kNsPerTick > 0,
              "steady_clock period does not convert to whole nanoseconds; "
              "Phase 4 refuses to round its samples");

constexpr std::int64_t to_ns(std::int64_t ticks) noexcept {
    return h::ticks_to_ns(ticks, kNsPerTick);
}

using h::kDefaultCalibReps;
using h::kDefaultInterval;
using h::kDefaultMessages;
using h::kDefaultReps;
using h::kDefaultSettling;
using h::kDefaultWarmup;
using h::kYieldAfterMisses;

constexpr int kDefaultSession = 1;

// Exit codes, distinct so a caller can tell "this experiment was invalid" from
// "the queue was wrong".
constexpr int kExitOk              = 0;
constexpr int kExitUsage           = 2;
constexpr int kExitLayoutFailed    = 3; // cursor placement is not the verified one
constexpr int kExitMeasurementFail = 4; // a repetition failed its own invariants

constexpr const char* kQueueName = "SpscSeparatedBaselineRingBuffer";

[[noreturn]] void usage(const char* argv0) {
    std::fprintf(
        stderr,
        "Experiment 02 Phase 4 — SPSC tail latency / jitter over the\n"
        "SEPARATED-CURSOR BASELINE queue (no remote-cursor cache).\n"
        "\n"
        "usage: %s --message-bytes=16|32|64 --capacity=1024|4096|65536\n"
        "          [--messages=%" PRIu64 "] [--reps=%d] [--warmup=%d]\n"
        "          [--sample-interval=%" PRIu64 "] [--settling=%" PRIu64 "]\n"
        "          [--session=%d] [--calibration-reps=%" PRIu64 "]\n"
        "          [--reported-line-size=%zu]\n"
        "          [--raw-out=FILE] [--summary-out=FILE]\n"
        "          [--calibration-out=FILE]\n"
        "\n"
        "Measured quantity: producer_ready (immediately before the try_push\n"
        "retry loop) -> consumer_received (immediately after a successful\n"
        "try_pop, before any validation). This INCLUDES the producer's\n"
        "backpressure wait, the queue's release/acquire synchronization and\n"
        "the consumer's empty-retry loop. It is NOT pure queue residence.\n"
        "\n"
        "--sample-interval must be ODD, so that it is coprime with every\n"
        "power-of-two capacity and the deterministic countdown sample rotates\n"
        "through all ring positions. The default is 1021, not 1024.\n"
        "\n"
        "expected_samples = (messages - settling) / sample-interval, and a\n"
        "repetition whose observed sample count differs FAILS. --warmup\n"
        "repetitions run through the real queue and are excluded entirely.\n"
        "\n"
        "ONE (message_bytes, capacity) CELL PER PROCESS, by design, and one\n"
        "process per session; scripts/spsc-tail-latency.sh drives the full\n"
        "9-cell x 4-session matrix.\n",
        argv0, kDefaultMessages, kDefaultReps, kDefaultWarmup, kDefaultInterval,
        kDefaultSettling, kDefaultSession, kDefaultCalibReps,
        lltl::kAssumedCacheLineSize);
    std::exit(kExitUsage);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::size_t   message_bytes      = 0;
    std::size_t   capacity           = 0;
    std::uint64_t messages           = kDefaultMessages;
    int           reps               = kDefaultReps;
    int           warmup             = kDefaultWarmup;
    std::uint64_t interval           = kDefaultInterval;
    std::uint64_t settling           = kDefaultSettling;
    int           session            = kDefaultSession;
    std::uint64_t calib_reps         = kDefaultCalibReps;
    std::size_t   reported_line_size = lltl::kAssumedCacheLineSize;
    std::string   raw_out;
    std::string   summary_out;
    std::string   calib_out;
};

// ---------------------------------------------------------------------------
// Timer calibration — OUTSIDE the timed transfer, on the SAME clock.
//
// This measures back-to-back steady_clock::now() pairs. It establishes the SCALE
// of the timer's own cost so a reader can judge how much of a small tail value
// could be instrumentation. It is DESCRIPTIVE ONLY and is NEVER subtracted from
// a measured latency: subtracting it would assume a calibration read and a
// hot-path read cost the same, which is not established here.
// ---------------------------------------------------------------------------
std::vector<std::int64_t> calibrate(std::uint64_t n) {
    std::vector<std::int64_t> d(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
        const Clock::time_point a = Clock::now();
        const Clock::time_point b = Clock::now();
        d[static_cast<std::size_t>(i)] =
            static_cast<std::int64_t>((b - a).count());
    }
    return d;
}

// ---------------------------------------------------------------------------
// One repetition: a full transfer of `messages` through the real queue.
//
// `collect` false is a WARM-UP: identical work through the identical queue, but
// its samples are discarded and never reach a file.
// ---------------------------------------------------------------------------

struct RepResult {
    int           repetition            = 0;
    bool          collect               = false;
    std::uint64_t expected_delivered    = 0;
    std::uint64_t expected_samples      = 0;
    std::uint64_t sample_count          = 0;
    std::uint64_t delivered             = 0;
    std::uint64_t producer_full_retries = 0;
    std::uint64_t consumer_empty_retries = 0;
    std::uint64_t checksum              = 0;
    std::uint64_t payload_mismatches    = 0;
    std::uint64_t timestamp_inversions  = 0;
    std::uint64_t elapsed_ns            = 0;
    bool          sequence_ok           = true;
    bool          cursor_ok             = true;
    bool          layout_checked        = false;
    std::string   layout_summary;
    std::vector<std::int64_t> samples; // RAW TICKS; empty when !collect

    h::RepInvariants invariants() const noexcept {
        h::RepInvariants v;
        v.expected_delivered   = expected_delivered;
        v.delivered            = delivered;
        v.expected_samples     = expected_samples;
        v.sample_count         = sample_count;
        v.payload_mismatches   = payload_mismatches;
        v.timestamp_inversions = timestamp_inversions;
        v.sequence_ok          = sequence_ok;
        v.cursor_ok            = layout_checked && cursor_ok;
        return v;
    }
    bool ok() const noexcept { return h::repetition_ok(invariants()); }
};

template <typename Msg, std::size_t Capacity>
RepResult run_repetition(const Config& c, int repetition, bool collect) {
    static_assert(Capacity > 0, "capacity must be positive");
    using Queue = lltl::SpscSeparatedBaselineRingBuffer<Msg, Capacity>;

    const std::uint64_t n        = c.messages;
    const std::uint64_t settling = c.settling;
    const std::uint64_t interval = c.interval;
    const std::uint64_t expected_samples =
        h::expected_sample_count(n, settling, interval);

    RepResult r;
    r.repetition         = repetition;
    r.collect            = collect;
    r.expected_delivered = n;
    r.expected_samples   = expected_samples;

    auto q = std::make_unique<Queue>();

    // Runtime layout verification on the object that is ABOUT TO BE MEASURED.
    // A fresh queue is allocated per repetition, so the cursor ADDRESSES change
    // between repetitions; the Phase-3A separated invariant is therefore
    // re-checked here rather than asserted once from the type. Phase 4 measures
    // the baseline queue, so the gate is the Phase-3A cursor claim; the Phase-3B
    // cached-placement claim is reported but NOT gated, because it belongs to a
    // treatment this matrix does not contain.
    {
        const lltl::RemoteCursorLayoutReport layout =
            q->cursor_layout_report(c.reported_line_size);
        r.layout_checked = true;
        r.cursor_ok      = layout.cursor.ok();
        r.layout_summary = layout.summary_line();
        std::fprintf(stderr, "[layout bytes=%zu cap=%zu rep=%d] %s\n",
                     Msg::kBytes, Capacity, repetition,
                     r.layout_summary.c_str());
        if (!r.cursor_ok) {
            std::fprintf(stderr,
                         "\nCURSOR LAYOUT INVARIANT FAILED (bytes=%zu cap=%zu "
                         "rep=%d).\n"
                         "Phase 4 measures the SEPARATED-CURSOR BASELINE queue, "
                         "so an unverified separated cursor placement is a "
                         "FAILED EXPERIMENT, not a slow cell. Exiting non-zero "
                         "without timing anything.\n",
                         Msg::kBytes, Capacity, repetition);
            return r;
        }
    }

    // Measurement scaffolding, allocated BEFORE the transfer: one raw tick stamp
    // per message, written by the producer before it publishes the message and
    // read by the consumer after it acquires that message.
    //
    // WHY 2 * Capacity SLOTS AND NOT Capacity.
    //
    // Indexing this array by the ring slot (`s & (Capacity - 1)`) is WRONG, and
    // wrong in a way that corrupts samples rather than failing loudly. The
    // producer must write the stamp for message s before it pushes s, so it can
    // write the stamp for index `h + Capacity` while the consumer's head is
    // still h — the ring allows Capacity messages in flight, and the producer
    // writes its stamp for the message it is ABOUT to push, one past the last
    // one it managed to publish. With Capacity slots that write lands on exactly
    // the slot holding the stamp for index h, i.e. on the stamp the consumer is
    // about to read for the message it just popped. The consumer then subtracts
    // a LATER timestamp from its own and computes a negative latency — a sample
    // that is not merely noisy but impossible. It is rare (roughly one in 5e7
    // samples on this host) because the window between the pop and the stamp
    // read is a few instructions, which is precisely what makes it dangerous.
    //
    // With 2 * Capacity slots addressed by a power-of-two mask, let the consumer
    // have just popped index c, so the shared head is c + 1. The producer can
    // publish at most index head + Capacity - 1 and can therefore stamp at most
    // index head + Capacity = c + 1 + Capacity. The next index sharing c's slot
    // is c + 2 * Capacity, which is unreachable because c + 1 + Capacity <
    // c + 2 * Capacity for every Capacity >= 2. So the stamp for index c is
    // intact from the moment it is written until the moment it is read.
    //
    // Visibility is the ring's own release/acquire pair, exactly as for the
    // payload: the stamp store is sequenced before the release store that
    // publishes the message, and the consumer's acquire load of the cursor
    // synchronizes with it. A plain (non-atomic) store and load are therefore
    // correctly ordered, not a race.
    constexpr std::size_t kStampSlots = 2 * Capacity;
    static_assert(kStampSlots > Capacity, "the stamp array must be larger than "
                                          "the ring, or a live stamp can be "
                                          "overwritten before it is read");
    auto ready_ticks = std::make_unique<std::int64_t[]>(kStampSlots);

    // Sample storage, sized EXACTLY, before the transfer. Inside the transfer
    // there is no growth, no allocation, no sorting and no I/O.
    std::vector<std::int64_t> samples;
    if (collect) samples.assign(static_cast<std::size_t>(expected_samples), 0);

    std::atomic<bool> start{false};
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> consumer_ready{false};

    std::uint64_t producer_full_retries  = 0;
    std::uint64_t consumer_empty_retries = 0;
    std::uint64_t checksum               = 0;
    std::uint64_t delivered              = 0;
    std::uint64_t sample_count           = 0;
    std::uint64_t payload_mismatches     = 0;
    std::uint64_t timestamp_inversions   = 0;
    bool          sequence_ok            = true;
    Clock::time_point consumer_end{};

    std::thread producer([&] {
        producer_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield(); // ready-wait: NOT part of the measurement
        }
        std::uint64_t misses = 0;
        for (std::uint64_t s = 0; s < n; ++s) {
            const Msg m = Msg::make(s);
            // MEASUREMENT POINT: immediately before the retry loop, so the stamp
            // precedes every push attempt for this message. It is written before
            // the push that publishes the message, and the consumer's acquire
            // observation of `head` publishes it. Indexed over 2 * Capacity
            // slots — see the allocation comment for why Capacity slots would
            // let this write corrupt a stamp the consumer has not read yet.
            ready_ticks[s & (kStampSlots - 1)] = static_cast<std::int64_t>(
                Clock::now().time_since_epoch().count());
            while (!q->try_push(m)) {
                ++producer_full_retries;
                if (++misses >= kYieldAfterMisses) {
                    misses = 0;
                    std::this_thread::yield();
                }
            }
            misses = 0; // success: the consecutive-miss run is over
        }
    });

    std::thread consumer([&] {
        consumer_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield(); // ready-wait: NOT part of the measurement
        }
        h::SamplingCountdown sampler(interval);
        std::uint64_t        expected = 0;
        std::uint64_t        misses   = 0;
        Msg                  m{};
        while (expected < n) {
            if (!q->try_pop(m)) {
                ++consumer_empty_retries;
                if (++misses >= kYieldAfterMisses) {
                    misses = 0;
                    std::this_thread::yield();
                }
                continue;
            }
            // MEASUREMENT POINT: immediately after a successful pop, BEFORE any
            // sequence check, checksum fold or payload validation. Taken for
            // EVERY message; the sampler below only decides whether the latency
            // is recorded.
            const std::int64_t received = static_cast<std::int64_t>(
                Clock::now().time_since_epoch().count());
            misses = 0; // success: the consecutive-miss run is over

            if (expected >= settling && sampler.on_message()) {
                const std::int64_t lat =
                    received - ready_ticks[expected & (kStampSlots - 1)];
                if (!h::timestamp_ok(lat)) ++timestamp_inversions;
                // Bounded by the storage that ACTUALLY exists, not by the
                // derived count: a warm-up repetition runs the same schedule
                // with an EMPTY sample vector, and must still count what the
                // schedule produced so its own invariant is checked.
                if (sample_count < samples.size()) {
                    samples[static_cast<std::size_t>(sample_count)] = lat;
                }
                ++sample_count;
            }

            if (sequence_ok && m.seq == expected) {
                if (!m.valid_for(expected)) ++payload_mismatches;
                checksum = m.fold(checksum);
            } else {
                sequence_ok = false;
            }
            ++expected; // count regardless, so a failure can never hang join()
        }
        delivered    = expected;
        consumer_end = Clock::now();
    });

    while (!producer_ready.load(std::memory_order_acquire) ||
           !consumer_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // ---- timed interval: both threads are ready and parked on `start` ----
    const Clock::time_point t0 = Clock::now();
    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();
    // ---- end of timed interval; the CONSUMER took the end stamp itself ----

    r.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(consumer_end - t0)
            .count());
    r.delivered              = delivered;
    r.producer_full_retries  = producer_full_retries;
    r.consumer_empty_retries = consumer_empty_retries;
    r.checksum               = checksum;
    r.payload_mismatches     = payload_mismatches;
    r.timestamp_inversions   = timestamp_inversions;
    r.sequence_ok            = sequence_ok;
    r.sample_count           = sample_count;
    if (collect) r.samples = std::move(samples);
    return r;
}

// ---------------------------------------------------------------------------
// Output — all of it after both threads have joined.
// ---------------------------------------------------------------------------

void write_calibration(const Config& c, const std::vector<std::int64_t>& cal) {
    if (c.calib_out.empty()) return;
    std::FILE* f = std::fopen(c.calib_out.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open --calibration-out=%s\n",
                     c.calib_out.c_str());
        std::exit(kExitMeasurementFail);
    }
    const h::Distribution d = h::summarize(cal);
    std::fprintf(
        f,
        "# Experiment 02 Phase 4 — TIMER CALIBRATION (descriptive only).\n"
        "# Back-to-back std::chrono::steady_clock::now() pairs, taken OUTSIDE "
        "the timed transfer.\n"
        "# This is NOT subtracted from any measured latency and is NOT a "
        "correction factor.\n"
        "# It measures the timer's own scale, not the queue's cost.\n"
        "# clock=steady_clock steady=%d ns_per_tick=%" PRId64 "\n"
        "# samples=%" PRIu64 " min_ns=%" PRId64 " mean_ns=%.3f p50_ns=%" PRId64
        " p90_ns=%" PRId64 " p99_ns=%" PRId64 " p999_ns=%" PRId64
        " max_ns=%" PRId64 "\n"
        "pair_index,pair_cost_ns\n",
        Clock::is_steady ? 1 : 0, kNsPerTick, d.count, to_ns(d.min),
        d.mean * static_cast<double>(kNsPerTick), to_ns(d.p50), to_ns(d.p90),
        to_ns(d.p99), to_ns(d.p999), to_ns(d.max));
    for (std::size_t i = 0; i < cal.size(); ++i) {
        std::fprintf(f, "%zu,%" PRId64 "\n", i, to_ns(cal[i]));
    }
    std::fclose(f);
}

void write_raw(const Config& c, const std::vector<RepResult>& reps) {
    if (c.raw_out.empty()) return;
    std::FILE* f = std::fopen(c.raw_out.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open --raw-out=%s\n", c.raw_out.c_str());
        std::exit(kExitMeasurementFail);
    }
    std::fprintf(
        f,
        "# Experiment 02 Phase 4 — RAW SAMPLED LATENCIES, one row per sample.\n"
        "# measured quantity = producer_ready -> consumer_received "
        "(std::chrono::steady_clock).\n"
        "# It INCLUDES the producer's retry/backpressure wait, the queue's "
        "release/acquire\n"
        "# synchronization, the payload copy and the consumer's empty-retry "
        "loop. It is NOT\n"
        "# pure queue residence time, and it is NOT a per-call queue cost.\n"
        "# queue=%s (separated cursors, baseline remote cursor loads, no "
        "remote-cursor cache)\n"
        "# session=%d message_bytes=%zu capacity=%zu messages=%" PRIu64
        " settling_prefix=%" PRIu64
        " sample_interval=%" PRIu64 " warmup_reps=%d measured_reps=%d\n"
        "# Every distribution here is PER REPETITION. No repetition is pooled "
        "into another.\n"
        "# latency_ticks is the RAW steady_clock duration count, unrounded. "
        "latency_ns is\n"
        "# ticks * %" PRId64 ", an EXACT integer conversion (the build fails if "
        "the period does\n"
        "# not divide into whole nanoseconds), so latency_ns is never a rounded "
        "latency.\n"
        "# This file is written only after every thread has joined; nothing in "
        "the timed\n"
        "# transfer performs I/O.\n"
        "session,message_bytes,capacity,repetition,sample_index,latency_ticks,"
        "latency_ns\n",
        kQueueName, c.session, c.message_bytes, c.capacity, c.messages,
        c.settling, c.interval, c.warmup, c.reps, kNsPerTick);
    for (const RepResult& r : reps) {
        if (!r.collect) continue;
        for (std::size_t i = 0; i < r.samples.size(); ++i) {
            std::fprintf(f, "%d,%zu,%zu,%d,%zu,%" PRId64 ",%" PRId64 "\n",
                         c.session, c.message_bytes, c.capacity, r.repetition,
                         i, r.samples[i], to_ns(r.samples[i]));
        }
    }
    std::fclose(f);
}

void write_summary(const Config& c, const std::vector<RepResult>& reps) {
    if (c.summary_out.empty()) return;
    std::FILE* f = std::fopen(c.summary_out.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open --summary-out=%s\n",
                     c.summary_out.c_str());
        std::exit(kExitMeasurementFail);
    }
    std::fprintf(
        f,
        "# Experiment 02 Phase 4 — PER-REPETITION LATENCY SUMMARY.\n"
        "# One row per MEASURED repetition. The warm-up repetition is not "
        "summarized here at all.\n"
        "# Percentiles are nearest-rank on the observed samples: "
        "index(p) = ceil(p*N) - 1.\n"
        "# P99.99 is deliberately not reported. No pooled distribution is "
        "produced.\n"
        "# ns_per_message = elapsed_ns / messages delivered: an END-TO-END rate "
        "over the whole\n"
        "# repetition, NOT a latency, and NOT comparable with Phase-2/3 "
        "absolute values.\n"
        "# expected_samples = (messages - settling_prefix) / sample_interval; a "
        "row whose\n"
        "# sample_count differs from it is a FAILURE, not a smaller dataset.\n"
        "# queue=%s session=%d message_bytes=%zu capacity=%zu messages=%" PRIu64
        " settling_prefix=%" PRIu64
        " sample_interval=%" PRIu64 " warmup_reps=%d measured_reps=%d\n"
        "session,message_bytes,capacity,repetition,status,expected_samples,"
        "sample_count,min_ns,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,"
        "elapsed_ns,ns_per_message,delivered,producer_full_retries,"
        "consumer_empty_retries,checksum,sequence_ok,correctness\n",
        kQueueName, c.session, c.message_bytes, c.capacity, c.messages,
        c.settling, c.interval, c.warmup, c.reps);

    for (const RepResult& r : reps) {
        if (!r.collect) continue;
        const h::Distribution d  = h::summarize(r.samples);
        const bool            ok = r.ok();
        const double          per_msg =
            r.delivered == 0 ? 0.0
                             : static_cast<double>(r.elapsed_ns) /
                                   static_cast<double>(r.delivered);
        std::fprintf(
            f,
            "%d,%zu,%zu,%d,%s,%" PRIu64 ",%" PRIu64 ",%" PRId64
            ",%.3f,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
            ",%" PRIu64 ",%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%d,%s\n",
            c.session, c.message_bytes, c.capacity, r.repetition,
            ok ? "PASS" : "FAIL", r.expected_samples, r.sample_count,
            to_ns(d.min), d.mean * static_cast<double>(kNsPerTick), to_ns(d.p50),
            to_ns(d.p90), to_ns(d.p99), to_ns(d.p999), to_ns(d.max),
            r.elapsed_ns, per_msg, r.delivered, r.producer_full_retries,
            r.consumer_empty_retries, r.checksum, r.sequence_ok ? 1 : 0,
            ok ? "PASS" : "FAIL");
    }
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Cell runner
// ---------------------------------------------------------------------------

template <typename Msg, std::size_t Capacity>
int run_cell(const Config& c) {
    std::vector<RepResult> reps;
    reps.reserve(static_cast<std::size_t>(c.warmup + c.reps));

    // Warm-up first, then the measured repetitions. A warm-up is negative-
    // numbered so it can never be mistaken for a measured repetition in a log.
    for (int i = 0; i < c.warmup; ++i) {
        RepResult r = run_repetition<Msg, Capacity>(c, -(i + 1), false);
        if (!r.cursor_ok) return kExitLayoutFailed;
        reps.push_back(std::move(r));
    }
    for (int i = 0; i < c.reps; ++i) {
        RepResult r = run_repetition<Msg, Capacity>(c, i + 1, true);
        if (!r.cursor_ok) return kExitLayoutFailed;
        reps.push_back(std::move(r));
    }

    write_raw(c, reps);
    write_summary(c, reps);

    int failures = 0;
    for (const RepResult& r : reps) {
        const char* kind = r.collect ? "measured" : "warmup";
        if (!r.ok()) ++failures;
        std::fprintf(stderr,
                     "[%s rep %d] delivered=%" PRIu64 "/%" PRIu64
                     " samples=%" PRIu64 "/%" PRIu64
                     " elapsed=%" PRIu64 "ns full_retries=%" PRIu64
                     " empty_retries=%" PRIu64 " checksum=%" PRIu64
                     " seq_ok=%d payload_mismatch=%" PRIu64
                     " ts_inversions=%" PRIu64 " -> %s\n",
                     kind, r.repetition, r.delivered, r.expected_delivered,
                     r.sample_count, r.expected_samples, r.elapsed_ns,
                     r.producer_full_retries, r.consumer_empty_retries,
                     r.checksum, r.sequence_ok ? 1 : 0, r.payload_mismatches,
                     r.timestamp_inversions, r.ok() ? "PASS" : "FAIL");
    }

    if (failures != 0) {
        std::fprintf(stderr,
                     "\nPhase 4 cell FAILED: %d of %zu repetitions did not "
                     "satisfy their own invariants (delivery, sequence, payload "
                     "validation, timestamp ordering or expected sample count). "
                     "Exit code %d.\n",
                     failures, reps.size(), kExitMeasurementFail);
        return kExitMeasurementFail;
    }
    return kExitOk;
}

template <typename Msg>
int run_with_message(const Config& c) {
    switch (c.capacity) {
    case 1024:
        return run_cell<Msg, 1024>(c);
    case 4096:
        return run_cell<Msg, 4096>(c);
    case 65536:
        return run_cell<Msg, 65536>(c);
    default:
        return kExitUsage;
    }
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

bool parse_u64(const char* s, std::uint64_t& out) {
    if (s == nullptr || *s == '\0') return false;
    char*                     end = nullptr;
    const unsigned long long  v   = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

bool take(const char* arg, const char* key, const char*& value) {
    const std::size_t klen = std::strlen(key);
    if (std::strncmp(arg, key, klen) != 0) return false;
    if (arg[klen] != '=') return false;
    value = arg + klen + 1;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config      c;
    const char* value = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        std::uint64_t v = 0;
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage(argv[0]);
        } else if (take(a, "--message-bytes", value)) {
            if (!parse_u64(value, v)) usage(argv[0]);
            c.message_bytes = static_cast<std::size_t>(v);
        } else if (take(a, "--capacity", value)) {
            if (!parse_u64(value, v)) usage(argv[0]);
            c.capacity = static_cast<std::size_t>(v);
        } else if (take(a, "--messages", value)) {
            if (!parse_u64(value, c.messages)) usage(argv[0]);
        } else if (take(a, "--reps", value)) {
            if (!parse_u64(value, v)) usage(argv[0]);
            c.reps = static_cast<int>(v);
        } else if (take(a, "--warmup", value)) {
            if (!parse_u64(value, v)) usage(argv[0]);
            c.warmup = static_cast<int>(v);
        } else if (take(a, "--sample-interval", value)) {
            if (!parse_u64(value, c.interval)) usage(argv[0]);
        } else if (take(a, "--settling", value)) {
            if (!parse_u64(value, c.settling)) usage(argv[0]);
        } else if (take(a, "--session", value)) {
            if (!parse_u64(value, v)) usage(argv[0]);
            c.session = static_cast<int>(v);
        } else if (take(a, "--calibration-reps", value)) {
            if (!parse_u64(value, c.calib_reps)) usage(argv[0]);
        } else if (take(a, "--reported-line-size", value)) {
            if (!parse_u64(value, v)) usage(argv[0]);
            c.reported_line_size = static_cast<std::size_t>(v);
        } else if (take(a, "--raw-out", value)) {
            c.raw_out = value;
        } else if (take(a, "--summary-out", value)) {
            c.summary_out = value;
        } else if (take(a, "--calibration-out", value)) {
            c.calib_out = value;
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", a);
            usage(argv[0]);
        }
    }

    // ---- validation: a malformed Phase-4 run must not produce numbers ----
    const bool bytes_ok = c.message_bytes == 16 || c.message_bytes == 32 ||
                          c.message_bytes == 64;
    const bool cap_ok = c.capacity == 1024 || c.capacity == 4096 ||
                        c.capacity == 65536;
    if (!bytes_ok || !cap_ok) {
        std::fprintf(stderr,
                     "Phase 4 canonical matrix is message_bytes in "
                     "{16,32,64} x capacity in {1024,4096,65536}.\n");
        usage(argv[0]);
    }
    if (c.interval == 0 || c.interval % 2 == 0) {
        std::fprintf(stderr,
                     "--sample-interval must be a positive ODD number, so that "
                     "it is coprime with every power-of-two capacity and the "
                     "sample rotates through all ring positions.\n");
        usage(argv[0]);
    }
    if (c.reps < 1) {
        std::fprintf(stderr, "--reps must be >= 1\n");
        usage(argv[0]);
    }
    if (c.warmup < 0) {
        std::fprintf(stderr, "--warmup must be >= 0\n");
        usage(argv[0]);
    }
    if (c.settling >= c.messages) {
        std::fprintf(stderr,
                     "--settling (%" PRIu64
                     ") must be smaller than --messages (%" PRIu64 ")\n",
                     c.settling, c.messages);
        usage(argv[0]);
    }
    if (c.messages - c.settling < c.interval) {
        std::fprintf(
            stderr,
            "the measured region (%" PRIu64
            " messages) is shorter than one --sample-interval (%" PRIu64
            "), so it would produce zero samples\n",
            c.messages - c.settling, c.interval);
        usage(argv[0]);
    }
    if (c.calib_reps == 0) {
        std::fprintf(stderr, "--calibration-reps must be >= 1\n");
        usage(argv[0]);
    }

    const std::uint64_t expected_samples =
        h::expected_sample_count(c.messages, c.settling, c.interval);
    std::fprintf(
        stderr,
        "Phase 4 — TAIL LATENCY over %s\n"
        "  session=%d message_bytes=%zu capacity=%zu messages=%" PRIu64 "\n"
        "  settling_prefix=%" PRIu64 " sample_interval=%" PRIu64
        " -> expected_samples=%" PRIu64 " per measured repetition\n"
        "  warmup_reps=%d (excluded entirely) measured_reps=%d\n"
        "  measured quantity: producer_ready -> consumer_received; it INCLUDES "
        "backpressure,\n"
        "  queue synchronization and the consumer's retry loop — NOT pure queue "
        "residence.\n"
        "  clock: steady_clock, ns_per_tick=%" PRId64
        ". Timer calibration is reported separately and is NEVER subtracted.\n",
        kQueueName, c.session, c.message_bytes, c.capacity, c.messages,
        c.settling, c.interval, expected_samples, c.warmup, c.reps, kNsPerTick);

    // Calibration first, on the same clock, outside every timed transfer.
    const std::vector<std::int64_t> cal = calibrate(c.calib_reps);
    write_calibration(c, cal);

    if (c.message_bytes == 16) return run_with_message<h::Msg16>(c);
    if (c.message_bytes == 32) return run_with_message<h::Msg32>(c);
    return run_with_message<h::Msg64>(c);
}
