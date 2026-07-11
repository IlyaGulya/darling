#!/usr/bin/env bash
# Configure, build, and run the standalone source-owned host CTest suite.
set -euo pipefail

if [ "$#" -gt 1 ]; then
	echo "usage: $0 [DARLING_SOURCE_ROOT]" >&2
	exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_root="${1:-${DARLING_SRC_ROOT:-$(cd "$script_dir/../.." && pwd)}}"
suite_root="$source_root/tests/regression"
if [ ! -f "$suite_root/CMakeLists.txt" ]; then
	echo "host regression CMake suite is unavailable at $suite_root" >&2
	exit 1
fi

build_root="${DARLING_HOST_REGRESSION_BUILD_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/darling-host-regressions.XXXXXX")}"
cleanup_build=0
if [ -z "${DARLING_HOST_REGRESSION_BUILD_DIR:-}" ]; then
	cleanup_build=1
fi
cleanup() {
	if [ "$cleanup_build" -eq 1 ]; then
		rm -rf "$build_root"
	fi
}
trap cleanup EXIT

cmake -S "$suite_root" -B "$build_root" -G Ninja
cmake --build "$build_root" --target \
	darling_host_thread_create_checkin_wait \
	darling_host_glibc_fork_lock_reset \
	darling_host_shellspawn_exit_status
ctest --test-dir "$build_root" --output-on-failure -L '^env:host$'
