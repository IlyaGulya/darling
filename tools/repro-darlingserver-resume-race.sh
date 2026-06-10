#!/usr/bin/env bash
set -euo pipefail

# Runs one or more intentionally amplified darlingserver builds. See
# repro-darlingserver-resume-race/README.md for source-level build instructions.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="${ROOT:-$(cd "$script_dir/.." && pwd)}"
runner="${RUNNER:-$root/tools/darling-debug-runner/target/release/darling-debug-runner}"
darling="${DARLING:-$root/../darling-prefix/bin/darling}"
dprefix="${DPREFIX:-$root/../darling-prefix-homebrew-test}"
iterations="${ITERATIONS:-3}"

ruby_command='/usr/local/Homebrew/Library/Homebrew/vendor/portable-ruby/current/bin/ruby -e '\''far=Thread.new{sleep 3600};ts=5.times.map{Thread.new{10.times{sleep(rand*0.004+0.0001)}}};ts.each(&:join);far.kill;puts "done"'\'''

usage() {
	cat <<EOF
Usage: $0 LABEL=SERVER [LABEL=SERVER ...]

Environment:
  ROOT        Darling source root (default: \$HOME/work/darling)
  RUNNER      darling-debug-runner executable
  DARLING     darling executable
  DPREFIX     disposable Darling prefix containing Homebrew portable Ruby
  ITERATIONS  runs per server (default: 3)

Example:
  $0 baseline=/tmp/darlingserver-baseline fixed=/tmp/darlingserver-fixed
EOF
}

if (($# == 0)); then
	usage >&2
	exit 2
fi

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
	local timeout=0
	local failed=0

	for i in $(seq 1 "$iterations"); do
		local output
		output="$(mktemp)"
		if "$runner" darling \
			--name "resume-race-$name-$i" \
			--timeout-seconds 15 \
			--darling "$darling" \
			--dprefix "$dprefix" \
			--install-server "$server" \
			-- /bin/bash -lc "$ruby_command" >"$output" 2>&1; then
			((pass += 1))
		elif grep -q 'RESULT=timeout' "$output"; then
			((timeout += 1))
		else
			((failed += 1))
		fi
		rm -f "$output"
	done

	printf '%s pass=%d timeout=%d failed=%d\n' "$name" "$pass" "$timeout" "$failed"
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
