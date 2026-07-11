#!/usr/bin/env bash
# Require a negative regression arm to fail for its documented reason.
set -euo pipefail

marker="$1"
shift
output="$(mktemp "${TMPDIR:-/tmp}/darling-regression-red.XXXXXX")"
trap 'rm -f "$output"' EXIT

set +e
"$@" >"$output" 2>&1
rc=$?
set -e
cat "$output"

if [ "$rc" -eq 0 ]; then
	echo "negative arm passed unexpectedly" >&2
	exit 1
fi
if ! grep -F -q -- "$marker" "$output"; then
	echo "negative arm did not report expected marker: $marker" >&2
	exit 1
fi
