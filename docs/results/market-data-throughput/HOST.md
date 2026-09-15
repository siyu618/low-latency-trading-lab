# Experiment 03 Phase 3A — host and toolchain metadata

Measured with `market_data_pipeline_throughput_bench` built from repo HEAD
`2b24040643445e7ff30349af3dbc7d4b169b7460`.

| property | value |
|---|---|
| hostname | `bogon` |
| uname | `Darwin bogon 23.2.0 Darwin Kernel Version 23.2.0: Wed Nov 15 21:54:05 PST 2023; root:xnu-10002.61.3~2/RELEASE_ARM64_T6031 arm64` |
| macOS | `14.2.1` |
| CPU | `Apple M3 Max` |
| logical CPUs | 14 |
| performance cores | 10 |
| efficiency cores | 4 |
| reported cache line | 128 bytes |
| compiler | `Apple clang version 15.0.0 (clang-1500.3.9.4) Target: arm64-apple-darwin23.2.0 ` |
| build type | Release, forced `-O3 -DNDEBUG` |
| extra arch flags | `` (empty = compiler default) |
| clock | `std::chrono::steady_clock` only, read twice per repetition |
| load average (1 min) at start | `3.08` |
| load average (1 min) at end | `3.16` |
| pre-flight load limit | `14` (0 = check disabled) |
| uptime at start | `3.08 3.80 3.70` |

Thread placement is NOT controlled: no affinity, no pinning and no thread
priority is set anywhere in this experiment. macOS may migrate either
thread mid-run, and the two threads may land on different core types. No
measurement in this dataset observes which cores were used, so no result
here may be attributed to a core type.

The reported figure is therefore **host-specific**, and the host was NOT
quiesced beyond the load-average pre-flight. It must not be quoted as a
property of the pipeline alone.

## Exact sources that produced this dataset

A commit hash alone does not describe a working tree with uncommitted
changes, so the digest of each source file is recorded here. The digest is
of the file ON DISK at run time, which is what the binary was built from.

```
git HEAD: 2b24040643445e7ff30349af3dbc7d4b169b7460

working tree at run time (git status --short):
 M CMakeLists.txt
?? benchmark/market_data_pipeline_throughput_bench.cpp
?? docs/results/market-data-throughput/
?? scripts/analyze-market-data-throughput.py
?? scripts/market-data-throughput.sh

sha256  a2bfc501a1851db8645122828abf1e2c66192e4685bd5d21722979d90ec8455d  benchmark/market_data_pipeline_throughput_bench.cpp
sha256  d90666dad0bf786d4e245d0ef595ebe7a6c4ba6f6618d79026b558b536a3bec9  scripts/market-data-throughput.sh
sha256  4bc9a2a5ea0088f2aa3bbd3f365f181bc57648d550feef4cb8ae0a53fea8ad02  scripts/analyze-market-data-throughput.py
sha256  fd40fa1bf9d11986fd77b88fd593fc9d1434388ef028299b21e25a3bad9efdf4  market-data-pipeline/include/md_stream_decoder.h
sha256  3c21eaccac7d4dd897927b6b3345183056988caaaa2c4ceed87fa22e95db4e77  market-data-pipeline/include/md_threaded_pipeline.h
sha256  35cde5c70447762ad9ceb6dda5c56633bca753db4b00100db1eac1e9578a6253  market-data-pipeline/include/market_data_pipeline.h
sha256  7b0745e1223495950a9d925a5eb801537b3d1461de6b0c6476d7333d8bfa2486  market-data-pipeline/include/md_decoder.h
sha256  6a048e2f609ef746237e745004b0b986fed086748bc73808af31b8a0e723c257  market-data-pipeline/include/md_encoder.h
sha256  1e094dbd518f0053585b482a53b7c996540702a162ee019245770ecaa54bb11b  market-data-pipeline/include/md_stream_gen.h
sha256  ed4855223270e483128a4b8833df7d815b0b2b4bca9ffba1387b433918e8e96a  include/spsc_remote_cursor_ring_buffer.h
sha256  d8faa6af586b0955a66c7fb4fb3eb6a7c451d61a99c1dea8e5400d9f6b2aa473  include/flat_order_book.h
sha256  5c650afd5ea2075982613f2c42f9dbd4b0556fdc90d1a9a94bc2bc4168cdf8e8  include/types.h
```
