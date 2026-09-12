#pragma once

// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3A.
//
// Cache-line / interference-block utilities, and the reason this header exists.
//
// Phase 3A is a CONTROLLED false-sharing experiment: it changes exactly one
// variable (the cache-line placement of the two SPSC cursors) and attributes a
// measured difference to that variable. The whole attribution rests on the two
// layouts actually being what they claim to be at runtime. A wrong cache-line
// size invalidates BOTH controls at once and in opposite directions:
//
//   * if the assumed line is SMALLER than the real line, the "same line"
//     control is not same-line: its two cursors sit in two different real lines
//     and the experiment measures nothing;
//   * if the assumed line is LARGER than the real line, the "separated"
//     control is not separated: its two cursor blocks can fall inside one real
//     line and the experiment measures the opposite of what it claims.
//
// The canonical host makes this concrete rather than hypothetical. Apple M3 Max
// reports a cache-line size of 128 bytes (`sysctl hw.cachelinesize`), not the 64
// that most code assumes. Building Phase 3A on a hard-coded 64 would have
// silently produced a "separated" control whose cursor blocks share a line.
//
// Two guards therefore exist, and they are independent:
//
//   1. kAssumedCacheLineSize is the COMPILE-TIME layout assumption. Alignment
//      must be a compile-time constant, so this cannot be a runtime value.
//   2. reported_cache_line_size() queries the HOST at runtime. The experiment
//      requires reported <= assumed; if the host reports a line size LARGER
//      than the layout was built for, the run FAILS instead of publishing.
//
// The guards are necessary but not sufficient on their own, because `reported`
// could in principle be a non-divisor of the assumption. The decisive check is
// per-instance and address-based: the experiment measures the real addresses of
// both cursors, converts them to indices under the reported line size, and
// requires the layout invariant to hold for THAT object (see
// CursorLayoutReport / verify_cursor_layout in
// include/spsc_cursor_layout_ring_buffer.h). A cell whose invariant is false is
// never published.
//
// `std::hardware_destructive_interference_size` would be the standard spelling
// of the assumption, but it is not usable here: Apple clang 15 (the canonical
// toolchain) does not provide it, and where it does exist its value is
// implementation-defined and may not match the running host. A stated constant
// plus a runtime check is both portable and honest.
//
// Scope note: this header is Phase-3A-only tooling. Nothing in the frozen
// Phase-1 or Phase-2 code includes it.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace lltl {

// Compile-time layout assumption: the alignment used for every Phase-3A cursor
// block and for the payload array.
//
// 128 is chosen as the CONSERVATIVE value for the hosts this project builds on:
// it is the Apple M3 Max (canonical host) line size, and it is a multiple of
// the 64-byte line size used by the x86-64 machines the Phase-3L work targets.
// Over-aligning on a 64-byte host costs padding and changes no behaviour;
// under-aligning would break the same-line control, which is why the larger
// value is the safe one to state at compile time.
//
// This is an ASSUMPTION, not a measurement. Every Phase-3A run verifies it
// against reported_cache_line_size() before it is allowed to publish anything.
inline constexpr std::size_t kAssumedCacheLineSize = 128;

// The host's reported cache-line size, in bytes, or 0 if the host could not be
// queried. This is a real runtime query, deliberately not a compiled-in value.
//
// It is NOT on any hot path and is called once per process, before timing.
inline std::size_t reported_cache_line_size() noexcept {
#if defined(__APPLE__)
    // hw.cachelinesize is the documented Apple sysctl for the data cache line
    // size; it reports 128 on Apple silicon (M-series). Note that
    // _SC_LEVEL1_DCACHE_LINESIZE is defined by the macOS headers but sysconf()
    // does not implement it there, which is why Apple gets its own branch.
    std::size_t    value = 0;
    std::size_t    len   = sizeof(value);
    if (::sysctlbyname("hw.cachelinesize", &value, &len, nullptr, 0) == 0 &&
        value > 0) {
        return value;
    }
#endif

#if defined(_SC_LEVEL1_DCACHE_LINESIZE)
    const long queried = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (queried > 0) {
        return static_cast<std::size_t>(queried);
    }
#endif

    return 0; // unknown — callers must treat this as a failure, not as a default
}

// The guard required by the Phase-3A spec: a run may only publish if the host's
// real interference block is no larger than the layout was built for. A host
// reporting a LARGER line than kAssumedCacheLineSize would make the "separated"
// control share a line, so it must fail loudly rather than produce numbers.
constexpr bool line_size_supported(std::size_t reported) noexcept {
    return reported > 0 && reported <= kAssumedCacheLineSize;
}

// Which of two addresses are in the same interference block under a given line
// size. `line` must be a power of two (all real line sizes are). Both helpers
// treat line == 0 as "unknown", which compares unequal to everything.
inline std::size_t cache_line_index(std::uintptr_t address,
                                    std::size_t  line) noexcept {
    return line == 0 ? 0 : static_cast<std::size_t>(address / line);
}

inline bool same_cache_line(std::uintptr_t a, std::uintptr_t b,
                            std::size_t line) noexcept {
    return line != 0 && cache_line_index(a, line) == cache_line_index(b, line);
}

} // namespace lltl
