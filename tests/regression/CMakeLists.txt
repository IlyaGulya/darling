# Source-owned host regression targets.  This file also configures standalone
# so the narrow suite can be developed without configuring all of Darling.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.13)
	project(darling-host-regressions C)
	include(CTest)
endif()

find_package(Threads REQUIRED)

add_executable(darling_host_thread_create_checkin_wait
	thread_create_checkin_wait.c)
target_link_libraries(darling_host_thread_create_checkin_wait PRIVATE Threads::Threads)

add_test(
	NAME host/mldr_thread_create_checkin_wait
	COMMAND darling_host_thread_create_checkin_wait)
set_tests_properties(host/mldr_thread_create_checkin_wait PROPERTIES
	LABELS "env:host;diag:bare;bead:dar-dar6x4-perf-5dq.1;submod:darling"
	TIMEOUT 5)

# The same behavioral harness carries an explicit old-algorithm arm.  The
# runner requires both the expected non-zero result and the busy-spin symptom.
add_test(
	NAME host/mldr_thread_create_checkin_wait_spin_red
	COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/expect_failure_marker.sh"
		"FAIL: creator burned"
		$<TARGET_FILE:darling_host_thread_create_checkin_wait> spin)
set_tests_properties(host/mldr_thread_create_checkin_wait_spin_red PROPERTIES
	LABELS "env:host;diag:bare;bead:dar-dar6x4-perf-5dq.1;submod:darling;red:true"
	TIMEOUT 5)

add_library(darling_host_glibc_fork_tls SHARED glibc_fork_lock_reset_tls.c)
set_target_properties(darling_host_glibc_fork_tls PROPERTIES
	PREFIX ""
	OUTPUT_NAME "tls_mod"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_executable(darling_host_glibc_fork_lock_reset
	glibc_fork_lock_reset.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/startup/mldr/glibc_fork_reset.c)
target_compile_definitions(darling_host_glibc_fork_lock_reset
	PRIVATE GLIBC_FORK_RESET_TEST_HOOKS)
target_include_directories(darling_host_glibc_fork_lock_reset PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/startup/mldr)
target_link_libraries(darling_host_glibc_fork_lock_reset PRIVATE
	${CMAKE_DL_LIBS}
	Threads::Threads)
add_dependencies(darling_host_glibc_fork_lock_reset darling_host_glibc_fork_tls)

add_test(
	NAME host/glibc_fork_lock_reset
	COMMAND darling_host_glibc_fork_lock_reset)
set_tests_properties(host/glibc_fork_lock_reset PROPERTIES
	LABELS "env:host;diag:bare;bead:dar-gwn.5;submod:darling"
	TIMEOUT 20
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
