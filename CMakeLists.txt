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

    # -march=native (or the closest Apple clang equivalent) materially changes
    # which instructions the timed loop compiles to, so it is opt-in and guarded
    # so a compiler without support simply omits it instead of failing.
    option(ENABLE_NATIVE_ARCH
        "Compile the benchmark with -march=native when the compiler supports it" OFF)
    if(ENABLE_NATIVE_ARCH)
        if(CMAKE_SYSTEM_NAME MATCHES "Darwin" AND CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
            check_cxx_compiler_flag("-mcpu=apple-m1" CXX_SUPPORTS_APPLE_M1)
            if(CXX_SUPPORTS_APPLE_M1)
                target_compile_options(orderbook_bench PRIVATE -mcpu=apple-m1)
            else()
                message(WARNING "ENABLE_NATIVE_ARCH: -mcpu=apple-m1 unsupported, building without it")
            endif()
        else()
            check_cxx_compiler_flag("-march=native" CXX_SUPPORTS_MARCH_NATIVE)
            if(CXX_SUPPORTS_MARCH_NATIVE)
                target_compile_options(orderbook_bench PRIVATE -march=native)
            else()
                message(WARNING "ENABLE_NATIVE_ARCH: -march=native unsupported, building without it")
            endif()
        endif()
    endif()
endif()
