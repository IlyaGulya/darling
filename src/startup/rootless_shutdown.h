#ifndef DARLING_ROOTLESS_SHUTDOWN_H
#define DARLING_ROOTLESS_SHUTDOWN_H

#include "runtime_mode_prefix.h"

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

enum rootless_shutdown_phase {
	ROOTLESS_SHUTDOWN_RUNNING,
	ROOTLESS_SHUTDOWN_CLOSURE_BOUND,
	ROOTLESS_SHUTDOWN_QUIESCING,
	ROOTLESS_SHUTDOWN_TERM,
	ROOTLESS_SHUTDOWN_DRAINING,
	ROOTLESS_SHUTDOWN_KILL,
	ROOTLESS_SHUTDOWN_DRAINED,
	ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED,
	ROOTLESS_SHUTDOWN_STOPPED,
};

struct rootless_shutdown_policy {
	unsigned quiesce_timeout_ms;
	unsigned term_timeout_ms;
	unsigned kill_timeout_ms;
	unsigned poll_interval_ms;
};

struct rootless_shutdown_result {
	enum rootless_shutdown_phase phase;
	pid_t session;
	ino_t closure_inode;
	size_t identities_observed;
	unsigned quiesce_rounds;
	unsigned term_rounds;
	unsigned kill_rounds;
};

struct rootless_shutdown_closure_capability {
	int parent_fd;
	int directory_fd;
	int membership_fd;
	char leaf[NAME_MAX + 1];
};

#define ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER { \
	.parent_fd = -1, .directory_fd = -1, .membership_fd = -1, .leaf = {0} \
}

int rootless_shutdown_prepare_closure(
	const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	char* error,
	size_t error_size
);

int rootless_shutdown_enter_closure(
	const struct rootless_shutdown_closure_capability* capability,
	char* error,
	size_t error_size
);

int rootless_shutdown_closure_contains(
	const struct rootless_shutdown_closure_capability* capability,
	pid_t pid,
	char* error,
	size_t error_size
);

int rootless_shutdown_cleanup_empty_closure(
	struct rootless_shutdown_closure_capability* capability,
	char* error,
	size_t error_size
);

void rootless_shutdown_release_closure(
	struct rootless_shutdown_closure_capability* capability
);

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
