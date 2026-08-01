#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd -P)"
source_root="${DARLING_SRC_ROOT:-$(cd "$script_dir/../../.." && pwd -P)}"
source_dir="$source_root/src/startup/tests"
if [ ! -f "$source_dir/CMakeLists.txt" ] ||
	[ ! -f "$source_root/src/startup/rootless_shutdown.c" ]; then
	printf 'ROOTLESS_SHUTDOWN_PERMANENT_COVERAGE_MISSING source=%s\n' \
		"$source_root" >&2
	exit 42
fi
if [ -n "${WEST_TEST_TMP:-}" ]; then
	build_dir="$WEST_TEST_TMP/rootless-shutdown-ctest"
	mkdir -p "$build_dir"
	cleanup=0
else
	build_dir="$(mktemp -d /tmp/darling-rootless-shutdown-ctest.XXXXXX)"
	cleanup=1
fi
cleanup_build() {
	if [ "$cleanup" -eq 1 ]; then
		rm -rf -- "$build_dir"
	fi
}
trap cleanup_build EXIT INT TERM

cmake -S "$source_dir" -B "$build_dir" -G Ninja
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure -L rootless-shutdown
