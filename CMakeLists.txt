cmake_minimum_required(VERSION 3.16)

project(low_latency_trading_lab
    VERSION 0.1.0
    DESCRIPTION "Experiments in low-latency C++ trading infrastructure (Experiment 01: L2 order book, std::map vs flat)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# llob_core — the order-book implementations. Header-only; consumers get the
# library purely as include directories. All translation units using the hot
# path must compile with the same optimization flags.
# ---------------------------------------------------------------------------
add_library(llob_core INTERFACE)
target_include_directories(llob_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Tests (Experiment 01, correctness). Run with `ctest` or the binary directly.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(orderbook_tests tests/order_book_tests.cpp)
target_link_libraries(orderbook_tests PRIVATE llob_core)
# Assertions are plain CHECK macros, but make sure none are compiled out.
target_compile_options(orderbook_tests PRIVATE -Wall -Wextra)

add_test(NAME orderbook_tests COMMAND orderbook_tests)

# Phase 4: lightweight tail-latency statistics tests (percentiles, batch
# normalization, stream determinism, calibration path). Fast — never runs the
# canonical multi-million-update tail benchmark.
add_executable(phase4_stats_tests tests/phase4_stats_tests.cpp)
target_link_libraries(phase4_stats_tests PRIVATE llob_core)
target_compile_options(phase4_stats_tests PRIVATE -Wall -Wextra)
add_test(NAME phase4_stats_tests COMMAND phase4_stats_tests)

# Guard against a regression of the exit-code bug: the self-test mode runs only
# a deliberately failing CHECK and must return non-zero. If it ever returns 0
# (e.g. the runner stops propagating failures), CTest reports this test as
# FAILED and the suite signals the regression.
add_test(NAME orderbook_tests_exitcode
         COMMAND ${CMAKE_COMMAND}
                 -DLLDB_SELFTEST_FAIL=1
                 -DBINARY=$<TARGET_FILE:orderbook_tests>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/assert_nonzero_exit.cmake)
set_tests_properties(orderbook_tests_exitcode PROPERTIES
                     PASS_REGULAR_EXPRESSION "orderbook_tests_exitcode: got expected non-zero exit")

# ---------------------------------------------------------------------------
# Benchmark (Experiment 01, Phase 2). Deterministic steady-state apply()
# throughput. The benchmark is only meaningful as a release build: enable it
# explicitly (default ON), and it is forced to -O3 -DNDEBUG regardless of the
# CMAKE_BUILD_TYPE so a debug build cannot accidentally time unoptimized code.
# ---------------------------------------------------------------------------
option(BUILD_BENCHMARKS "Build the deterministic order-book benchmark" ON)
include(CheckCXXCompilerFlag)

if(BUILD_BENCHMARKS)
    add_executable(orderbook_bench benchmark/order_book_bench.cpp)
    target_link_libraries(orderbook_bench PRIVATE llob_core)
    # Override any build type: the benchmark hot path must always run optimized.
    target_compile_options(orderbook_bench PRIVATE -O3 -DNDEBUG)

    # Phase 4: tail-latency / jitter benchmark (fixed-batch sampling). Same
    # forced-optimized contract as the Phase 2 benchmark.
    add_executable(orderbook_tail_bench benchmark/order_book_tail_bench.cpp)
    target_link_libraries(orderbook_tail_bench PRIVATE llob_core)
    target_compile_options(orderbook_tail_bench PRIVATE -O3 -DNDEBUG)

    # CPU/arch tuning is OPT-IN and free-form so it is never hard-coded to one
    # machine. The canonical M3 measurements in the README were built WITHOUT
    # any arch flag (the compiler's default for the target). To tune for the
    # host, pass an explicit, compiler-validated flag:
    #   cmake -DBENCH_ARCH_FLAGS="-march=native"      # gcc/clang on Linux
    #   cmake -DBENCH_ARCH_FLAGS="-mcpu=apple-m1"     # Apple clang on M1-era
    #   cmake -DBENCH_ARCH_FLAGS="-mcpu=apple-m3"     # Apple clang on M3, if the
    #                                                 # installed clang accepts it
    # An unsupported flag is detected at configure time and dropped with a
    # warning rather than failing the build.
    set(BENCH_ARCH_FLAGS "" CACHE STRING
        "Extra arch flags for the benchmark target (e.g. -march=native). Empty = compiler default")
    if(NOT BENCH_ARCH_FLAGS STREQUAL "")
        check_cxx_compiler_flag("${BENCH_ARCH_FLAGS}" CXX_SUPPORTS_BENCH_ARCH)
        if(CXX_SUPPORTS_BENCH_ARCH)
            target_compile_options(orderbook_bench PRIVATE ${BENCH_ARCH_FLAGS})
            target_compile_options(orderbook_tail_bench PRIVATE ${BENCH_ARCH_FLAGS})
        else()
            message(WARNING "BENCH_ARCH_FLAGS '${BENCH_ARCH_FLAGS}' unsupported by this compiler; "
                            "building the benchmark with the compiler default")
        endif()
    endif()
endif()
