#!/usr/bin/env bash
set -euo pipefail

ipc_source="${1:?usage: rootless_launchd_endpoint_contract.sh IPC_SOURCE}"
function_body="$({
	awk '
		/^ipc_server_shutdown\(void\)$/ { capture = 1 }
		capture { print }
		capture && /^}$/ { exit }
	' "$ipc_source"
} || true)"

if [ -z "$function_body" ]; then
	printf 'ROOTLESS_LAUNCHD_ENDPOINT_CONTRACT_MISSING function=ipc_server_shutdown\n' >&2
	exit 42
fi
if [ "$(printf '%s\n' "$function_body" | grep -Fc 'unlink(sockpath)')" -ne 1 ]; then
	printf 'ROOTLESS_LAUNCHD_ENDPOINT_CONTRACT_INVALID unlink_count\n' >&2
	exit 42
fi
if printf '%s\n' "$function_body" | grep -Fq 'rmdir(sockdir)'; then
	printf 'ROOTLESS_LAUNCHD_ENDPOINT_PARENT_REMOVED path=/var/tmp/launchd\n' >&2
	exit 42
fi
if [ "$(printf '%s\n' "$function_body" | grep -Fc 'ipc_inited = false;')" -ne 1 ]; then
	printf 'ROOTLESS_LAUNCHD_ENDPOINT_CONTRACT_INVALID state_reset\n' >&2
	exit 42
fi

printf 'ROOTLESS_LAUNCHD_ENDPOINT_CONTRACT_OK socket=removed parent=preserved\n'
