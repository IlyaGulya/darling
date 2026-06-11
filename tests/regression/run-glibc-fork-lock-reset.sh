#!/usr/bin/env bash
# Regression test for dar-gwn.5: build the production loader/stack-lock reset from
# src/startup/mldr/glibc_fork_reset.c together with a host harness, and assert that
# every inherited-held-lock case deadlocks without the reset and passes with it.
#
# This is a HOST test (plain glibc, no Darling runtime needed). It finishes in a
# couple of seconds. Exit 0 on PASS.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mldr_dir="$here/../../src/startup/mldr"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cc="${CC:-cc}"

# TLS-bearing object so dlopen() exercises _dl_load_tls_lock.
"$cc" -shared -fPIC -O0 -g "$here/glibc_fork_lock_reset_tls.c" -o "$work/tls_mod.so"

# Harness + the real production reset code, with the test-only accessors enabled.
"$cc" -O0 -g -DGLIBC_FORK_RESET_TEST_HOOKS \
	-I "$mldr_dir" \
	"$here/glibc_fork_lock_reset.c" "$mldr_dir/glibc_fork_reset.c" \
	-o "$work/glibc_fork_lock_reset" -ldl -lpthread

# The harness dlopen()s ./tls_mod.so, so run from the work dir.
cd "$work"
./glibc_fork_lock_reset
