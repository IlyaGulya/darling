#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 [--timeout seconds] [--xtrace] name -- darling-shell-command" >&2
	exit 2
}

prefix="${DARLING_PREFIX:-$HOME/work/darling-prefix}"
log_root="${DARLING_DEBUG_LOG_ROOT:-$HOME/work/darling-debug}"
timeout_seconds=120
use_xtrace=0

while (($#)); do
	case "$1" in
		--timeout)
			shift
			(($#)) || usage
			timeout_seconds="$1"
			shift
			;;
		--xtrace)
			use_xtrace=1
			shift
			;;
		--)
			shift
			break
			;;
		-*)
			usage
			;;
		*)
			break
			;;
	esac
done

(($# >= 2)) || usage
name="$1"
shift
[[ "${1:-}" == "--" ]] && shift
(($#)) || usage

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
safe_name="$(printf '%s' "$name" | tr -c 'A-Za-z0-9_.-' '_')"
bundle="$log_root/$stamp-$safe_name"
darwin_bundle="/Users/$USER/darling-debug/$stamp-$safe_name"
darwin_host_bundle="$HOME/.darling/Users/$USER/darling-debug/$stamp-$safe_name"
mkdir -p "$bundle"
mkdir -p "$darwin_host_bundle"

cmd="$*"
inner="$cmd"
if ((use_xtrace)); then
	inner="export XTRACE_NO_COLOR=1 XTRACE_LOG_FILE='$darwin_bundle/xtrace.log'; /usr/bin/xtrace /bin/bash -lc $(printf '%q' "$cmd")"
fi

{
	echo "date_utc=$stamp"
	echo "host=$(hostname)"
	echo "prefix=$prefix"
	echo "darwin_bundle=$darwin_bundle"
	echo "timeout_seconds=$timeout_seconds"
	echo "xtrace=$use_xtrace"
	echo "command=$cmd"
} >"$bundle/meta.txt"

ps -eo pid,ppid,pgid,sid,stat,etime,cmd >"$bundle/ps.before.txt" || true
cp "$HOME/.darling/private/var/log/dserver.log" "$bundle/dserver.before.log" 2>/dev/null || true

"$prefix/bin/darling" shutdown >>"$bundle/preflight.out" 2>>"$bundle/preflight.err" || true
sudo umount -R "$HOME/.darling/proc" >>"$bundle/preflight.out" 2>>"$bundle/preflight.err" || sudo umount -l "$HOME/.darling/proc" >>"$bundle/preflight.out" 2>>"$bundle/preflight.err" || true

set +e
(while true; do
	ps -eo pid,ppid,pgid,sid,stat,etime,cmd >"$bundle/ps.live.txt" 2>/dev/null || true
	sleep 2
done) &
monitor_pid=$!

timeout -k 5s "${timeout_seconds}s" "$prefix/bin/darling" shell /bin/bash -lc "$inner" >"$bundle/stdout.log" 2>"$bundle/stderr.log"
rc=$?

kill "$monitor_pid" 2>/dev/null || true
wait "$monitor_pid" 2>/dev/null || true

if ((rc == 124 || rc == 137)); then
	echo "timeout after ${timeout_seconds}s" >"$bundle/timeout.txt"
	rc=124
fi
set -e

DARLING_PREFIX="$prefix" "$HOME/work/darling-debug-cleanup.sh" >"$bundle/cleanup.out" 2>"$bundle/cleanup.err" || true

cp "$darwin_host_bundle"/xtrace.log* "$bundle"/ 2>/dev/null || true

ps -eo pid,ppid,pgid,sid,stat,etime,cmd >"$bundle/ps.after.txt" || true
cp "$HOME/.darling/private/var/log/dserver.log" "$bundle/dserver.after.log" 2>/dev/null || true
journalctl --user -n 300 >"$bundle/journal.user.tail.log" 2>/dev/null || true
journalctl -n 300 >"$bundle/journal.system.tail.log" 2>/dev/null || true

printf '%s\n' "$rc" >"$bundle/exit-code.txt"
printf 'BUNDLE=%s\nRC=%s\n' "$bundle" "$rc"
exit "$rc"
