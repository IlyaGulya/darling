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
