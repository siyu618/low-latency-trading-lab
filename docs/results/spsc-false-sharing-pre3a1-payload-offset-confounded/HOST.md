# Experiment 02 Phase 3A — host and toolchain metadata

Measured with `spsc_false_sharing_bench` built from repo HEAD
`5f485fe6b5a4dceb41dac22c88a12a7d0aac1f0e`.

**Cache-line size.** The host reports its cache-line size at runtime;
this run recorded **128 bytes**. The compile-time layout
assumption is 128 bytes. If the reported size had exceeded that
assumption the benchmark would have exited non-zero before timing
anything, because the separated control could then share a real line.

**Scheduling limitation:** this is Apple Silicon/macOS. No hard CPU
pinning or affinity is implemented or claimed; scheduler placement,
P-core/E-core placement, migration, frequency and system load can all
influence these concurrent measurements. See docs/SPSC_FALSE_SHARING.md.

# macOS / Apple Silicon benchmark metadata — 2026-09-12T02:13:36Z

## System
macOS:                   macOS 14.2.1 (Build 23C71)
uname:                   Darwin 192.168.1.226 23.2.0 Darwin Kernel Version 23.2.0: Wed Nov 15 21:54:05 PST 2023; root:xnu-10002.61.3~2/RELEASE_ARM64_T6031 arm64
model:                   Mac15,10
chip:                    Apple M3 Max
cores:                   14 logical
efficiency:              10 P-cores / 4 E-cores (hw.perflevel*, may report 0)
memory:                  36 GiB

## Scheduling caveat (experimental limitation)
note:                    Apple Silicon mixes performance (P) and efficiency (E) cores; macOS schedules freely.
note:                    No strict CPU-core pinning is claimed unless a recording pinned and verified affinity.
note:                    P/E mix and OS scheduling can move a process between cores across reps.

## Apple toolchain
xcode-select:            /Applications/Xcode.app/Contents/Developer
xcodebuild:              Xcode 15.4 Build version 15F31d 
xctrace:                 xctrace version 15.3 (15F31d)
Instruments:             /Applications/Xcode.app/Contents/Applications/Instruments.app
clang:                   Apple clang version 15.0.0 (clang-1500.3.9.4)

## Benchmark build (Phase 2 canonical flags)
flags:                   -O3 -DNDEBUG (forced on the benchmark target); BENCH_ARCH_FLAGS empty unless stated
standard:                C++20
commit:                  5f485fe6b5a4dceb41dac22c88a12a7d0aac1f0e
tree:                    DIRTY (uncommitted changes present at recording time)

# end Phase 3M metadata
