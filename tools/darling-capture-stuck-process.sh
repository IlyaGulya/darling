#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 [options]" >&2
	echo "  --output DIR        directory for capture bundles (default: \$DARLING_DEBUG_LOG_ROOT or ~/work/darling-debug)" >&2
	echo "  --name NAME         capture name (default: stuck-process)" >&2
	echo "  --comm COMM         exact process comm to match, e.g. mldr" >&2
	echo "  --pattern PATTERN   substring or regex to match against command line" >&2
	echo "  --wchan WCHAN       exact kernel wait channel to match" >&2
	echo "  --pid PID           capture this PID directly instead of polling" >&2
	echo "  --timeout SECONDS   polling timeout (default: 60)" >&2
	echo "  --interval SECONDS  polling interval (default: 1)" >&2
	echo "  --strace-timeout S  strace attach timeout (default: 8; 0 disables)" >&2
	echo "  --gdb-timeout S     gdb attach timeout (default: 25; 0 disables)" >&2
	exit 2
}

log_root="${DARLING_DEBUG_LOG_ROOT:-$HOME/work/darling-debug}"
name="stuck-process"
comm=""
pattern=""
wchan=""
pid=""
timeout_seconds=60
interval_seconds=1
strace_timeout=8
gdb_timeout=25

while [ "$#" -gt 0 ]; do
	case "$1" in
		--output)
			shift || usage
			log_root="$1"
			;;
		--name)
			shift || usage
			name="$1"
			;;
		--comm)
			shift || usage
			comm="$1"
			;;
		--pattern)
			shift || usage
			pattern="$1"
			;;
		--wchan)
			shift || usage
			wchan="$1"
			;;
		--pid)
			shift || usage
			pid="$1"
			;;
		--timeout)
			shift || usage
			timeout_seconds="$1"
			;;
		--interval)
			shift || usage
			interval_seconds="$1"
			;;
		--strace-timeout)
			shift || usage
			strace_timeout="$1"
			;;
		--gdb-timeout)
			shift || usage
			gdb_timeout="$1"
			;;
		-h|--help)
			usage
			;;
		*)
			usage
			;;
	esac
	shift
done

safe_name=$(printf '%s' "$name" | tr -c 'A-Za-z0-9_.-' '_')
stamp=$(date -u +%Y%m%dT%H%M%SZ)
bundle="$log_root/$stamp-$safe_name"
mkdir -p "$bundle"

sudo_cmd=()
if command -v sudo >/dev/null 2>&1; then
	sudo_cmd=(sudo -n)
fi

run_optional() {
	local output="$1"
	shift
	"$@" >"$output" 2>&1 || true
}

poll_for_pid() {
	local deadline=$((SECONDS + timeout_seconds))

	while [ "$SECONDS" -le "$deadline" ]; do
		ps -eLo pid=,ppid=,tid=,stat=,wchan:32=,comm=,args= |
			awk -v comm="$comm" -v pattern="$pattern" -v wchan="$wchan" '
				{
					pid=$1; ppid=$2; tid=$3; stat=$4; wait=$5; proc=$6;
					args=$0;
					sub(/^[[:space:]]*[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+[^[:space:]]+[[:space:]]+[^[:space:]]+[[:space:]]+[^[:space:]]+[[:space:]]*/, "", args);
					if (comm != "" && proc != comm) next;
					if (wchan != "" && wait != wchan) next;
					if (pattern != "" && args !~ pattern) next;
					print pid;
					exit;
				}
			'
		sleep "$interval_seconds"
	done
}

if [ -z "$pid" ]; then
	pid=$(poll_for_pid | head -1 || true)
fi

{
	echo "timestamp_utc=$stamp"
	echo "name=$name"
	echo "comm=$comm"
	echo "pattern=$pattern"
	echo "wchan=$wchan"
	echo "timeout_seconds=$timeout_seconds"
	echo "interval_seconds=$interval_seconds"
	echo "strace_timeout=$strace_timeout"
	echo "gdb_timeout=$gdb_timeout"
	echo "pid=$pid"
} >"$bundle/capture.env"

run_optional "$bundle/ps-full.txt" ps -eLo pid,ppid,tid,stat,wchan:32,comm,args

if [ -z "$pid" ]; then
	echo "no matching process found" >"$bundle/not-found.txt"
	echo "$bundle"
	exit 1
fi

run_optional "$bundle/ps-target.txt" ps -p "$pid" -Lo pid,ppid,tid,stat,wchan:32,comm,args

for proc_file in status cmdline wchan syscall stack maps; do
	if [ -r "/proc/$pid/$proc_file" ]; then
		run_optional "$bundle/proc-$proc_file.txt" cat "/proc/$pid/$proc_file"
	elif [ "${#sudo_cmd[@]}" -gt 0 ]; then
		run_optional "$bundle/proc-$proc_file.txt" "${sudo_cmd[@]}" cat "/proc/$pid/$proc_file"
	else
		echo "not readable: /proc/$pid/$proc_file" >"$bundle/proc-$proc_file.txt"
	fi
done

if [ "$strace_timeout" = "0" ]; then
	echo "strace disabled" >"$bundle/strace.txt"
elif command -v strace >/dev/null 2>&1; then
	if [ "${#sudo_cmd[@]}" -gt 0 ]; then
		run_optional "$bundle/strace.txt" timeout "${strace_timeout}s" "${sudo_cmd[@]}" strace -f -tt -T -p "$pid"
	else
		run_optional "$bundle/strace.txt" timeout "${strace_timeout}s" strace -f -tt -T -p "$pid"
	fi
else
	echo "strace not found" >"$bundle/strace.txt"
fi

if [ "$gdb_timeout" = "0" ]; then
	echo "gdb disabled" >"$bundle/gdb.txt"
elif command -v gdb >/dev/null 2>&1; then
	if [ "${#sudo_cmd[@]}" -gt 0 ]; then
		run_optional "$bundle/gdb.txt" timeout "${gdb_timeout}s" "${sudo_cmd[@]}" gdb -q -p "$pid" -batch \
			-ex "set pagination off" \
			-ex "info threads" \
			-ex "thread apply all bt"
	else
		run_optional "$bundle/gdb.txt" timeout "${gdb_timeout}s" gdb -q -p "$pid" -batch \
			-ex "set pagination off" \
			-ex "info threads" \
			-ex "thread apply all bt"
	fi
else
	echo "gdb not found" >"$bundle/gdb.txt"
fi

echo "$bundle"
