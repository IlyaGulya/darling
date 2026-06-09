#!/usr/bin/env bash
set -euo pipefail

prefix="${DARLING_PREFIX:-$HOME/work/darling-prefix}"
dprefix="${DARLING_DPREFIX:-${DPREFIX:-$HOME/.darling}}"

kill_matching() {
	local pattern="$1"
	local signal="${2:-TERM}"
	local pid

	while read -r pid; do
		[[ -n "$pid" ]] || continue
		kill "-$signal" "$pid" 2>/dev/null || sudo kill "-$signal" "$pid" 2>/dev/null || true
	done < <(pgrep -f "$pattern" 2>/dev/null || true)
}

kill_tree() {
	local pid="$1"
	local children child

	children="$(pgrep -P "$pid" 2>/dev/null || true)"
	for child in $children; do
		kill_tree "$child"
	done

	kill -TERM "$pid" 2>/dev/null || sudo kill -TERM "$pid" 2>/dev/null || true
}

for pid in "$@"; do
	if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
		kill_tree "$pid"
	fi
done

timeout -k 3s 10s env DPREFIX="$dprefix" "$prefix/bin/darling" shutdown >/dev/null 2>&1 || true
timeout -k 3s 10s sudo umount -R "$dprefix/proc" 2>/dev/null || timeout -k 3s 10s sudo umount -l "$dprefix/proc" 2>/dev/null || true

kill_matching "^$prefix/bin/darling shell " TERM
kill_matching "^/Library/Developer/CommandLineTools/usr/bin/" TERM
kill_matching "^/usr/libexec/shellspawn$" TERM
kill_matching "^/usr/sbin/memberd " TERM
kill_matching "^/usr/sbin/securityd " TERM
kill_matching "^/sbin/launchd$" TERM
kill_matching "^darlingserver $dprefix" TERM
