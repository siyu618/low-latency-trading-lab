# Experiment 02 Phase 3B — host and toolchain metadata

Measured with `spsc_remote_cursor_bench` built from repo HEAD
`6ff0b57e40a28b766a338342b7e950bcb643aa38`.

**Cache-line size.** The host reports its cache-line size at runtime; the
benchmark queries it before timing anything and refuses to run if the report
exceeds the compile-time layout assumption. On the canonical M3 Max host
this is **128 bytes**, not the 64 most code assumes.

| property | value |
|---|---|
| hostname | `192.168.1.226` |
| uname | `Darwin 192.168.1.226 23.2.0 Darwin Kernel Version 23.2.0: Wed Nov 15 21:54:05 PST 2023; root:xnu-10002.61.3~2/RELEASE_ARM64_T6031 arm64` |
| macOS | `14.2.1` |
| CPU | `Apple M3 Max` |
| logical CPUs | 14 |
| performance cores | 10 |
| efficiency cores | 4 |
| reported cache line | 128 bytes |
| compiler | `Apple clang version 15.0.0 (clang-1500.3.9.4) Target: arm64-apple-darwin23.2.0 ` |
| build type | Release, forced `-O3 -DNDEBUG` |
| extra arch flags | `` (empty = compiler default) |
| load average at start | `7.07 5.85 5.08` |

Thread placement is NOT controlled: no affinity, no pinning and no thread
priority is set anywhere in this experiment, and macOS may migrate either
thread mid-run or place the two processes on different core types.

## Reported by the measured processes themselves

```
cache_line_size_reported=128
cursor_policy_size=256
cursor_layout=separated
```
