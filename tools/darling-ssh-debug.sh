#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 [--host HOST] [--workdir DIR] [--dprefix DIR] [--timeout SECONDS] [--lines N] [--xtrace] [--no-summary] NAME [script-file]" >&2
	echo "       script may also be provided on stdin" >&2
	exit 2
}

host="nhs"
workdir="\$HOME/work/darling"
dprefix=""
timeout_seconds=120
lines=80
use_xtrace=0
show_summary=1
name=""
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
		--dprefix)
			shift || usage
			dprefix="$1"
			;;
		--timeout)
			shift || usage
			timeout_seconds="$1"
			;;
		--lines)
			shift || usage
			lines="$1"
			;;
		--xtrace)
			use_xtrace=1
			;;
		--no-summary)
			show_summary=0
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
			if [[ -z "$name" ]]; then
				name="$1"
			elif [[ -z "$script_file" ]]; then
				script_file="$1"
			else
				usage
			fi
			;;
	esac
	shift
done

[[ -n "$name" ]] || usage
if (($#)); then
	usage
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ssh_run="$script_dir/darling-ssh-run.sh"

args=(--host "$host" --workdir "$workdir" --darling "$name" --timeout "$timeout_seconds")
if [[ -n "$dprefix" ]]; then
	args+=(--dprefix "$dprefix")
fi
if ((use_xtrace)); then
	args+=(--xtrace)
fi
if [[ -n "$script_file" ]]; then
	args+=("$script_file")
fi

set +e
output="$("$ssh_run" "${args[@]}" 2>&1)"
rc=$?
set -e

printf '%s\n' "$output"

bundle="$(printf '%s\n' "$output" | awk -F= '/^BUNDLE=/{print $2}' | tail -1)"
if ((show_summary)) && [[ -n "$bundle" ]]; then
	ssh -n "$host" "cd $workdir; ./tools/darling-debug-summary.sh --lines '$lines' '$bundle'" || true
fi

exit "$rc"
