#ifndef DARLING_ROOTLESS_SHUTDOWN_H
#define DARLING_ROOTLESS_SHUTDOWN_H

#include "runtime_mode_prefix.h"

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

#define ROOTLESS_SHUTDOWN_SESSION_STATE_NAME \
	".darling-rootless-shutdown-session-v1"

enum rootless_shutdown_backend {
	/* No backend is a valid runtime choice. It is the typed fail-closed
	 * verdict when pidfd/subreaper/procfs capabilities are unavailable. */
	ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED = 0,
	/* Writable delegated cgroup v2, retained by directory and membership FDs. */
	ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED = 1,
	/* Persistent controller with retained pidfds and anchored task-children
	 * traversal. Exact traversal is performed only behind a SIGSTOP barrier,
	 * as required by Linux procfs' children-file contract. */
	ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER = 2,
};

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
	unsigned acquisition_timeout_ms;
	size_t pidfd_budget;
	unsigned quiesce_timeout_ms;
	unsigned term_timeout_ms;
	unsigned kill_timeout_ms;
	unsigned poll_interval_ms;
};

struct rootless_shutdown_result {
	enum rootless_shutdown_phase phase;
	enum rootless_shutdown_backend backend;
	pid_t session;
	ino_t closure_inode;
	dev_t capability_device;
	ino_t capability_inode;
	pid_t anchor_pid;
	unsigned long long anchor_start_time;
	size_t identities_observed;
	unsigned quiesce_rounds;
	unsigned term_rounds;
	unsigned kill_rounds;
};

struct rootless_shutdown_closure_capability {
	enum rootless_shutdown_backend backend;
	int prefix_fd;
	int parent_fd;
	int directory_fd;
	int membership_fd;
	int state_fd;
	int proc_fd;
	int anchor_pidfd;
	dev_t cgroup_device;
	ino_t cgroup_inode;
	dev_t state_device;
	ino_t state_inode;
	dev_t proc_device;
	ino_t proc_inode;
	pid_t anchor_pid;
	unsigned long long anchor_start_time;
	char path[PATH_MAX];
	char leaf[NAME_MAX + 1];
};

#define ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER { \
	.backend = ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED, \
	.prefix_fd = -1, .parent_fd = -1, .directory_fd = -1, \
	.membership_fd = -1, .state_fd = -1, .proc_fd = -1, \
	.anchor_pidfd = -1, .path = {0}, .leaf = {0} \
}

const char* rootless_shutdown_backend_name(
	enum rootless_shutdown_backend backend
);

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

/* fork(2)-compatible runtime root creation. The child returns 0. For the
 * PIDFD_SUBREAPER backend the returned child is born below a persistent,
 * identity-bound subreaper controller before any runtime code executes. */
pid_t rootless_shutdown_fork_runtime(
	struct rootless_shutdown_closure_capability* capability,
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

#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
enum rootless_shutdown_test_cgroup_create_phase {
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_CAPABILITY_OPEN = 1,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MODE = 2,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_READABLE_OPEN = 3,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_EVENTS = 4,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MEMBERSHIP = 5,
};

enum rootless_shutdown_test_startup_phase {
	ROOTLESS_SHUTDOWN_TEST_STARTUP_CONTROLLER_READY = 1,
	ROOTLESS_SHUTDOWN_TEST_STARTUP_SESSION_PUBLISHED = 2,
	ROOTLESS_SHUTDOWN_TEST_STARTUP_COMMAND_SENT = 3,
	ROOTLESS_SHUTDOWN_TEST_STARTUP_RUNTIME_REPORTED = 4,
};

void rootless_shutdown_test_set_pidfd_open_checkpoint(
	void (*checkpoint)(pid_t)
);
void rootless_shutdown_test_set_snapshot_replacement(pid_t pid, int enabled);
void rootless_shutdown_test_set_pidfd_preflight_error(int error_number);
void rootless_shutdown_test_set_parent_lookup_error(int error_number);
void rootless_shutdown_test_set_delegated_parent_override(
	int directory_fd,
	const char* path
);
void rootless_shutdown_test_force_backend(
	enum rootless_shutdown_backend backend
);
void rootless_shutdown_test_clear_forced_backend(void);
void rootless_shutdown_test_set_membership_checkpoint(void (*checkpoint)(void));
void rootless_shutdown_test_set_membership_read_checkpoint(
	void (*checkpoint)(size_t)
);
void rootless_shutdown_test_set_snapshot_sorted_checkpoint(
	void (*checkpoint)(unsigned)
);
void rootless_shutdown_test_set_monotonic_clock(
	int (*clock)(unsigned long long*)
);
void rootless_shutdown_test_set_session_publish_checkpoint(
	void (*checkpoint)(void)
);
void rootless_shutdown_test_set_ledger_compact_checkpoint(
	void (*checkpoint)(unsigned, size_t)
);
void rootless_shutdown_test_set_containment_checkpoint(
	void (*checkpoint)(void)
);
void rootless_shutdown_test_set_signal_checkpoint(
	void (*checkpoint)(int, size_t)
);
void rootless_shutdown_test_set_cgroup_create_checkpoint(
	void (*checkpoint)(unsigned, int, const char*)
);
void rootless_shutdown_test_set_cgroup_create_error(
	unsigned phase,
	int error_number
);
void rootless_shutdown_test_set_prebind_event_budget(size_t budget);
void rootless_shutdown_test_set_prebind_event_checkpoint(
	void (*checkpoint)(size_t)
);
void rootless_shutdown_test_set_startup_checkpoint(
	void (*checkpoint)(unsigned)
);
void rootless_shutdown_test_set_startup_error(
	unsigned phase,
	int error_number
);
void rootless_shutdown_test_set_close_range_error(int error_number);
void rootless_shutdown_test_set_proc_barrier_checkpoint(
	void (*checkpoint)(pid_t, int)
);
void rootless_shutdown_test_set_proc_children_checkpoint(
	void (*checkpoint)(pid_t)
);
void rootless_shutdown_test_close_controller_descriptors(void);
#endif

#endif
