#!/usr/bin/env bash
set -euo pipefail

# Compares two intentionally amplified darlingserver builds. Both binaries
# contain a 20ms delay between observing TH_WAIT and entering Thread::suspend();
# the baseline loses that wake, while the resume-permit build retains it.
#
# Override paths with RUNNER, DARLING, DPREFIX, BASELINE_SERVER, FIXED_SERVER,
# and the run count with ITERATIONS.

root="${ROOT:-$HOME/work/darling}"
runner="${RUNNER:-$root/tools/darling-debug-runner/target/release/darling-debug-runner}"
darling="${DARLING:-$root/../darling-prefix/bin/darling}"
dprefix="${DPREFIX:-$root/../darling-prefix-homebrew-test}"
baseline="${BASELINE_SERVER:-$root/../darling-debug/bin/darlingserver-generic-baseline-amplified}"
fixed="${FIXED_SERVER:-$root/../darling-debug/bin/darlingserver-generic-permit-amplified}"
iterations="${ITERATIONS:-3}"

ruby_command='/usr/local/Homebrew/Library/Homebrew/vendor/portable-ruby/current/bin/ruby -e '\''far=Thread.new{sleep 3600};ts=5.times.map{Thread.new{10.times{sleep(rand*0.004+0.0001)}}};ts.each(&:join);far.kill;puts "done"'\'''

for required in "$runner" "$darling" "$baseline" "$fixed"; do
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

run_variant baseline "$baseline"
run_variant fixed "$fixed"
