#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 [--lines N] BUNDLE_DIR" >&2
	exit 2
}

lines=80

while (($#)); do
	case "$1" in
		--lines)
			shift
			(($#)) || usage
			lines="$1"
			shift
			;;
		-h|--help)
			usage
			;;
		-*)
			usage
			;;
		*)
			break
			;;
	esac
done

(($# == 1)) || usage
bundle="${1%/}"

if [[ ! -d "$bundle" ]]; then
	echo "bundle not found: $bundle" >&2
	exit 2
fi

section() {
	printf '\n== %s ==\n' "$1"
}

show_file() {
	local label="$1"
	local file="$2"

	[[ -s "$file" ]] || return 0
	section "$label"
	cat "$file"
	return 0
}

show_tail() {
	local label="$1"
	local file="$2"

	[[ -s "$file" ]] || return 0
	section "$label"
	tail -n "$lines" "$file"
	return 0
}

truncate_lines() {
	awk 'length($0) > 260 { print substr($0, 1, 260) " ..."; next } { print }'
}

show_ps_focus() {
	local label="$1"
	local file="$2"

	[[ -s "$file" ]] || return 0
	section "$label"
	awk '
		NR == 1 ||
		/tanuki|darling|darlingserver|mldr|brew|ruby|curl|git|clang|pkgutil|installer|system_command|xcrun|xcode|uname/ {
			print
		}
	' "$file" | truncate_lines | tail -n "$lines" || true
	return 0
}

show_capture() {
	local capture="$1"

	[[ -d "$capture" ]] || return 0
	section "capture $(basename "$capture")"
	[[ -s "$capture/capture.env" ]] && cat "$capture/capture.env"
	[[ -s "$capture/ps-target.txt" ]] && {
		printf '\n-- ps-target --\n'
		truncate_lines <"$capture/ps-target.txt"
	}
	[[ -s "$capture/proc-wchan.txt" ]] && {
		printf '\n-- wchan --\n'
		cat "$capture/proc-wchan.txt"
	}
	[[ -s "$capture/strace.txt" ]] && {
		printf '\n-- strace tail --\n'
		tail -n 80 "$capture/strace.txt"
	}
	[[ -s "$capture/gdb.txt" ]] && {
		printf '\n-- gdb head --\n'
		sed -n '1,120p' "$capture/gdb.txt"
	}
	return 0
}

section "bundle"
printf '%s\n' "$bundle"
show_file "meta" "$bundle/meta.txt"
show_file "exit" "$bundle/exit-code.txt"
show_file "timeout" "$bundle/timeout.txt"
show_file "timeout capture" "$bundle/timeout-capture.txt"
show_tail "runner" "$bundle/runner-state.log"
show_tail "stderr" "$bundle/stderr.log"
show_tail "stdout" "$bundle/stdout.log"
show_ps_focus "processes at timeout" "$bundle/ps.timeout.txt"
show_ps_focus "last sampled live processes" "$bundle/ps.live.txt"
show_ps_focus "processes after cleanup" "$bundle/ps.after.txt"
show_tail "xtrace" "$bundle/xtrace.log"
for xtrace_file in "$bundle"/xtrace.log.*; do
	[[ -e "$xtrace_file" ]] || continue
	show_tail "xtrace $(basename "$xtrace_file")" "$xtrace_file"
done
show_tail "cleanup stderr" "$bundle/cleanup.err"
show_tail "timeout cleanup stderr" "$bundle/timeout-cleanup.err"

if [[ -d "$bundle/captures" ]]; then
	while IFS= read -r capture; do
		show_capture "$capture"
	done < <(find "$bundle/captures" -mindepth 1 -maxdepth 1 -type d | sort)
fi
