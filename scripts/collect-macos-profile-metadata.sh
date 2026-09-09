#!/usr/bin/env bash
# Collect macOS / Apple Silicon benchmark metadata for a measured run on this
# machine. It is shared by the Phase 3M Apple Instruments recordings (which
# observe the SAME macOS machine that produced the Phase 2 canonical numbers)
# and the Phase 4 tail-bench cells (scripts/tail-bench.sh), so the heading is
# intentionally generic — "macOS / Apple Silicon benchmark metadata" — rather
# than naming one phase. In each case the metadata anchors the measured
# artifact to this machine/build. Read-only: it never changes system settings.
#
# Usage:
#   scripts/collect-macos-profile-metadata.sh [outfile]
#     outfile   where to write the metadata block (default: stdout)
#
# Records (where available): macOS version, uname, machine model, Apple chip,
# memory size, Xcode version, Instruments availability, Apple clang version,
# the benchmark's compiler flags, date, and the repository commit hash.
# Missing tools are reported as "absent" rather than guessed.

set -u

OUT="${1:-/dev/stdout}"

# ---- helpers: record a key or report it absent --------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

# Section header to the output stream.
section() { printf '\n## %s\n' "$1" >>"$OUT"; }
kv()       { printf '%-24s %s\n' "$1:" "$2" >>"$OUT"; }

umask 022   # results are shared/committed; keep the metadata world-readable

printf '# macOS / Apple Silicon benchmark metadata — %s\n' \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$OUT"

# ---- OS / machine -------------------------------------------------------------
section 'System'
kv "macOS"      "$(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null) (Build $(sw_vers -buildVersion 2>/dev/null))"
kv "uname"      "$(uname -a 2>/dev/null)"
kv "model"      "$(sysctl -n hw.model 2>/dev/null || echo absent)"
kv "chip"       "$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sysctl -n hw.optional.arm64 2>/dev/null | sed 's/^1$/Apple Silicon (arm64)/' || echo absent)"
kv "cores"      "$(sysctl -n hw.ncpu 2>/dev/null || echo absent) logical"
kv "efficiency" "$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || echo 0) P-cores / $(sysctl -n hw.perflevel1.logicalcpu 2>/dev/null || echo 0) E-cores (hw.perflevel*, may report 0)"
kv "memory"     "$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1024 / 1024 / 1024 )) GiB"

# ---- Scheduling caveat (honest) -----------------------------------------------
section 'Scheduling caveat (experimental limitation)'
kv "note" "Apple Silicon mixes performance (P) and efficiency (E) cores; macOS schedules freely."
kv "note" "No strict CPU-core pinning is claimed unless a recording pinned and verified affinity."
kv "note" "P/E mix and OS scheduling can move a process between cores across reps."

# ---- Apple toolchain ----------------------------------------------------------
section 'Apple toolchain'
if [ -n "${DEVELOPER_DIR:-}" ]; then
    kv "DEVELOPER_DIR" "$DEVELOPER_DIR"
fi
if have xcode-select; then
    kv "xcode-select" "$(xcode-select -p 2>/dev/null)"
fi
if have xcodebuild; then
    xb="$(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
    kv "xcodebuild" "${xb:-absent — Command Line Tools only (no full Xcode)}"
else
    kv "xcodebuild" "absent"
fi

# Instruments / xctrace — what a Phase 3M recording actually needs.
if have xcrun && xcrun --find xctrace >/dev/null 2>&1; then
    kv "xctrace" "$(xcrun xctrace version 2>&1 | head -1)"
else
    kv "xctrace" "absent — no full Xcode / Instruments on this host"
fi
if [ -d "/Applications/Xcode.app/Contents/Applications/Instruments.app" ]; then
    kv "Instruments" "/Applications/Xcode.app/Contents/Applications/Instruments.app"
else
    kv "Instruments" "absent — no /Applications/Xcode.app"
fi

if have clang; then
    kv "clang" "$(clang --version 2>/dev/null | head -1)"
else
    kv "clang" "absent"
fi

# ---- Benchmark build provenance -------------------------------------------------
section 'Benchmark build (Phase 2 canonical flags)'
kv "flags" "-O3 -DNDEBUG (forced on the benchmark target); BENCH_ARCH_FLAGS empty unless stated"
kv "standard" "C++20"

# The exact git commit that produced this metadata (for provenance of a
# recording made from a dirty tree, say so).
if git rev-parse --git-dir >/dev/null 2>&1; then
    kv "commit" "$(git rev-parse HEAD 2>/dev/null)"
    if ! git diff --quiet 2>/dev/null; then
        kv "tree" "DIRTY (uncommitted changes present at recording time)"
    else
        kv "tree" "clean"
    fi
else
    kv "commit" "not a git checkout"
fi

printf '\n# end Phase 3M metadata\n' >>"$OUT"

# If a file was requested, echo a pointer (metadata went to the file).
if [ "$OUT" != "/dev/stdout" ]; then
    printf 'metadata written to %s\n' "$OUT"
fi
