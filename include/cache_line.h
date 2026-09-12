#pragma once

// ---------------------------------------------------------------------------
// Experiment 02 — SPSC Ring Buffer / Concurrency, Phase 3A.
//
// Cache-line / interference-block utilities, and the reason this header exists.
//
// Phase 3A is a CONTROLLED cursor-placement experiment: it changes exactly one
// variable (the cache-line placement of the two SPSC cursors) and reports the
// measured difference. That report is only meaningful if the two layouts really
// are what they claim to be on the running host, so layout is established twice,
// by two independent mechanisms with clearly separated roles:
//
//   1. COMPILE TIME — kAssumedCacheLineSize drives the alignment of both cursor
//      policies and of the payload array. Alignment must be a compile-time
//      constant, so this cannot be a runtime value. The type-level assertions in
//      include/spsc_cursor_layout_ring_buffer.h then fix the intended CANDIDATE
//      layout: which members share an assumed block, and that both cursor
//      policies have the same footprint so the payload offset does not move.
//
//   2. RUNTIME — reported_cache_line_size() queries the HOST, and the
//      per-object address check converts the MEASURED cursor and payload
//      addresses into block indices under that reported size. THIS is
//      authoritative. Alignment alone never proves where two objects landed
//      relative to each other in the running machine's coherence granularity.
//
// A compile-time assumption larger or smaller than the host's real block does
// not, by itself, imply any particular outcome: what matters is the measured
// relationship of the actual addresses. The canonical host makes the concrete
// case rather than a hypothetical one — Apple M3 Max reports a cache-line size
// of 128 bytes (`sysctl hw.cachelinesize`), not the 64 most code assumes, and a
// layout built on a hard-coded 64 would have had to be checked against the real
// addresses before it could be trusted.
//
// line_size_supported() is therefore a cheap EARLY precondition, not the proof:
// it rejects a host whose real block is larger than the layout was built for,
// because separation cannot be guaranteed by construction there. The decisive
// check remains per-instance and address-based (CursorLayoutReport in
// include/spsc_cursor_layout_ring_buffer.h): the experiment requires the
// same_line / separated invariant to hold for the object that ACTUALLY RAN, and
// requires no cursor block to overlap payload storage. A cell whose runtime
// invariant is false is never published, whatever the compile-time assumption
// said.
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
// Over-aligning on a 64-byte host costs padding and changes no behaviour, so the
// larger value is the safer one to state at compile time.
//
// This is an ASSUMPTION, not a measurement, and it is not on its own evidence
// about any object's placement. It creates the intended candidate layout; every
// Phase-3A run then verifies the ACTUAL addresses of the cursors and payload it
// measured against reported_cache_line_size() before it may publish anything.
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

// EARLY precondition, not the layout proof: a run may only proceed if the host's
// reported interference block is no larger than the layout was built for. Beyond
// that size the compile-time alignment can no longer guarantee that the two
// cursor blocks land in distinct real blocks, so such a host must fail loudly
// rather than produce numbers that would be labelled "separated" without
// justification. Hosts at or below the assumption still have to pass the
// per-object runtime address check — that check is what is authoritative.
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
