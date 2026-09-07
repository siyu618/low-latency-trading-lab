cmake_minimum_required(VERSION 3.16)

project(low_latency_orderbook
    VERSION 0.1.0
    DESCRIPTION "Two L2 order book designs (map vs flat) benchmarked for latency"
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
# Tests (Phase 1). Run with `ctest` or execute the binary directly.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(orderbook_tests tests/order_book_tests.cpp)
target_link_libraries(orderbook_tests PRIVATE llob_core)
# Assertions are plain CHECK macros, but make sure none are compiled out.
target_compile_options(orderbook_tests PRIVATE -Wall -Wextra)

add_test(NAME orderbook_tests COMMAND orderbook_tests)

# ---------------------------------------------------------------------------
# Benchmark (Phase 2+). Disabled until the benchmark source lands; kept as a
# clear next step.
# ---------------------------------------------------------------------------
# add_executable(orderbook_bench benchmark/order_book_bench.cpp)
# target_link_libraries(orderbook_bench PRIVATE llob_core)
# target_compile_options(orderbook_bench PRIVATE -O3 -DNDEBUG)
