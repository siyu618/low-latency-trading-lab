# ---------------------------------------------------------------------------
# Experiment 03 — Market Data Pipeline (Phase 1: sequencer / correctness).
#
# Header-only, like every other library in this repository: the INTERFACE
# target carries the include directory and the dependency on llob_core (for
# types.h, the books, and validate_snapshot), and nothing is compiled into a
# translation unit of its own.
#
# There is no benchmark target in this phase. Phase 1 measures no time — no
# clock is read anywhere in the pipeline — so there is nothing to time yet, and
# adding a target that measured nothing would be worse than adding none. When a
# measurement phase arrives it will need an entry in the root BENCH_ARCH_FLAGS
# list; that list is hand-extended per target and this directory deliberately
# does not appear in it.
# ---------------------------------------------------------------------------

add_library(llob_mdp INTERFACE)
target_include_directories(llob_mdp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(llob_mdp INTERFACE llob_core)

# Test target. `md_oracle.h` and `md_stream_gen.h` are reached by quoted include
# relative to the including file, so no extra include directory is needed.
add_executable(market_data_pipeline_tests tests/market_data_pipeline_tests.cpp)
target_link_libraries(market_data_pipeline_tests PRIVATE llob_mdp)
# Assertions are plain CHECK macros, but make sure none are compiled out.
target_compile_options(market_data_pipeline_tests PRIVATE -Wall -Wextra)

add_test(NAME market_data_pipeline_tests COMMAND market_data_pipeline_tests)

# Guard against a regression of the exit-code bug: the self-test mode runs only
# a deliberately failing CHECK and must return non-zero. If it ever returns 0
# (e.g. the runner stops propagating failures), CTest reports this test as
# FAILED and the suite signals the regression.
add_test(NAME market_data_pipeline_tests_exitcode
         COMMAND ${CMAKE_COMMAND}
                 -DLLDB_SELFTEST_FAIL=1
                 -DNAME=market_data_pipeline_tests_exitcode
                 -DBINARY=$<TARGET_FILE:market_data_pipeline_tests>
                 -P ${CMAKE_SOURCE_DIR}/cmake/assert_nonzero_exit.cmake)
set_tests_properties(market_data_pipeline_tests_exitcode PROPERTIES
                     PASS_REGULAR_EXPRESSION "market_data_pipeline_tests_exitcode: got expected non-zero exit")
