cmake_minimum_required(VERSION 3.13)
project(darling-rootless-shutdown-contracts C)

enable_testing()

foreach(shutdown_test IN ITEMS session lifecycle shared_session)
	set(shutdown_target "darling_rootless_shutdown_${shutdown_test}_test")
	add_executable(${shutdown_target}
		rootless_shutdown_${shutdown_test}_test.c
		../rootless_shutdown.c
		../runtime_credentials.c
		../runtime_mode.c
		../runtime_mode_prefix.c)
	target_compile_definitions(${shutdown_target} PRIVATE _GNU_SOURCE)
	target_compile_options(${shutdown_target} PRIVATE
		-Wall -Wextra -Werror -Wno-c23-extensions)
	target_include_directories(${shutdown_target} PRIVATE ..)
	add_test(NAME ${shutdown_target} COMMAND ${shutdown_target})
	set_tests_properties(${shutdown_target} PROPERTIES
		LABELS "env:host;rootless-shutdown"
		TIMEOUT 20)
endforeach()

add_executable(darling_rootless_shellspawn_shutdown_test
	../../shellspawn/shellspawn.c
	../../shellspawn/duct_signals.c
	../runtime_mode.c)
target_compile_definitions(darling_rootless_shellspawn_shutdown_test PRIVATE
	_GNU_SOURCE
	TESTING
	DARLING_RUNTIME_EUNION_CAPABLE=1)
target_compile_options(darling_rootless_shellspawn_shutdown_test PRIVATE
	-Wall -Wextra -Werror -Wno-c23-extensions)
target_include_directories(darling_rootless_shellspawn_shutdown_test PRIVATE
	..
	../../shellspawn)
add_test(NAME darling_rootless_shellspawn_shutdown_test
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/rootless_shellspawn_shutdown_test.sh
		$<TARGET_FILE:darling_rootless_shellspawn_shutdown_test>)
set_tests_properties(darling_rootless_shellspawn_shutdown_test PROPERTIES
	LABELS "env:host;rootless-shutdown"
	TIMEOUT 20)

add_test(NAME darling_rootless_launchd_endpoint_contract
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/rootless_launchd_endpoint_contract.sh
		${CMAKE_CURRENT_SOURCE_DIR}/../../launchd/src/ipc.c)
set_tests_properties(darling_rootless_launchd_endpoint_contract PROPERTIES
	LABELS "env:host;rootless-shutdown"
	TIMEOUT 20)
