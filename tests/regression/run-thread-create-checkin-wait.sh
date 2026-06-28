#!/usr/bin/env bash
# Regression test for perf #1 (dar-dar6x4-perf-5dq.1): __darling_thread_create() must
# FUTEX_WAIT for the new thread's checkin, not busy-spin sched_yield().
#
# Builds the handshake harness and asserts:
#   - GREEN: the futex path keeps the creator's CPU ~0 while it waits  (exit 0)
#   - RED:   the old spin path burns a core (exit 1) -- proves the test discriminates
#
# HOST test (plain glibc, no Darling runtime). Finishes in ~1s. Exit 0 on PASS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"

"$cc" -O2 -g "$here/thread_create_checkin_wait.c" -o "$work/tccw" -lpthread

# RED proof first: the discriminating spin path MUST fail (non-zero), else the test is blind.
echo "--- RED proof (old sched_yield spin path, expected to FAIL) ---"
if "$work/tccw" spin; then
	echo "FAIL: spin path unexpectedly passed -- test does not discriminate busy-spin" >&2
	exit 2
fi
echo "(RED path failed as expected)"

# GREEN: the production futex path must pass.
echo "--- GREEN (futex wait path, expected to PASS) ---"
"$work/tccw"
