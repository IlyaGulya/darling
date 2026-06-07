#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 [--host HOST] [--workdir DIR] [--darling NAME] [--dprefix DIR] [--timeout SECONDS] [--xtrace] [--keep] [script-file]" >&2
	echo "       script may also be provided on stdin" >&2
	exit 2
}

host="nhs"
workdir="\$HOME/work/darling"
darling_name=""
dprefix=""
timeout_seconds=120
use_xtrace=0
keep_remote=0
script_file=""

while (($#)); do
	case "$1" in
		--host)
			shift || usage
			host="$1"
			;;
		--workdir)
			shift || usage
			workdir="$1"
			;;
		--darling)
			shift || usage
			darling_name="$1"
			;;
		--dprefix)
			shift || usage
			dprefix="$1"
			;;
		--timeout)
			shift || usage
			timeout_seconds="$1"
			;;
		--xtrace)
			use_xtrace=1
			;;
		--keep)
			keep_remote=1
			;;
		-h|--help)
			usage
			;;
		--)
			shift
			break
			;;
		-*)
			usage
			;;
		*)
			if [[ -n "$script_file" ]]; then
				usage
			fi
			script_file="$1"
			;;
	esac
	shift
done

if (($#)); then
	usage
fi

local_script=""
cleanup_local() {
	if [[ -n "$local_script" && -f "$local_script" ]]; then
		rm -f "$local_script"
	fi
}
trap cleanup_local EXIT

if [[ -n "$script_file" ]]; then
	if [[ ! -f "$script_file" ]]; then
		echo "script file not found: $script_file" >&2
		exit 2
	fi
	local_script="$script_file"
else
	tmpdir="${TMPDIR:-/tmp}"
	local_script="$(mktemp "${tmpdir%/}/darling-ssh-run.XXXXXX")"
	cat >"$local_script"
fi

remote_script="/tmp/darling-ssh-run-$(date -u +%Y%m%dT%H%M%SZ)-$$.sh"
scp -q "$local_script" "$host:$remote_script"

remote_cleanup="rm -f '$remote_script'"
if ((keep_remote)); then
	remote_cleanup=":"
fi

if [[ -n "$darling_name" ]]; then
	xtrace_arg=()
	if ((use_xtrace)); then
		xtrace_arg=(--xtrace)
	fi
	dprefix_arg=()
	if [[ -n "$dprefix" ]]; then
		dprefix_arg=(--dprefix "$dprefix")
	fi
	ssh -n "$host" "set -euo pipefail; chmod +x '$remote_script'; cd $workdir; ./tools/darling-debug-run.sh --timeout '$timeout_seconds' ${dprefix_arg[*]} ${xtrace_arg[*]} '$darling_name' -- '/bin/bash /Volumes/SystemRoot$remote_script'; rc=\$?; $remote_cleanup; exit \$rc"
else
	ssh -n "$host" "set -euo pipefail; chmod +x '$remote_script'; '$remote_script'; rc=\$?; $remote_cleanup; exit \$rc"
fi
