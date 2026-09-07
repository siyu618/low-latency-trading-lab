# CMake script used by the `orderbook_tests_exitcode` CTest test.
#
# Invoked by ctest (via cmake -P) as:
#   cmake -DLLDB_SELFTEST_FAIL=1 -DBINARY=<path> -P assert_nonzero_exit.cmake
#
# Purpose: prove that the test binary returns NON-ZERO when a CHECK fails.
# Regression guard for the order-book test runner's exit-code handling.

if(NOT DEFINED BINARY)
    message(FATAL_ERROR "assert_nonzero_exit.cmake: BINARY not provided")
endif()
if(NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "assert_nonzero_exit.cmake: binary not found: ${BINARY}")
endif()

# The test binary is driven to fail via its self-test env-var mode.
set(_env "LLDB_SELFTEST_FAIL=1")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "${_env}" "${BINARY}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err
)

# Expected: the self-test CHECK fails => the binary must exit non-zero.
if(_rc EQUAL 0)
    message(FATAL_ERROR
        "exit-code self-test FAILED: binary returned 0 despite a failing CHECK. "
        "Output was:\n${_out}\n${_err}")
endif()

message(STATUS "orderbook_tests_exitcode: got expected non-zero exit (code=${_rc})")
