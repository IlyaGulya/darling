#!/usr/bin/env bash
set -euo pipefail

# Runs one unchanged Mach-O client against unmodified darlingserver binaries.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${ROOT:-$(cd "$script_dir/.." && pwd)}"
runner="${RUNNER:-$root/tools/darling-debug-runner/target/release/darling-debug-runner}"
darling="${DARLING:-$root/../darling-prefix/bin/darling}"
dprefix="${DPREFIX:-$root/../darling-prefix-homebrew-test}"
iterations="${ITERATIONS:-3}"
timeout="${TIMEOUT:-20}"
threads="${THREADS:-32}"
waits="${WAITS:-1000}"
delay_ns="${DELAY_NS:-500000}"

usage() {
	cat <<EOF
Usage: $0 CLIENT LABEL=SERVER [LABEL=SERVER ...]

CLIENT must be a Mach-O executable at a path visible inside Darling.

Environment:
  ROOT        Darling source root (default: \$HOME/work/darling)
  RUNNER      darling-debug-runner executable
  DARLING     darling executable
  DPREFIX     disposable Darling prefix
  ITERATIONS  runs per server (default: 3)
  TIMEOUT     seconds allowed per run (default: 20)
  THREADS     concurrent client threads (default: 32)
  WAITS       timed waits per thread (default: 1000)
  DELAY_NS    timeout for each wait (default: 500000)

Example:
  $0 /Volumes/SystemRoot/tmp/timer-wait-stress \
    old=/tmp/darlingserver-old new=/tmp/darlingserver-new
EOF
}

if (($# < 2)); then
	usage >&2
	exit 2
fi

client="$1"
shift

for required in "$runner" "$darling"; do
	if [[ ! -x "$required" ]]; then
		printf 'Missing executable: %s\n' "$required" >&2
		exit 2
	fi
done

run_variant() {
	local name="$1"
	local server="$2"
	local pass=0
	local timeout_count=0
	local failed=0

	for i in $(seq 1 "$iterations"); do
		local output
		output="$(mktemp)"
		if "$runner" darling \
			--name "timer-wait-$name-$i" \
			--timeout-seconds "$timeout" \
			--darling "$darling" \
			--dprefix "$dprefix" \
			--install-server "$server" \
			-- "$client" "$threads" "$waits" "$delay_ns" >"$output" 2>&1; then
			((pass += 1))
		elif grep -q 'RESULT=timeout' "$output"; then
			((timeout_count += 1))
		else
			((failed += 1))
			cat "$output" >&2
		fi
		rm -f "$output"
	done

	printf '%s pass=%d timeout=%d failed=%d\n' "$name" "$pass" "$timeout_count" "$failed"
}

for variant in "$@"; do
	if [[ "$variant" != *=* ]]; then
		printf 'Expected LABEL=SERVER, got: %s\n' "$variant" >&2
		exit 2
	fi
	name="${variant%%=*}"
	server="${variant#*=}"
	if [[ ! -x "$server" ]]; then
		printf 'Missing executable for %s: %s\n' "$name" "$server" >&2
		exit 2
	fi
	run_variant "$name" "$server"
done
