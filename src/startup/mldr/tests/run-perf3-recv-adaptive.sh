#!/usr/bin/env bash
# RED->GREEN runner for perf #3 (dar-dar6x4-perf-5dq.3): the mldr RPC recv hook's
# adaptive spin-then-block. Builds the host model test, asserts the BLOCKING arm FAILS
# (RED proof: a wakeup per RPC) and the ADAPTIVE arm PASSES (GREEN: fast reply caught
# without sleeping). Exit 0 only if RED failed AND GREEN passed.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/perf3_recv_adaptive_test.c"
BIN="$(mktemp -d)/perf3_recv_adaptive"

cc -O2 -o "$BIN" "$SRC" -lpthread || { echo "BUILD FAILED"; exit 2; }

echo "=== RED arm (blocking recv: must FAIL) ==="
if "$BIN" blocking; then
	echo "UNEXPECTED: blocking arm PASSED -- the test does not discriminate; aborting"
	exit 3
fi
echo "(RED failed as expected)"
echo

echo "=== GREEN arm (adaptive spin-then-block: must PASS) ==="
if ! "$BIN" adaptive; then
	echo "REGRESSION: adaptive arm FAILED"
	exit 1
fi

echo
echo "perf3 recv-adaptive RED->GREEN OK"
