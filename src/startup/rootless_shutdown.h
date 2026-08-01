#ifndef DARLING_ROOTLESS_SHUTDOWN_H
#define DARLING_ROOTLESS_SHUTDOWN_H

#include "runtime_mode_prefix.h"

#include <stddef.h>
#include <sys/types.h>

enum rootless_shutdown_phase {
	ROOTLESS_SHUTDOWN_RUNNING,
	ROOTLESS_SHUTDOWN_TERM,
	ROOTLESS_SHUTDOWN_DRAINING,
	ROOTLESS_SHUTDOWN_KILL,
	ROOTLESS_SHUTDOWN_DRAINED,
	ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED,
	ROOTLESS_SHUTDOWN_STOPPED,
};

struct rootless_shutdown_policy {
	unsigned term_timeout_ms;
	unsigned kill_timeout_ms;
	unsigned poll_interval_ms;
};

struct rootless_shutdown_result {
	enum rootless_shutdown_phase phase;
	pid_t session;
	unsigned term_rounds;
	unsigned kill_rounds;
};

const char* rootless_shutdown_phase_name(enum rootless_shutdown_phase phase);

int shutdown_rootless_runtime(
	pid_t session_member,
	pid_t init_process,
	const darling_runtime_prefix prefix,
	const struct rootless_shutdown_policy* policy,
	struct rootless_shutdown_result* result,
	char* error,
	size_t error_size
);

#endif
