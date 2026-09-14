# ---------------------------------------------------------------------------
# Experiment 03 — Market Data Pipeline.
#
#   Phase 1A  sequencer / snapshot / recovery correctness   COMPLETE / FROZEN
#   Phase 1B  binary protocol / decoder correctness         COMPLETE / FROZEN
#
# Header-only, like every other library in this repository: the INTERFACE target
# carries the include directory and the dependency on llob_core (for types.h,
# the books, and validate_snapshot), and nothing is compiled into a translation
# unit of its own. Phase 1B adds three more headers to the same target — the
# wire protocol, the decoder and the encoder — so there is still exactly one
# Experiment-03 library and no new link dependency.
#
# There is no benchmark target in Phase 1. No clock is read anywhere in the
# pipeline or the decoder, so there is nothing to time yet, and adding a target
# that measured nothing would be worse than adding none. When a measurement
# phase arrives it will need an entry in the root BENCH_ARCH_FLAGS list; that
# list is hand-extended per target and this directory deliberately does not
# appear in it.
# ---------------------------------------------------------------------------

# The strict warning set, held to Experiment 03 ONLY.
#
# Deliberately NOT global and NOT applied to the Experiment 01/02 targets.
# Those trees are frozen with published measurements, so tightening their
# warnings would either churn frozen files or invite a "fix" to code whose
# results are already in the record. Experiment 03 is the directory being held
# to this contract, and it is held to it on every target it owns — including
# the Phase-1A suite, which compiles clean under these flags unchanged.
set(LLMD_STRICT_FLAGS
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

add_library(llob_mdp INTERFACE)
target_include_directories(llob_mdp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(llob_mdp INTERFACE llob_core)

# ---------------------------------------------------------------------------
# Phase 1A — the sequencer suite.
#
# `md_oracle.h` and `md_stream_gen.h` are reached by quoted include relative to
# the including file, so no extra include directory is needed.
# ---------------------------------------------------------------------------
add_executable(market_data_pipeline_tests tests/market_data_pipeline_tests.cpp)
target_link_libraries(market_data_pipeline_tests PRIVATE llob_mdp)
# Assertions are plain CHECK macros, but make sure none are compiled out.
target_compile_options(market_data_pipeline_tests PRIVATE ${LLMD_STRICT_FLAGS})

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

# ---------------------------------------------------------------------------
# Phase 1B — the wire protocol decoder suite.
#
# Same shape as the Phase-1A target: an executable, a plain CTest entry, and a
# paired exit-code guard.
# ---------------------------------------------------------------------------
add_executable(md_decoder_tests tests/md_decoder_tests.cpp)
target_link_libraries(md_decoder_tests PRIVATE llob_mdp)
target_compile_options(md_decoder_tests PRIVATE ${LLMD_STRICT_FLAGS})

add_test(NAME md_decoder_tests COMMAND md_decoder_tests)

add_test(NAME md_decoder_tests_exitcode
         COMMAND ${CMAKE_COMMAND}
                 -DLLDB_SELFTEST_FAIL=1
                 -DNAME=md_decoder_tests_exitcode
                 -DBINARY=$<TARGET_FILE:md_decoder_tests>
                 -P ${CMAKE_SOURCE_DIR}/cmake/assert_nonzero_exit.cmake)
set_tests_properties(md_decoder_tests_exitcode PROPERTIES
                     PASS_REGULAR_EXPRESSION "md_decoder_tests_exitcode: got expected non-zero exit")

# ---------------------------------------------------------------------------
# Phase 2 — the threaded decoder -> SPSC -> book pipeline.
#
# The only target in this directory that creates threads, so it is the only one
# that links Threads::Threads — explicitly, rather than relying on the C++
# runtime happening to provide it. On a platform where std::thread needs a
# separate library, an implicit dependency would be a link error in this
# directory and nowhere else, which is exactly the kind of thing that should be
# declared rather than discovered.
#
# Same shape as the two suites above: strict flags, a plain CTest entry, and a
# paired exit-code guard.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_executable(md_threaded_pipeline_tests tests/md_threaded_pipeline_tests.cpp)
target_link_libraries(md_threaded_pipeline_tests PRIVATE llob_mdp Threads::Threads)
target_compile_options(md_threaded_pipeline_tests PRIVATE ${LLMD_STRICT_FLAGS})

add_test(NAME md_threaded_pipeline_tests COMMAND md_threaded_pipeline_tests)

add_test(NAME md_threaded_pipeline_tests_exitcode
         COMMAND ${CMAKE_COMMAND}
                 -DLLDB_SELFTEST_FAIL=1
                 -DNAME=md_threaded_pipeline_tests_exitcode
                 -DBINARY=$<TARGET_FILE:md_threaded_pipeline_tests>
                 -P ${CMAKE_SOURCE_DIR}/cmake/assert_nonzero_exit.cmake)
set_tests_properties(md_threaded_pipeline_tests_exitcode PROPERTIES
                     PASS_REGULAR_EXPRESSION "md_threaded_pipeline_tests_exitcode: got expected non-zero exit")

# A threaded suite is the one place a hang is a realistic failure mode: a
# consumer that never observes completion, or a producer spinning on a ring
# nobody drains, blocks forever rather than failing. Give it a hard timeout so
# that failure mode reports as a test failure instead of stalling CI.
set_tests_properties(md_threaded_pipeline_tests PROPERTIES TIMEOUT 120)
