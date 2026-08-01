#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ] || [ ! -x "$1" ]; then
	printf 'usage: %s SHELLSPAWN_BINARY\n' "$0" >&2
	exit 2
fi

socket=/tmp/shellspawn.sock
if [ -e "$socket" ] || [ -L "$socket" ]; then
	printf 'test socket already exists: %s\n' "$socket" >&2
	exit 3
fi

scratch="$(mktemp -d /tmp/darling-shellspawn-shutdown.XXXXXX)"
pid=
cleanup() {
	if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
		kill -KILL "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
	if [ -S "$socket" ]; then
		rm -f -- "$socket"
	fi
	rm -rf -- "$scratch"
}
trap cleanup EXIT INT TERM

env -u DARLING_ROOTLESS -u DARLING_NOOVERLAYFS -u DARLING_EUNION \
	DARLING_RUNTIME_MODE=rootless-eunion \
	"$1" >"$scratch/stdout" 2>"$scratch/stderr" &
pid=$!

for _attempt in $(seq 1 200); do
	if [ -S "$socket" ]; then
		break
	fi
	if ! kill -0 "$pid" 2>/dev/null; then
		wait "$pid" || true
		printf 'shellspawn exited before socket readiness\n' >&2
		cat "$scratch/stderr" >&2
		exit 4
	fi
	sleep 0.01
done
if [ ! -S "$socket" ]; then
	printf 'shellspawn socket did not become ready\n' >&2
	exit 5
fi

kill -TERM "$pid"
set +e
wait "$pid"
rc=$?
set -e
pid=
if [ "$rc" -ne 0 ]; then
	printf 'shellspawn SIGTERM shutdown failed: rc=%s\n' "$rc" >&2
	cat "$scratch/stderr" >&2
	exit 6
fi
if [ -e "$socket" ] || [ -L "$socket" ]; then
	printf 'shellspawn socket survived graceful shutdown\n' >&2
	exit 7
fi

printf 'ROOTLESS_SHELLSPAWN_SHUTDOWN_OK signal=SIGTERM socket_removed=PASS\n'
