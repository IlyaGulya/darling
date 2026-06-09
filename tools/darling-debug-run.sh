#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 [--timeout seconds] [--dprefix dir] [--name name] [--xtrace] [--cleanup] name -- darling-shell-command" >&2
	exit 2
}

prefix="${DARLING_PREFIX:-$HOME/work/darling-prefix}"
dprefix="${DPREFIX:-$HOME/.darling}"
log_root="${DARLING_DEBUG_LOG_ROOT:-$HOME/work/darling-debug}"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cleanup_script="$script_dir/darling-debug-cleanup.sh"
if [[ ! -x "$cleanup_script" ]]; then
	cleanup_script="$HOME/work/darling-debug-cleanup.sh"
fi
capture_script="$script_dir/darling-capture-stuck-process.sh"
if [[ ! -x "$capture_script" ]]; then
	capture_script="$HOME/work/darling-capture-stuck-process.sh"
fi
timeout_seconds=120
use_xtrace=0
name_arg=""
capture_on_timeout=1
lock_fd=9
lock_file=""
lock_dir=""
have_flock=0

send_signal() {
	local signal="$1"
	local target="$2"

	kill -s "$signal" -- "$target" 2>/dev/null || sudo kill -s "$signal" -- "$target" 2>/dev/null || true
}

kill_children() {
	local parent="$1"
	local child

	while read -r child; do
		[[ -n "$child" ]] || continue
		kill_children "$child"
		send_signal TERM "$child"
	done < <(pgrep -P "$parent" 2>/dev/null || true)
}

capture_timeout_target() {
	local capture_pid

	ps -eLo pid,ppid,pgid,sid,stat,etime,wchan:32,comm,args >"$bundle/ps.timeout.txt" 2>/dev/null || true
	capture_pid="$(
		awk '
			$5 !~ /^Z/ && ($8 == "mldr" || $8 == "system_command.") && $0 ~ /(portable-ruby|\/ruby)/ && $0 !~ /\/bin\/bash/ {
				print $1
				exit
			}
		' "$bundle/ps.timeout.txt" 2>/dev/null
	)"
	if [[ -z "$capture_pid" ]]; then
		capture_pid="$(
			awk '
				$5 !~ /^Z/ && ($8 == "mldr" || $8 == "system_command.") && $0 ~ /(ruby|brew|system_command|curl)/ {
					print $1
					exit
				}
			' "$bundle/ps.timeout.txt" 2>/dev/null
		)"
	fi
	if [[ -z "$capture_pid" ]]; then
		capture_pid="$(
			awk '
				$5 !~ /^Z/ && $8 == "mldr" && $0 !~ /(launchd|opendirectoryd|memberd|securityd|shellspawn|iokitd)/ {
					print $1
					exit
				}
			' "$bundle/ps.timeout.txt" 2>/dev/null
		)"
	fi
	if [[ -z "$capture_pid" ]]; then
		capture_pid="$(
			awk -v pgid="$darling_pid" '
				$3 == pgid && $5 !~ /^Z/ && $8 != "bash" && $8 != "darling" && $8 != "darlingserver" {
					print $1
					exit
				}
			' "$bundle/ps.timeout.txt" 2>/dev/null
		)"
	fi
	if [[ -z "$capture_pid" ]]; then
		echo "no timeout capture target found" >"$bundle/timeout-capture.txt"
		return
	fi

	echo "$capture_pid" >"$bundle/timeout-capture-pid.txt"
	if [[ -x "$capture_script" ]]; then
		mkdir -p "$bundle/captures"
		timeout -k 3s 20s "$capture_script" \
			--output "$bundle/captures" \
			--name "$safe_name-timeout" \
			--pid "$capture_pid" \
			--strace-timeout 2 \
			--gdb-timeout 8 >"$bundle/timeout-capture.txt" 2>&1 || true
	else
		echo "capture script not found: $capture_script" >"$bundle/timeout-capture.txt"
	fi
}

cleanup_lock() {
	if ((have_flock)); then
		flock -u "$lock_fd" 2>/dev/null || true
	elif [[ -n "$lock_dir" ]]; then
		rmdir "$lock_dir" 2>/dev/null || true
	fi
}

acquire_prefix_lock() {
	local lock_root safe_dprefix

	lock_root="${TMPDIR:-/tmp}/darling-debug-run-locks"
	mkdir -p "$lock_root"
	safe_dprefix="$(printf '%s' "$dprefix" | tr -c 'A-Za-z0-9_.-' '_')"

	if command -v flock >/dev/null 2>&1; then
		have_flock=1
		lock_file="$lock_root/$safe_dprefix.lock"
		eval "exec $lock_fd>\"\$lock_file\""
		if ! flock -n "$lock_fd"; then
			echo "another darling-debug-run is active for dprefix: $dprefix" >&2
			echo "lock: $lock_file" >&2
			exit 75
		fi
		printf 'pid=%s\nstarted=%s\ndprefix=%s\n' "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$dprefix" >&"$lock_fd"
	else
		lock_dir="$lock_root/$safe_dprefix.lockdir"
		if ! mkdir "$lock_dir" 2>/dev/null; then
			echo "another darling-debug-run is active for dprefix: $dprefix" >&2
			echo "lock: $lock_dir" >&2
			exit 75
		fi
		printf 'pid=%s\nstarted=%s\ndprefix=%s\n' "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$dprefix" >"$lock_dir/owner"
	fi
	trap cleanup_lock EXIT
}

while (($#)); do
	case "$1" in
		--timeout)
			shift
			(($#)) || usage
			timeout_seconds="$1"
			shift
			;;
		--dprefix)
			shift
			(($#)) || usage
			dprefix="$1"
			shift
			;;
		--name)
			shift
			(($#)) || usage
			name_arg="$1"
			shift
			;;
		--xtrace)
			use_xtrace=1
			shift
			;;
		--no-capture)
			capture_on_timeout=0
			shift
			;;
		--cleanup)
			# Cleanup is always performed; keep this accepted for older invocations.
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

if [[ -n "$name_arg" ]]; then
	name="$name_arg"
	[[ "${1:-}" == "--" ]] && shift
else
	(($# >= 2)) || usage
	name="$1"
	shift
	[[ "${1:-}" == "--" ]] && shift
fi
(($#)) || usage

acquire_prefix_lock

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
safe_name="$(printf '%s' "$name" | tr -c 'A-Za-z0-9_.-' '_')"
bundle="$log_root/$stamp-$safe_name"
darwin_bundle="/Users/$USER/darling-debug/$stamp-$safe_name"
darwin_host_bundle="$dprefix/Users/$USER/darling-debug/$stamp-$safe_name"
mkdir -p "$bundle"
mkdir -p "$darwin_host_bundle"

cmd="$*"
guest_env="unset BASH_ENV; export DPREFIX=$(printf '%q' "$dprefix");"
inner="$guest_env $cmd"
if ((use_xtrace)); then
	inner="$guest_env export XTRACE_NO_COLOR=1 XTRACE_LOG_FILE='$darwin_bundle/xtrace.log'; /usr/bin/xtrace /bin/bash -lc $(printf '%q' "$cmd")"
fi

{
	echo "date_utc=$stamp"
	echo "host=$(hostname)"
	echo "prefix=$prefix"
	echo "dprefix=$dprefix"
	echo "lock_file=${lock_file:-$lock_dir}"
	echo "darwin_bundle=$darwin_bundle"
	echo "timeout_seconds=$timeout_seconds"
	echo "xtrace=$use_xtrace"
	echo "capture_on_timeout=$capture_on_timeout"
	echo "command=$cmd"
} >"$bundle/meta.txt"

ps -eo pid,ppid,pgid,sid,stat,etime,cmd >"$bundle/ps.before.txt" || true
cp "$dprefix/private/var/log/dserver.log" "$bundle/dserver.before.log" 2>/dev/null || true

env -u BASH_ENV DPREFIX="$dprefix" "$prefix/bin/darling" shutdown >>"$bundle/preflight.out" 2>>"$bundle/preflight.err" || true
sudo umount -R "$dprefix/proc" >>"$bundle/preflight.out" 2>>"$bundle/preflight.err" || sudo umount -l "$dprefix/proc" >>"$bundle/preflight.out" 2>>"$bundle/preflight.err" || true

set +e
(while true; do
	ps -eo pid,ppid,pgid,sid,stat,etime,cmd >"$bundle/ps.live.txt" 2>/dev/null || true
	sleep 2
done) &
monitor_pid=$!

if command -v setsid >/dev/null 2>&1; then
	setsid env -u BASH_ENV DPREFIX="$dprefix" "$prefix/bin/darling" shell /bin/bash -lc "$inner" >"$bundle/stdout.log" 2>"$bundle/stderr.log" &
else
	env -u BASH_ENV DPREFIX="$dprefix" "$prefix/bin/darling" shell /bin/bash -lc "$inner" >"$bundle/stdout.log" 2>"$bundle/stderr.log" &
fi
darling_pid=$!
{
	echo "started darling_pid=$darling_pid at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "timeout_seconds=$timeout_seconds"
} >>"$bundle/runner-state.log"
rc=0
timed_out=0

(echo "watchdog sleeping at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
sleep "$timeout_seconds"
echo "watchdog woke at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if ps -p "$darling_pid" >/dev/null 2>&1; then
	echo "watchdog killing darling_pid=$darling_pid"
	echo "timeout after ${timeout_seconds}s" >"$bundle/timeout.txt"
	if ((capture_on_timeout)); then
		echo "watchdog capturing timeout target"
		capture_timeout_target
	fi
	send_signal TERM "-$darling_pid"
	send_signal TERM "$darling_pid"
else
	echo "watchdog saw darling_pid=$darling_pid already gone"
fi) >>"$bundle/runner-state.log" 2>&1 &
watchdog_pid=$!
echo "started watchdog_pid=$watchdog_pid at $(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$bundle/runner-state.log"

wait "$darling_pid" 2>>"$bundle/runner-state.log"
rc=$?
echo "wait returned rc=$rc at $(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$bundle/runner-state.log"

if [[ -f "$bundle/timeout.txt" ]]; then
	timed_out=1
	rc=124
else
	kill_children "$watchdog_pid"
	kill "$watchdog_pid" 2>/dev/null || true
	wait "$watchdog_pid" 2>/dev/null || true
fi

if ((timed_out)); then
	wait "$watchdog_pid" 2>/dev/null || true
	timeout -k 3s 20s env DARLING_PREFIX="$prefix" DARLING_DPREFIX="$dprefix" "$cleanup_script" "$darling_pid" >"$bundle/timeout-cleanup.out" 2>"$bundle/timeout-cleanup.err" || true
	if ps -p "$darling_pid" >/dev/null 2>&1; then
		echo "timeout after ${timeout_seconds}s" >"$bundle/timeout.txt"
	fi
	wait "$darling_pid" 2>/dev/null || true
	rc=124
fi

kill "$monitor_pid" 2>/dev/null || true
wait "$monitor_pid" 2>/dev/null || true

set -e

timeout -k 3s 20s env DARLING_PREFIX="$prefix" DARLING_DPREFIX="$dprefix" "$cleanup_script" >"$bundle/cleanup.out" 2>"$bundle/cleanup.err" || true

cp "$darwin_host_bundle"/xtrace.log* "$bundle"/ 2>/dev/null || true

ps -eo pid,ppid,pgid,sid,stat,etime,cmd >"$bundle/ps.after.txt" || true
cp "$dprefix/private/var/log/dserver.log" "$bundle/dserver.after.log" 2>/dev/null || true
journalctl --user -n 300 >"$bundle/journal.user.tail.log" 2>/dev/null || true
journalctl -n 300 >"$bundle/journal.system.tail.log" 2>/dev/null || true

printf '%s\n' "$rc" >"$bundle/exit-code.txt"
printf 'BUNDLE=%s\nRC=%s\n' "$bundle" "$rc"
exit "$rc"
