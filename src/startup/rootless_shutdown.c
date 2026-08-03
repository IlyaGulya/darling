#include "rootless_shutdown.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/magic.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct process_snapshot {
	pid_t session;
	unsigned long long start_time;
	char state;
};

enum process_barrier_result {
	PROCESS_BARRIER_UNKNOWN,
	PROCESS_BARRIER_STOPPED,
	PROCESS_BARRIER_GONE,
};

enum process_capture_result {
	PROCESS_CAPTURE_GONE,
	PROCESS_CAPTURE_INACTIVE,
	PROCESS_CAPTURE_RETAINED,
};

struct process_identity {
	pid_t pid;
	unsigned long long start_time;
	int pidfd;
	int proc_directory_fd;
	int stopped_by_us;
	int termination_requested;
	enum process_barrier_result barrier;
};

struct process_ledger {
	struct process_identity* identities;
	size_t count;
	size_t capacity;
	size_t observed;
};

struct rootless_shutdown_session_state {
	enum rootless_shutdown_backend backend;
	dev_t prefix_device;
	ino_t prefix_inode;
	uid_t owner_uid;
	dev_t cgroup_device;
	ino_t cgroup_inode;
	char cgroup_path[PATH_MAX];
	dev_t proc_device;
	ino_t proc_inode;
	pid_t anchor_pid;
	unsigned long long anchor_start_time;
};

static int process_snapshot_for_pid(pid_t pid,
	struct process_snapshot* snapshot);
static int process_snapshot_for_pid_at(int proc_fd, pid_t pid,
	struct process_snapshot* snapshot);
static int process_is_active(const struct process_snapshot* snapshot);
static int process_pidfd_active(int pidfd, int* active);
static int inspect_identity_barrier(struct process_identity* identity,
	enum process_barrier_result* result);
static void sleep_milliseconds(unsigned milliseconds);
static int deadline_not_expired(unsigned long long deadline);
static int ledger_capture_member(struct process_ledger* ledger, int proc_fd,
	pid_t pid, size_t pidfd_budget, enum process_capture_result* result);
static struct process_identity* ledger_find_pid(
	struct process_ledger* ledger, pid_t pid);
static int resume_stopped_identities(struct process_ledger* ledger);
static void ledger_release(struct process_ledger* ledger);
static int acquire_subreaper_closure(
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, size_t pidfd_budget,
	unsigned long long deadline, unsigned retry_interval_ms);
static int signal_subreaper_closure(pid_t init_process,
	unsigned long long init_start_time,
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, int signal_number, size_t pidfd_budget,
	unsigned long long deadline, unsigned retry_interval_ms,
	unsigned* active);

#ifndef DARLING_ROOTLESS_SHUTDOWN_TESTING
enum rootless_shutdown_cgroup_create_phase {
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_CAPABILITY_OPEN = 1,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MODE = 2,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_READABLE_OPEN = 3,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_EVENTS = 4,
	ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MEMBERSHIP = 5,
};

enum rootless_shutdown_startup_phase {
	ROOTLESS_SHUTDOWN_TEST_STARTUP_CONTROLLER_READY = 1,
	ROOTLESS_SHUTDOWN_TEST_STARTUP_SESSION_PUBLISHED = 2,
	ROOTLESS_SHUTDOWN_TEST_STARTUP_COMMAND_SENT = 3,
	ROOTLESS_SHUTDOWN_TEST_STARTUP_RUNTIME_REPORTED = 4,
};
#endif

static const struct rootless_shutdown_policy default_policy = {
	.acquisition_timeout_ms = 1000,
	.pidfd_budget = 0,
	.quiesce_timeout_ms = 0,
	.term_timeout_ms = 1000,
	.kill_timeout_ms = 5000,
	.poll_interval_ms = 20,
};

#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
static int pidfd_preflight_error;
static int delegated_parent_lookup_error;
static int delegated_parent_override_fd = -1;
static char delegated_parent_override_path[PATH_MAX];
static pid_t snapshot_replacement_pid = -1;
static int snapshot_replacement_enabled;
static void (*membership_checkpoint)(void);
static void (*membership_read_checkpoint)(size_t);
static void (*snapshot_sorted_checkpoint)(unsigned);
static void (*ledger_compact_checkpoint)(unsigned, size_t);
static void (*containment_checkpoint)(void);
static void (*signal_checkpoint)(int, size_t);
static int (*test_monotonic_clock)(unsigned long long*);
static void (*session_publish_checkpoint)(void);
static void (*cgroup_create_checkpoint)(unsigned, int, const char*);
static unsigned cgroup_create_error_phase;
static int cgroup_create_error_number;
static size_t prebind_event_budget_override;
static void (*prebind_event_checkpoint)(size_t);
static void (*startup_checkpoint)(unsigned);
static unsigned startup_error_phase;
static int startup_error_number;
static int close_range_error;
static void (*proc_barrier_checkpoint)(pid_t, int);
static void (*proc_children_checkpoint)(pid_t);
static enum rootless_shutdown_backend forced_backend;
static int forced_backend_enabled;

void rootless_shutdown_test_set_snapshot_replacement(pid_t pid, int enabled)
{
	snapshot_replacement_pid = pid;
	snapshot_replacement_enabled = enabled;
}

void rootless_shutdown_test_set_pidfd_preflight_error(int error_number)
{
	pidfd_preflight_error = error_number;
}

void rootless_shutdown_test_set_parent_lookup_error(int error_number)
{
	delegated_parent_lookup_error = error_number;
}

void rootless_shutdown_test_set_delegated_parent_override(
	int directory_fd, const char* path)
{
	delegated_parent_override_fd = directory_fd;
	delegated_parent_override_path[0] = '\0';
	if (path != NULL && strlen(path) < sizeof(delegated_parent_override_path))
		strcpy(delegated_parent_override_path, path);
}

void rootless_shutdown_test_force_backend(enum rootless_shutdown_backend backend)
{
	forced_backend = backend;
	forced_backend_enabled = 1;
}

void rootless_shutdown_test_clear_forced_backend(void)
{
	forced_backend = ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED;
	forced_backend_enabled = 0;
}

void rootless_shutdown_test_set_membership_checkpoint(void (*checkpoint)(void))
{
	membership_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_membership_read_checkpoint(
	void (*checkpoint)(size_t))
{
	membership_read_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_snapshot_sorted_checkpoint(
	void (*checkpoint)(unsigned))
{
	snapshot_sorted_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_monotonic_clock(
	int (*clock)(unsigned long long*))
{
	test_monotonic_clock = clock;
}

void rootless_shutdown_test_set_session_publish_checkpoint(
	void (*checkpoint)(void))
{
	session_publish_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_ledger_compact_checkpoint(
	void (*checkpoint)(unsigned, size_t))
{
	ledger_compact_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_containment_checkpoint(
	void (*checkpoint)(void))
{
	containment_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_signal_checkpoint(
	void (*checkpoint)(int, size_t))
{
	signal_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_cgroup_create_checkpoint(
	void (*checkpoint)(unsigned, int, const char*))
{
	cgroup_create_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_cgroup_create_error(
	unsigned phase, int error_number)
{
	cgroup_create_error_phase = phase;
	cgroup_create_error_number = error_number;
}

void rootless_shutdown_test_set_prebind_event_budget(size_t budget)
{
	prebind_event_budget_override = budget;
}

void rootless_shutdown_test_set_prebind_event_checkpoint(
	void (*checkpoint)(size_t))
{
	prebind_event_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_startup_checkpoint(
	void (*checkpoint)(unsigned))
{
	startup_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_startup_error(
	unsigned phase, int error_number)
{
	startup_error_phase = phase;
	startup_error_number = error_number;
}

void rootless_shutdown_test_set_close_range_error(int error_number)
{
	close_range_error = error_number;
}

void rootless_shutdown_test_set_proc_barrier_checkpoint(
	void (*checkpoint)(pid_t, int))
{
	proc_barrier_checkpoint = checkpoint;
}

void rootless_shutdown_test_set_proc_children_checkpoint(
	void (*checkpoint)(pid_t))
{
	proc_children_checkpoint = checkpoint;
}
#endif

const char* rootless_shutdown_backend_name(enum rootless_shutdown_backend backend)
{
	switch (backend) {
	case ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED:
		return "CGROUP_DELEGATED";
	case ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER:
		return "PIDFD_SUBREAPER";
	case ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED:
		return "UNSUPPORTED";
	}
	return "UNSUPPORTED";
}

static int open_process_pidfd(pid_t pid)
{
#ifdef SYS_pidfd_open
	int fd = (int)syscall(SYS_pidfd_open, pid, 0);
	return fd < 0 ? -errno : fd;
#else
	(void)pid;
	return -ENOSYS;
#endif
}

static int signal_process_pidfd(int pidfd, int signal_number)
{
#ifdef SYS_pidfd_send_signal
	if (syscall(SYS_pidfd_send_signal, pidfd, signal_number, NULL, 0) != 0 &&
		errno != ESRCH)
		return -errno;
	return 0;
#else
	(void)pidfd;
	(void)signal_number;
	return -ENOSYS;
#endif
}

static int rootless_shutdown_pidfd_preflight(void)
{
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (pidfd_preflight_error != 0)
		return -pidfd_preflight_error;
#endif
	int pidfd = open_process_pidfd(getpid());
	if (pidfd < 0)
		return pidfd;
	int status = signal_process_pidfd(pidfd, 0);
	close(pidfd);
	return status;
}

static int monotonic_milliseconds(unsigned long long* milliseconds);
static void sleep_milliseconds(unsigned milliseconds);
static int derive_pidfd_budget(
	const struct rootless_shutdown_policy* policy, size_t* budget);

static const char* const host_runtime_endpoints[] = {
	".init.pid",
	".darlingserver.sock",
	".darlingserver.stat.sock",
};

static const char* const guest_runtime_endpoints[] = {
	"var/run/shellspawn.sock",
	"var/tmp/launchd/sock",
};

const char* rootless_shutdown_phase_name(enum rootless_shutdown_phase phase)
{
	switch (phase) {
	case ROOTLESS_SHUTDOWN_RUNNING:
		return "RUNNING";
	case ROOTLESS_SHUTDOWN_CLOSURE_BOUND:
		return "CLOSURE_BOUND";
	case ROOTLESS_SHUTDOWN_QUIESCING:
		return "QUIESCING";
	case ROOTLESS_SHUTDOWN_TERM:
		return "TERM";
	case ROOTLESS_SHUTDOWN_DRAINING:
		return "DRAINING";
	case ROOTLESS_SHUTDOWN_KILL:
		return "KILL";
	case ROOTLESS_SHUTDOWN_DRAINED:
		return "DRAINED";
	case ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED:
		return "ENDPOINTS_REMOVED";
	case ROOTLESS_SHUTDOWN_STOPPED:
		return "STOPPED";
	}
	return "INVALID";
}

static int shutdown_error(char* error, size_t error_size, int code,
	const char* format, ...)
{
	if (error != NULL && error_size != 0) {
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(error, error_size, format, arguments);
		va_end(arguments);
	}
	return -code;
}

static int duplicate_cloexec(int fd)
{
	int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	return duplicate < 0 ? -errno : duplicate;
}

static int read_fd_text(int fd, char* output, size_t output_size)
{
	if (output == NULL || output_size < 2)
		return -EINVAL;
	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;
	ssize_t length = read(fd, output, output_size - 1);
	if (length < 0)
		return -errno;
	if ((size_t)length == output_size - 1)
		return -EOVERFLOW;
	output[length] = '\0';
	return 0;
}

static int cgroup_path_for_pid(pid_t pid, char* output, size_t output_size)
{
	char proc_path[64];
	char content[PATH_MAX + 32];
	if (pid == 0)
		snprintf(proc_path, sizeof(proc_path), "/proc/self/cgroup");
	else
		snprintf(proc_path, sizeof(proc_path), "/proc/%d/cgroup", (int)pid);
	int fd = open(proc_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	int status = read_fd_text(fd, content, sizeof(content));
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	if (status != 0)
		return status;
	char* newline = strchr(content, '\n');
	if (newline != NULL) {
		*newline = '\0';
		if (newline[1] != '\0')
			return -EPROTONOSUPPORT;
	}
	if (strncmp(content, "0::/", 4) != 0)
		return -EPROTONOSUPPORT;
	const char* path = content + 3;
	if (strlen(path) + 1 > output_size)
		return -ENAMETOOLONG;
	strcpy(output, path);
	return 0;
}

static int cgroup_populated_fd(int directory_fd, int* populated)
{
	char content[256];
	int fd = openat(directory_fd, "cgroup.events",
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	int status = read_fd_text(fd, content, sizeof(content));
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	if (status != 0)
		return status;
	char* save = NULL;
	for (char* line = strtok_r(content, "\n", &save);
		line != NULL; line = strtok_r(NULL, "\n", &save)) {
		int value;
		char trailing;
		if (sscanf(line, "populated %d%c", &value, &trailing) == 1 &&
			(value == 0 || value == 1)) {
			*populated = value;
			return 0;
		}
	}
	return -EPROTO;
}

static int cgroup_leaf_for_prefix(const darling_runtime_prefix prefix,
	char* leaf, size_t leaf_size)
{
	struct stat identity;
	if (prefix == NULL || prefix->directory_fd < 0 ||
		fstat(prefix->directory_fd, &identity) != 0)
		return -errno;
	if (!S_ISDIR(identity.st_mode))
		return -ENOTDIR;
	int length = snprintf(leaf, leaf_size, "darling-rootless-%jx-%jx",
		(uintmax_t)identity.st_dev, (uintmax_t)identity.st_ino);
	if (length < 0 || (size_t)length >= leaf_size)
		return -ENAMETOOLONG;
	return 0;
}

static int append_cgroup_component(char* path, size_t path_size,
	const char* component)
{
	size_t used = strlen(path);
	size_t length = strlen(component);
	if (length == 0 || strcmp(component, ".") == 0 ||
		strcmp(component, "..") == 0 || strchr(component, '/') != NULL)
		return -EINVAL;
	if (used + 1 + length + 1 > path_size)
		return -ENAMETOOLONG;
	path[used++] = '/';
	memcpy(path + used, component, length + 1);
	return 0;
}

static int open_delegated_cgroup_parent(int* output_fd,
	char* output_path, size_t output_path_size)
{
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (delegated_parent_lookup_error != 0)
		return -delegated_parent_lookup_error;
	if (delegated_parent_override_fd >= 0) {
		if (delegated_parent_override_path[0] == '\0' ||
			strlen(delegated_parent_override_path) + 1 > output_path_size)
			return -EINVAL;
		int duplicate = duplicate_cloexec(delegated_parent_override_fd);
		if (duplicate < 0)
			return duplicate;
		*output_fd = duplicate;
		strcpy(output_path, delegated_parent_override_path);
		return 0;
	}
#endif
	char path[PATH_MAX];
	int status = cgroup_path_for_pid(0, path, sizeof(path));
	if (status != 0)
		return status;
	int current = open("/sys/fs/cgroup",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (current < 0)
		return -errno;
	int selected = -1;
	char traversed[PATH_MAX] = {0};
	char selected_path[PATH_MAX] = {0};
	char copy[PATH_MAX];
	strcpy(copy, path);
	char* save = NULL;
	for (char* component = strtok_r(copy, "/", &save);
		component != NULL; component = strtok_r(NULL, "/", &save)) {
		status = append_cgroup_component(traversed, sizeof(traversed), component);
		if (status != 0)
			goto out;
		int next = openat(current, component,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0) {
			status = -errno;
			goto out;
		}
		close(current);
		current = next;
		struct stat metadata;
		if (selected < 0 && fstat(current, &metadata) == 0 &&
			metadata.st_uid == geteuid() &&
			(metadata.st_mode & S_IWUSR) != 0 &&
			(metadata.st_mode & S_IXUSR) != 0) {
			selected = duplicate_cloexec(current);
			if (selected < 0) {
				status = selected;
				goto out;
			}
			strcpy(selected_path, traversed);
		}
	}
	if (selected < 0)
		status = -EACCES;
	else {
		if (strlen(selected_path) + 1 > output_path_size) {
			status = -ENAMETOOLONG;
			goto out;
		}
		*output_fd = selected;
		strcpy(output_path, selected_path);
		selected = -1;
		status = 0;
	}
out:
	if (selected >= 0)
		close(selected);
	close(current);
	return status;
}

void rootless_shutdown_release_closure(
	struct rootless_shutdown_closure_capability* capability)
{
	if (capability == NULL)
		return;
	if (capability->state_fd >= 0)
		close(capability->state_fd);
	if (capability->anchor_pidfd >= 0)
		close(capability->anchor_pidfd);
	if (capability->proc_fd >= 0)
		close(capability->proc_fd);
	if (capability->membership_fd >= 0)
		close(capability->membership_fd);
	if (capability->directory_fd >= 0)
		close(capability->directory_fd);
	if (capability->parent_fd >= 0)
		close(capability->parent_fd);
	if (capability->prefix_fd >= 0)
		close(capability->prefix_fd);
	*capability = (struct rootless_shutdown_closure_capability)
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
}

static int write_all_fd(int fd, const char* content)
{
	size_t remaining = strlen(content);
	while (remaining != 0) {
		ssize_t written = write(fd, content, remaining);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return written < 0 ? -errno : -EIO;
		content += written;
		remaining -= (size_t)written;
	}
	return 0;
}

static int format_session_state(
	const struct rootless_shutdown_session_state* state,
	char* content, size_t content_size)
{
	int length = snprintf(content, content_size,
		"DARLING_ROOTLESS_SHUTDOWN_SESSION_V2\n"
		"version=2\n"
		"backend=%s\n"
		"prefix_device=%" PRIuMAX "\n"
		"prefix_inode=%" PRIuMAX "\n"
		"owner_uid=%" PRIuMAX "\n"
		"cgroup_device=%" PRIuMAX "\n"
		"cgroup_inode=%" PRIuMAX "\n"
		"cgroup_path=%s\n"
		"proc_device=%" PRIuMAX "\n"
		"proc_inode=%" PRIuMAX "\n"
		"anchor_pid=%ju\n"
		"anchor_start_time=%ju\n",
		rootless_shutdown_backend_name(state->backend),
		(uintmax_t)state->prefix_device,
		(uintmax_t)state->prefix_inode,
		(uintmax_t)state->owner_uid,
		(uintmax_t)state->cgroup_device,
		(uintmax_t)state->cgroup_inode,
		state->cgroup_path[0] != '\0' ? state->cgroup_path : "-",
		(uintmax_t)state->proc_device,
		(uintmax_t)state->proc_inode,
		(uintmax_t)state->anchor_pid,
		(uintmax_t)state->anchor_start_time);
	return length < 0 || (size_t)length >= content_size
		? -EOVERFLOW : 0;
}

static int parse_uintmax_line(const char* line, const char* key,
	uintmax_t* value)
{
	size_t key_length = strlen(key);
	if (strncmp(line, key, key_length) != 0 || line[key_length] == '\0')
		return -EPROTO;
	char* end = NULL;
	errno = 0;
	uintmax_t parsed = strtoumax(line + key_length, &end, 10);
	if (errno != 0 || end == line + key_length || *end != '\0')
		return -EPROTO;
	*value = parsed;
	return 0;
}

static int validate_cgroup_path(const char* path)
{
	if (path == NULL || path[0] != '/' || path[1] == '\0' ||
		path[strlen(path) - 1] == '/')
		return -EPROTO;
	char copy[PATH_MAX];
	if (strlen(path) >= sizeof(copy))
		return -ENAMETOOLONG;
	strcpy(copy, path);
	char* save = NULL;
	for (char* component = strtok_r(copy, "/", &save);
		component != NULL; component = strtok_r(NULL, "/", &save)) {
		if (component[0] == '\0' || strcmp(component, ".") == 0 ||
			strcmp(component, "..") == 0 || strlen(component) > NAME_MAX)
			return -EPROTO;
	}
	return 0;
}

static int parse_session_state(char* content,
	struct rootless_shutdown_session_state* state)
{
	char* lines[13] = {0};
	size_t count = 0;
	char* cursor = content;
	while (*cursor != '\0') {
		if (count == sizeof(lines) / sizeof(lines[0]))
			return -EPROTO;
		lines[count++] = cursor;
		char* newline = strchr(cursor, '\n');
		if (newline == NULL)
			return -EPROTO;
		*newline = '\0';
		cursor = newline + 1;
	}
	if (count == 8 &&
		strcmp(lines[0], "DARLING_ROOTLESS_SHUTDOWN_SESSION_V1") == 0 &&
		strcmp(lines[1], "version=1") == 0) {
		uintmax_t prefix_device, prefix_inode, owner_uid;
		uintmax_t cgroup_device, cgroup_inode;
		if (parse_uintmax_line(lines[2], "prefix_device=", &prefix_device) != 0 ||
			parse_uintmax_line(lines[3], "prefix_inode=", &prefix_inode) != 0 ||
			parse_uintmax_line(lines[4], "owner_uid=", &owner_uid) != 0 ||
			parse_uintmax_line(lines[5], "cgroup_device=", &cgroup_device) != 0 ||
			parse_uintmax_line(lines[6], "cgroup_inode=", &cgroup_inode) != 0 ||
			strncmp(lines[7], "cgroup_path=", 12) != 0 || lines[7][12] == '\0' ||
			prefix_device != (uintmax_t)(dev_t)prefix_device ||
			prefix_inode != (uintmax_t)(ino_t)prefix_inode ||
			owner_uid != (uintmax_t)(uid_t)owner_uid ||
			cgroup_device != (uintmax_t)(dev_t)cgroup_device ||
			cgroup_inode != (uintmax_t)(ino_t)cgroup_inode ||
			strlen(lines[7] + 12) >= sizeof(state->cgroup_path) ||
			validate_cgroup_path(lines[7] + 12) != 0)
			return -EPROTO;
		*state = (struct rootless_shutdown_session_state) {
			.backend = ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED,
			.prefix_device = (dev_t)prefix_device,
			.prefix_inode = (ino_t)prefix_inode,
			.owner_uid = (uid_t)owner_uid,
			.cgroup_device = (dev_t)cgroup_device,
			.cgroup_inode = (ino_t)cgroup_inode,
		};
		strcpy(state->cgroup_path, lines[7] + 12);
		return 0;
	}
	if (count != 13 ||
		strcmp(lines[0], "DARLING_ROOTLESS_SHUTDOWN_SESSION_V2") != 0 ||
		strcmp(lines[1], "version=2") != 0 ||
		strncmp(lines[2], "backend=", 8) != 0)
		return -EPROTO;
	enum rootless_shutdown_backend backend;
	if (strcmp(lines[2] + 8, "CGROUP_DELEGATED") == 0)
		backend = ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED;
	else if (strcmp(lines[2] + 8, "PIDFD_SUBREAPER") == 0)
		backend = ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER;
	else
		return -EPROTO;
	uintmax_t prefix_device;
	uintmax_t prefix_inode;
	uintmax_t owner_uid;
	uintmax_t cgroup_device;
	uintmax_t cgroup_inode;
	uintmax_t proc_device;
	uintmax_t proc_inode;
	uintmax_t anchor_pid;
	uintmax_t anchor_start_time;
	if (parse_uintmax_line(lines[3], "prefix_device=", &prefix_device) != 0 ||
		parse_uintmax_line(lines[4], "prefix_inode=", &prefix_inode) != 0 ||
		parse_uintmax_line(lines[5], "owner_uid=", &owner_uid) != 0 ||
		parse_uintmax_line(lines[6], "cgroup_device=", &cgroup_device) != 0 ||
		parse_uintmax_line(lines[7], "cgroup_inode=", &cgroup_inode) != 0 ||
		strncmp(lines[8], "cgroup_path=", 12) != 0 ||
		parse_uintmax_line(lines[9], "proc_device=", &proc_device) != 0 ||
		parse_uintmax_line(lines[10], "proc_inode=", &proc_inode) != 0 ||
		parse_uintmax_line(lines[11], "anchor_pid=", &anchor_pid) != 0 ||
		parse_uintmax_line(lines[12], "anchor_start_time=", &anchor_start_time) != 0 ||
		prefix_device != (uintmax_t)(dev_t)prefix_device ||
		prefix_inode != (uintmax_t)(ino_t)prefix_inode ||
		owner_uid != (uintmax_t)(uid_t)owner_uid ||
		cgroup_device != (uintmax_t)(dev_t)cgroup_device ||
		cgroup_inode != (uintmax_t)(ino_t)cgroup_inode ||
		proc_device != (uintmax_t)(dev_t)proc_device ||
		proc_inode != (uintmax_t)(ino_t)proc_inode ||
		anchor_pid != (uintmax_t)(pid_t)anchor_pid ||
		strlen(lines[8] + 12) >= sizeof(state->cgroup_path))
		return -EPROTO;
	*state = (struct rootless_shutdown_session_state) {
		.backend = backend,
		.prefix_device = (dev_t)prefix_device,
		.prefix_inode = (ino_t)prefix_inode,
		.owner_uid = (uid_t)owner_uid,
		.cgroup_device = (dev_t)cgroup_device,
		.cgroup_inode = (ino_t)cgroup_inode,
		.proc_device = (dev_t)proc_device,
		.proc_inode = (ino_t)proc_inode,
		.anchor_pid = (pid_t)anchor_pid,
		.anchor_start_time = anchor_start_time,
	};
	if (strcmp(lines[8] + 12, "-") != 0)
		strcpy(state->cgroup_path, lines[8] + 12);
	if ((backend == ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED &&
		(state->cgroup_path[0] == '\0' ||
		 validate_cgroup_path(state->cgroup_path) != 0 ||
		 state->cgroup_device == 0 || state->cgroup_inode == 0)) ||
		(backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER &&
		(state->proc_device == 0 || state->proc_inode == 0 ||
		 state->anchor_pid <= 0 || state->anchor_start_time == 0 ||
		 state->cgroup_path[0] != '\0')))
		return -EPROTO;
	return 0;
}

static int session_state_named_identity(
	const struct rootless_shutdown_closure_capability* capability)
{
	if (capability == NULL || capability->prefix_fd < 0 ||
		capability->state_fd < 0)
		return -EINVAL;
	struct stat prefix;
	struct stat opened;
	struct stat named;
	if (fstat(capability->prefix_fd, &prefix) != 0 ||
		fstat(capability->state_fd, &opened) != 0 ||
		fstatat(capability->prefix_fd, ROOTLESS_SHUTDOWN_SESSION_STATE_NAME,
			&named, AT_SYMLINK_NOFOLLOW) != 0)
		return -errno;
	if (!S_ISDIR(prefix.st_mode) || !S_ISREG(opened.st_mode) ||
		!S_ISREG(named.st_mode) || opened.st_dev != named.st_dev ||
		opened.st_ino != named.st_ino || opened.st_nlink != 1 ||
		named.st_nlink != 1 || (opened.st_mode & 0777) != 0600 ||
		(named.st_mode & 0777) != 0600 || opened.st_uid != prefix.st_uid ||
		capability->state_device != opened.st_dev ||
		capability->state_inode != opened.st_ino)
		return -ESTALE;
	return 0;
}

static int publish_session_state(
	const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	const struct rootless_shutdown_session_state* state)
{
	char content[PATH_MAX + 512];
	int status = format_session_state(state, content, sizeof(content));
	if (status != 0)
		return status;
	char temporary[NAME_MAX + 1];
	int length = snprintf(temporary, sizeof(temporary), ".%s.tmp.%ld",
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, (long)getpid());
	if (length < 0 || (size_t)length >= sizeof(temporary))
		return -ENAMETOOLONG;
	int fd = openat(prefix->directory_fd, temporary,
		O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	status = fchmod(fd, 0600) == 0 ? write_all_fd(fd, content) : -errno;
	if (status == 0 && fsync(fd) != 0)
		status = -errno;
	struct stat written = {0};
	struct stat prefix_identity;
	if (status == 0 && (fstat(fd, &written) != 0 ||
		fstat(prefix->directory_fd, &prefix_identity) != 0))
		status = -errno;
	if (status == 0 && (!S_ISREG(written.st_mode) || written.st_nlink != 1 ||
		(written.st_mode & 0777) != 0600 ||
		written.st_uid != prefix_identity.st_uid))
		status = -EPERM;
	int published = 0;
	if (status == 0 && renameat2(prefix->directory_fd, temporary,
			prefix->directory_fd, ROOTLESS_SHUTDOWN_SESSION_STATE_NAME,
			RENAME_NOREPLACE) != 0)
		status = -errno;
	else if (status == 0)
		published = 1;

#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (status == 0 && session_publish_checkpoint != NULL)
		session_publish_checkpoint();
#endif
	if (status == 0) {
		capability->state_fd = fd;
		fd = -1;
	}
	struct stat state_identity;
	if (status == 0 && fstat(capability->state_fd, &state_identity) != 0)
		status = -errno;
	if (status == 0) {
		capability->state_device = state_identity.st_dev;
		capability->state_inode = state_identity.st_ino;
		status = session_state_named_identity(capability);
	}
	if (status == 0 && fsync(prefix->directory_fd) != 0)
		status = -errno;
	if (status == 0)
		return 0;
	(void)unlinkat(prefix->directory_fd, temporary, 0);
	if (published) {
		struct stat named;
		if (fstatat(prefix->directory_fd,
				ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, &named,
				AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(named.st_mode) &&
			named.st_dev == written.st_dev && named.st_ino == written.st_ino)
			(void)unlinkat(prefix->directory_fd,
				ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, 0);
		(void)fsync(prefix->directory_fd);
	}
	if (capability->state_fd >= 0) {
		close(capability->state_fd);
		capability->state_fd = -1;
	}
	capability->state_device = 0;
	capability->state_inode = 0;
	if (fd >= 0)
		close(fd);
	return status;
}

static int load_session_state(const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	struct rootless_shutdown_session_state* state)
{
	capability->prefix_fd = duplicate_cloexec(prefix->directory_fd);
	if (capability->prefix_fd < 0)
		return capability->prefix_fd;
	capability->state_fd = openat(capability->prefix_fd,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (capability->state_fd < 0)
		return -errno;
	struct stat prefix_identity;
	struct stat state_identity;
	if (fstat(capability->prefix_fd, &prefix_identity) != 0 ||
		fstat(capability->state_fd, &state_identity) != 0)
		return -errno;
	capability->state_device = state_identity.st_dev;
	capability->state_inode = state_identity.st_ino;
	if (!S_ISREG(state_identity.st_mode) || state_identity.st_nlink != 1 ||
		(state_identity.st_mode & 0777) != 0600 ||
		state_identity.st_uid != prefix_identity.st_uid ||
		state_identity.st_size <= 0 || state_identity.st_size >= PATH_MAX + 512)
		return -EPERM;
	char content[PATH_MAX + 512];
	int status = read_fd_text(capability->state_fd, content, sizeof(content));
	if (status != 0)
		return status;
	status = parse_session_state(content, state);
	if (status != 0)
		return status;
	if (state->prefix_device != prefix_identity.st_dev ||
		state->prefix_inode != prefix_identity.st_ino ||
		state->owner_uid != prefix_identity.st_uid)
		return -ESTALE;
	return session_state_named_identity(capability);
}

static int closure_named_identity(
	const struct rootless_shutdown_closure_capability* capability)
{
	if (capability == NULL)
		return -EINVAL;
	if (capability->backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER) {
		if (capability->proc_fd < 0 || capability->anchor_pidfd < 0 ||
			capability->anchor_pid <= 0 || capability->anchor_start_time == 0)
			return -EINVAL;
		struct stat proc;
		if (fstat(capability->proc_fd, &proc) != 0)
			return -errno;
		if (!S_ISDIR(proc.st_mode) || proc.st_dev != capability->proc_device ||
			proc.st_ino != capability->proc_inode)
			return -ESTALE;
		if (capability->state_fd >= 0)
			return session_state_named_identity(capability);
		return 0;
	}
	if (capability->backend != ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED ||
		capability->parent_fd < 0 || capability->directory_fd < 0 ||
		capability->leaf[0] == '\0')
		return -EINVAL;
	struct stat opened;
	struct stat named;
	if (fstat(capability->directory_fd, &opened) != 0 ||
		fstatat(capability->parent_fd, capability->leaf,
			&named, AT_SYMLINK_NOFOLLOW) != 0)
		return -errno;
	if (!S_ISDIR(opened.st_mode) || !S_ISDIR(named.st_mode) ||
		opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
		opened.st_uid != geteuid() ||
		(capability->cgroup_device != 0 &&
		 opened.st_dev != capability->cgroup_device) ||
		(capability->cgroup_inode != 0 &&
		 opened.st_ino != capability->cgroup_inode))
		return -ESTALE;
	if (capability->state_fd >= 0) {
		int status = session_state_named_identity(capability);
		if (status != 0)
			return status;
	}
	return 0;
}

struct cgroup_prebind_watch {
	int fd;
	int watch;
};

#define CGROUP_PREBIND_WATCH_INITIALIZER { .fd = -1, .watch = -1 }
#define CGROUP_PREBIND_EVENT_BUDGET 4096U
#define CGROUP_PREBIND_DRAIN_TIMEOUT_MS 250U

static int monotonic_milliseconds(unsigned long long* milliseconds);

static void cgroup_prebind_watch_release(struct cgroup_prebind_watch* watch)
{
	if (watch->watch >= 0 && watch->fd >= 0)
		(void)inotify_rm_watch(watch->fd, watch->watch);
	if (watch->fd >= 0)
		close(watch->fd);
	*watch = (struct cgroup_prebind_watch)
		CGROUP_PREBIND_WATCH_INITIALIZER;
}

static int cgroup_prebind_watch_arm(int parent_fd,
	struct cgroup_prebind_watch* watch)
{
	char proc_path[64];
	int length = snprintf(proc_path, sizeof(proc_path),
		"/proc/self/fd/%d", parent_fd);
	if (length < 0 || (size_t)length >= sizeof(proc_path))
		return -EOVERFLOW;
	watch->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (watch->fd < 0)
		return -errno;
	watch->watch = inotify_add_watch(watch->fd, proc_path,
		IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
		IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
	if (watch->watch < 0) {
		int status = -errno;
		cgroup_prebind_watch_release(watch);
		return status;
	}
	return 0;
}

static int cgroup_prebind_watch_finish(struct cgroup_prebind_watch* watch,
	const char* leaf)
{
	char buffer[4096]
		__attribute__((aligned(__alignof__(struct inotify_event))));
	unsigned creates = 0;
	int conflict = 0;
	int status = 0;
	size_t event_count = 0;
	size_t event_budget = CGROUP_PREBIND_EVENT_BUDGET;
	int saw_ignored = 0;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (prebind_event_budget_override != 0)
		event_budget = prebind_event_budget_override;
#endif
	unsigned long long now = 0;
	status = monotonic_milliseconds(&now);
	if (status != 0)
		goto out;
	unsigned long long deadline = now + CGROUP_PREBIND_DRAIN_TIMEOUT_MS;
	int sealed_watch = watch->watch;
	if (inotify_rm_watch(watch->fd, sealed_watch) != 0) {
		status = -errno;
		goto out;
	}
	watch->watch = -1;
	for (;;) {
		status = monotonic_milliseconds(&now);
		if (status != 0)
			break;
		if (now >= deadline) {
			status = -ETIMEDOUT;
			break;
		}
		ssize_t length = read(watch->fd, buffer, sizeof(buffer));
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (length < 0) {
			status = -errno;
			break;
		}
		if (length == 0) {
			status = -EIO;
			break;
		}
		for (char* cursor = buffer; cursor < buffer + length;) {
			struct inotify_event* event = (struct inotify_event*)cursor;
			event_count++;
			if (event_count > event_budget) {
				status = -EOVERFLOW;
				break;
			}
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
			if (prebind_event_checkpoint != NULL)
				prebind_event_checkpoint(event_count);
#endif
			status = monotonic_milliseconds(&now);
			if (status != 0 || now >= deadline) {
				if (status == 0)
					status = -ETIMEDOUT;
				break;
			}
			if ((event->mask & IN_Q_OVERFLOW) != 0)
				conflict = 1;
			if (event->wd == sealed_watch &&
				(event->mask & IN_IGNORED) != 0)
				saw_ignored = 1;
			if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
				conflict = 1;
			if (event->wd == sealed_watch && event->len != 0 &&
				strcmp(event->name, leaf) == 0) {
				if ((event->mask & (IN_CREATE | IN_ISDIR)) ==
					(IN_CREATE | IN_ISDIR) &&
					(event->mask & (IN_DELETE | IN_MOVED_FROM |
					 IN_MOVED_TO)) == 0)
					creates++;
				else
					conflict = 1;
			}
			cursor += sizeof(*event) + event->len;
		}
		if (status != 0)
			break;
	}
out:
	cgroup_prebind_watch_release(watch);
	if (status != 0)
		return status;
	return !conflict && saw_ignored && creates == 1 ? 0 : -ESTALE;
}

static int named_directory_matches_fd(int parent_fd, const char* leaf,
	int directory_fd, const struct stat* expected)
{
	struct stat opened;
	struct stat named;
	if (parent_fd < 0 || leaf == NULL || leaf[0] == '\0' ||
		directory_fd < 0)
		return -EINVAL;
	if (fstat(directory_fd, &opened) != 0 ||
		fstatat(parent_fd, leaf, &named, AT_SYMLINK_NOFOLLOW) != 0)
		return -errno;
	if (!S_ISDIR(opened.st_mode) || !S_ISDIR(named.st_mode) ||
		opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
		(expected != NULL && (opened.st_dev != expected->st_dev ||
		 opened.st_ino != expected->st_ino)))
		return -ESTALE;
	return 0;
}

static int chmod_directory_capability(int directory_fd, mode_t mode)
{
#ifdef SYS_fchmodat2
	if (syscall(SYS_fchmodat2, directory_fd, "", mode, AT_EMPTY_PATH) == 0)
		return 0;
	if (errno != ENOSYS)
		return -errno;
#endif
	char proc_path[64];
	int length = snprintf(proc_path, sizeof(proc_path),
		"/proc/self/fd/%d", directory_fd);
	if (length < 0 || (size_t)length >= sizeof(proc_path))
		return -EOVERFLOW;
	return chmod(proc_path, mode) == 0 ? 0 : -errno;
}

static int cgroup_create_checkpoint_status(unsigned phase, int parent_fd,
	const char* leaf)
{
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (cgroup_create_checkpoint != NULL)
		cgroup_create_checkpoint(phase, parent_fd, leaf);
	if (cgroup_create_error_phase == phase &&
		cgroup_create_error_number != 0)
		return -cgroup_create_error_number;
#else
	(void)phase;
	(void)parent_fd;
	(void)leaf;
#endif
	return 0;
}

static int remove_created_cgroup_if_owned(int parent_fd, const char* leaf,
	int directory_fd, const struct stat* expected)
{
	int status = named_directory_matches_fd(parent_fd, leaf,
		directory_fd, expected);
	if (status != 0)
		return status;
	if (unlinkat(parent_fd, leaf, AT_REMOVEDIR) != 0)
		return -errno;
	return 0;
}

static int remove_owned_session_state(
	struct rootless_shutdown_closure_capability* capability)
{
	if (capability->prefix_fd < 0 || capability->state_fd < 0)
		return -EINVAL;
	int status = session_state_named_identity(capability);
	if (status != 0)
		return status;
	if (unlinkat(capability->prefix_fd,
			ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, 0) != 0)
		return -errno;
	if (fsync(capability->prefix_fd) != 0)
		return -errno;
	close(capability->state_fd);
	capability->state_fd = -1;
	capability->state_device = 0;
	capability->state_inode = 0;
	return 0;
}

static int open_recorded_cgroup(
	const struct rootless_shutdown_session_state* state,
	struct rootless_shutdown_closure_capability* capability)
{
	int status = validate_cgroup_path(state->cgroup_path);
	if (status != 0)
		return status;
	char copy[PATH_MAX];
	strcpy(copy, state->cgroup_path);
	int current = open("/sys/fs/cgroup",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (current < 0)
		return -errno;
	char* save = NULL;
	char* component = strtok_r(copy, "/", &save);
	if (component == NULL) {
		close(current);
		return -EPROTO;
	}
	for (;;) {
		char* next_component = strtok_r(NULL, "/", &save);
		if (next_component == NULL) {
			if (strlen(component) >= sizeof(capability->leaf)) {
				status = -ENAMETOOLONG;
				break;
			}
			capability->parent_fd = current;
			current = -1;
			strcpy(capability->leaf, component);
			capability->directory_fd = openat(capability->parent_fd,
				component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (capability->directory_fd < 0)
				status = -errno;
			break;
		}
		int next = openat(current, component,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0) {
			status = -errno;
			break;
		}
		close(current);
		current = next;
		component = next_component;
	}
	if (current >= 0)
		close(current);
	if (status != 0)
		return status;
	capability->cgroup_device = state->cgroup_device;
	capability->cgroup_inode = state->cgroup_inode;
	strcpy(capability->path, state->cgroup_path);
	int identity_status = closure_named_identity(capability);
	if (identity_status != 0)
		return identity_status;
	capability->proc_fd = open("/proc",
		O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	return capability->proc_fd < 0 ? -errno : 0;
}

static int open_recorded_subreaper(
	const struct rootless_shutdown_session_state* state,
	struct rootless_shutdown_closure_capability* capability)
{
	capability->proc_fd = open("/proc",
		O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (capability->proc_fd < 0)
		return -errno;
	struct statfs filesystem;
	struct stat proc;
	if (fstatfs(capability->proc_fd, &filesystem) != 0 ||
		fstat(capability->proc_fd, &proc) != 0)
		return -errno;
	if ((unsigned long)filesystem.f_type != (unsigned long)PROC_SUPER_MAGIC ||
		!S_ISDIR(proc.st_mode) || proc.st_dev != state->proc_device ||
		proc.st_ino != state->proc_inode)
		return -ESTALE;
	struct process_snapshot snapshot;
	int status = process_snapshot_for_pid_at(capability->proc_fd,
		state->anchor_pid, &snapshot);
	if (status != 0)
		return status;
	capability->anchor_pidfd = open_process_pidfd(state->anchor_pid);
	if (capability->anchor_pidfd < 0)
		return capability->anchor_pidfd;
	struct process_snapshot verified;
	status = process_snapshot_for_pid_at(capability->proc_fd,
		state->anchor_pid, &verified);
	if (status != 0 || snapshot.start_time != state->anchor_start_time ||
		verified.start_time != state->anchor_start_time ||
		!process_is_active(&verified))
		return status != 0 ? status : -ESTALE;
	capability->backend = ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER;
	capability->proc_device = state->proc_device;
	capability->proc_inode = state->proc_inode;
	capability->anchor_pid = state->anchor_pid;
	capability->anchor_start_time = state->anchor_start_time;
	return closure_named_identity(capability);
}

static int prepare_subreaper_capability(
	struct rootless_shutdown_closure_capability* capability)
{
	int subreaper = 0;
	if (prctl(PR_GET_CHILD_SUBREAPER, &subreaper) != 0)
		return -errno;
	pid_t probe = fork();
	if (probe < 0)
		return -errno;
	if (probe == 0)
		_exit(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0 ? 0 : 125);
	int probe_status = 0;
	while (waitpid(probe, &probe_status, 0) < 0) {
		if (errno != EINTR)
			return -errno;
	}
	if (!WIFEXITED(probe_status) || WEXITSTATUS(probe_status) != 0)
		return -EOPNOTSUPP;
	if (capability->proc_fd < 0)
		capability->proc_fd = open("/proc",
			O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (capability->proc_fd < 0)
		return -errno;
	struct statfs filesystem;
	struct stat identity;
	if (fstatfs(capability->proc_fd, &filesystem) != 0 ||
		fstat(capability->proc_fd, &identity) != 0)
		return -errno;
	if ((unsigned long)filesystem.f_type != (unsigned long)PROC_SUPER_MAGIC ||
		!S_ISDIR(identity.st_mode))
		return -EPROTONOSUPPORT;
	capability->backend = ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER;
	capability->proc_device = identity.st_dev;
	capability->proc_inode = identity.st_ino;
	return 0;
}

static int cgroup_fallback_error(int status)
{
	return status == -EACCES || status == -EPERM || status == -EROFS ||
		status == -ENOENT || status == -ENOSYS ||
		status == -EPROTONOSUPPORT;
}

static int cgroup_backend_is_forced(void)
{
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	return forced_backend_enabled &&
		forced_backend == ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED;
#else
	return 0;
#endif
}

int rootless_shutdown_prepare_closure(const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability == NULL || prefix == NULL || prefix->directory_fd < 0 ||
		capability->prefix_fd >= 0 || capability->parent_fd >= 0 ||
		capability->directory_fd >= 0 || capability->membership_fd >= 0 ||
		capability->state_fd >= 0 || capability->proc_fd >= 0 ||
		capability->anchor_pidfd >= 0)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure capability is already owned");
	struct rootless_shutdown_closure_capability local =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	int created = 0;
	int created_path_fd = -1;
	int prebind_reserve_fd = -1;
	struct stat created_identity = {0};
	int created_identity_valid = 0;
	struct cgroup_prebind_watch prebind_watch =
		CGROUP_PREBIND_WATCH_INITIALIZER;
	int state_published = 0;
	int cgroup_capability_complete = 0;
	int status = rootless_shutdown_pidfd_preflight();
	if (status != 0)
		goto fail;
	local.prefix_fd = duplicate_cloexec(prefix->directory_fd);
	if (local.prefix_fd < 0) {
		status = local.prefix_fd;
		goto fail;
	}
	local.proc_fd = open("/proc",
		O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (local.proc_fd < 0) {
		status = -errno;
		goto fail;
	}
	#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (forced_backend_enabled &&
		forced_backend == ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED) {
		status = -EOPNOTSUPP;
		goto fail;
	}
	if (forced_backend_enabled &&
		forced_backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER) {
		status = prepare_subreaper_capability(&local);
		if (status != 0)
			goto fail;
		*capability = local;
		if (error != NULL && error_size != 0)
			error[0] = '\0';
		return 0;
	}
	#endif
	status = cgroup_leaf_for_prefix(prefix, local.leaf, sizeof(local.leaf));
	if (status != 0)
		goto fail;
	char parent_path[PATH_MAX];
	status = open_delegated_cgroup_parent(&local.parent_fd,
		parent_path, sizeof(parent_path));
	if (status != 0 && cgroup_fallback_error(status) &&
		!cgroup_backend_is_forced()) {
		status = prepare_subreaper_capability(&local);
		if (status != 0)
			goto fail;
		*capability = local;
		if (error != NULL && error_size != 0)
			error[0] = '\0';
		return 0;
	}
	if (status != 0)
		goto fail;
	local.backend = ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED;
	/* Reserve one descriptor before mutation. If the first O_PATH acquisition
	 * hits the descriptor limit, releasing this reserve makes the recovery
	 * acquisition deterministic instead of leaving an unbound directory. */
	prebind_reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (prebind_reserve_fd < 0) {
		status = -errno;
		goto fail;
	}
	status = cgroup_prebind_watch_arm(local.parent_fd, &prebind_watch);
	if (status != 0)
		goto fail;
	if (mkdirat(local.parent_fd, local.leaf, 0700) != 0) {
		status = -errno;
		if (cgroup_fallback_error(status) &&
			!cgroup_backend_is_forced()) {
			cgroup_prebind_watch_release(&prebind_watch);
			close(prebind_reserve_fd);
			prebind_reserve_fd = -1;
			close(local.parent_fd);
			local.parent_fd = -1;
			local.leaf[0] = '\0';
			status = prepare_subreaper_capability(&local);
			if (status != 0)
				goto fail;
			*capability = local;
			if (error != NULL && error_size != 0)
				error[0] = '\0';
			return 0;
		}
		goto fail;
	}
	created = 1;
	int prebind_status = cgroup_create_checkpoint_status(
		ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_CAPABILITY_OPEN,
		local.parent_fd, local.leaf);
	if (prebind_status == 0) {
		created_path_fd = openat(local.parent_fd, local.leaf,
			O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (created_path_fd < 0)
			prebind_status = -errno;
	}
	/* A failed primary acquisition still gets one recovery acquisition while
	 * the parent event watch is authoritative. This allows exact rollback of
	 * the directory created by mkdirat without ever deleting a replacement. */
	close(prebind_reserve_fd);
	prebind_reserve_fd = -1;
	if (created_path_fd < 0)
		created_path_fd = openat(local.parent_fd, local.leaf,
			O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	int watch_status = cgroup_prebind_watch_finish(
		&prebind_watch, local.leaf);
	if (watch_status != 0) {
		status = watch_status;
		goto fail;
	}
	if (created_path_fd < 0) {
		status = prebind_status != 0 ? prebind_status : -errno;
		goto fail;
	}
	if (fstat(created_path_fd, &created_identity) != 0) {
		status = -errno;
		goto fail;
	}
	if (!S_ISDIR(created_identity.st_mode) ||
		created_identity.st_uid != geteuid()) {
		status = -EPERM;
		goto fail;
	}
	created_identity_valid = 1;
	status = named_directory_matches_fd(local.parent_fd, local.leaf,
		created_path_fd, &created_identity);
	if (status != 0)
		goto fail;
	if (prebind_status != 0) {
		status = prebind_status;
		goto fail;
	}
	status = cgroup_create_checkpoint_status(
		ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MODE,
		local.parent_fd, local.leaf);
	if (status != 0)
		goto fail;
	status = chmod_directory_capability(created_path_fd, 0700);
	if (status != 0)
		goto fail;
	status = named_directory_matches_fd(local.parent_fd, local.leaf,
		created_path_fd, &created_identity);
	if (status != 0)
		goto fail;
	struct stat mode_identity;
	if (fstat(created_path_fd, &mode_identity) != 0) {
		status = -errno;
		goto fail;
	}
	if ((mode_identity.st_mode & 0777) != 0700 ||
		mode_identity.st_uid != geteuid()) {
		status = -EPERM;
		goto fail;
	}
	int length = snprintf(local.path, sizeof(local.path), "%s/%s",
		parent_path, local.leaf);
	if (length < 0 || (size_t)length >= sizeof(local.path)) {
		status = -ENAMETOOLONG;
		goto fail;
	}
	status = cgroup_create_checkpoint_status(
		ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_READABLE_OPEN,
		local.parent_fd, local.leaf);
	if (status != 0)
		goto fail;
	status = named_directory_matches_fd(local.parent_fd, local.leaf,
		created_path_fd, &created_identity);
	if (status != 0)
		goto fail;
	local.directory_fd = openat(local.parent_fd, local.leaf,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (local.directory_fd < 0) {
		status = -errno;
		goto fail;
	}
	status = cgroup_create_checkpoint_status(
		ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_EVENTS,
		local.parent_fd, local.leaf);
	if (status != 0)
		goto fail;
	int populated = 0;
	status = cgroup_populated_fd(local.directory_fd, &populated);
	if (status != 0)
		goto fail;
	if (populated) {
		status = -EBUSY;
		goto fail;
	}
	struct stat prefix_identity;
	struct stat cgroup_identity;
	if (fstat(local.prefix_fd, &prefix_identity) != 0 ||
		fstat(local.directory_fd, &cgroup_identity) != 0) {
		status = -errno;
		goto fail;
	}
	status = named_directory_matches_fd(local.parent_fd, local.leaf,
		local.directory_fd, &created_identity);
	if (status != 0 || cgroup_identity.st_dev != mode_identity.st_dev ||
		cgroup_identity.st_ino != mode_identity.st_ino) {
		if (status == 0)
			status = -ESTALE;
		goto fail;
	}
	if ((cgroup_identity.st_mode & 0777) != 0700 ||
		cgroup_identity.st_uid != geteuid()) {
		status = -EPERM;
		goto fail;
	}
	local.cgroup_device = cgroup_identity.st_dev;
	local.cgroup_inode = cgroup_identity.st_ino;
	status = cgroup_create_checkpoint_status(
		ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MEMBERSHIP,
		local.parent_fd, local.leaf);
	if (status != 0)
		goto fail;
	local.membership_fd = openat(local.directory_fd, "cgroup.procs",
		O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (local.membership_fd < 0) {
		status = -errno;
		goto fail;
	}
	status = closure_named_identity(&local);
	if (status != 0)
		goto fail;
	cgroup_capability_complete = 1;
	struct rootless_shutdown_session_state state = {
		.backend = ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED,
		.prefix_device = prefix_identity.st_dev,
		.prefix_inode = prefix_identity.st_ino,
		.owner_uid = prefix_identity.st_uid,
		.cgroup_device = cgroup_identity.st_dev,
		.cgroup_inode = cgroup_identity.st_ino,
	};
	strcpy(state.cgroup_path, local.path);
	status = publish_session_state(prefix, &local, &state);
	if (status != 0)
		goto fail;
	state_published = 1;
	close(created_path_fd);
	created_path_fd = -1;
	*capability = local;
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
fail:
	{
		int original_status = status;
		int fallback = created && !state_published &&
			!cgroup_capability_complete &&
			cgroup_fallback_error(original_status) &&
			!cgroup_backend_is_forced();
		int cleanup_status = 0;
	cgroup_prebind_watch_release(&prebind_watch);
	if (prebind_reserve_fd >= 0)
		close(prebind_reserve_fd);
	if (state_published)
		(void)remove_owned_session_state(&local);
	if (created && local.parent_fd >= 0) {
		int cleanup_fd = local.directory_fd >= 0
			? local.directory_fd : created_path_fd;
		if (cleanup_fd >= 0 && created_identity_valid &&
			named_directory_matches_fd(local.parent_fd, local.leaf,
				cleanup_fd, &created_identity) == 0) {
			cleanup_status = remove_created_cgroup_if_owned(local.parent_fd,
				local.leaf, cleanup_fd, &created_identity);
		} else
			cleanup_status = -ESTALE;
	}
	if (created_path_fd >= 0)
		close(created_path_fd);
	rootless_shutdown_release_closure(&local);
	if (fallback && cleanup_status == 0) {
		local = (struct rootless_shutdown_closure_capability)
			ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
		local.prefix_fd = duplicate_cloexec(prefix->directory_fd);
		if (local.prefix_fd >= 0)
			status = prepare_subreaper_capability(&local);
		else
			status = local.prefix_fd;
		if (status == 0) {
			*capability = local;
			if (error != NULL && error_size != 0)
				error[0] = '\0';
			return 0;
		}
		rootless_shutdown_release_closure(&local);
	} else if (fallback && cleanup_status != 0)
		status = cleanup_status;
	else
		status = original_status;
	return shutdown_error(error, error_size, -status,
		"cannot prepare rootless shutdown backend: %s", strerror(-status));
	}
}

struct subreaper_start_message {
	int status;
	pid_t runtime_pid;
};

static int read_exact(int fd, void* output, size_t size)
{
	char* cursor = output;
	while (size != 0) {
		ssize_t received = read(fd, cursor, size);
		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return received < 0 ? -errno : -EPIPE;
		cursor += received;
		size -= (size_t)received;
	}
	return 0;
}

static int write_exact(int fd, const void* input, size_t size)
{
	const char* cursor = input;
	while (size != 0) {
		ssize_t written = write(fd, cursor, size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return written < 0 ? -errno : -EPIPE;
		cursor += written;
		size -= (size_t)written;
	}
	return 0;
}

static void controller_close_descriptors(void)
{
#ifdef SYS_close_range
	int close_range_status;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (close_range_error != 0) {
		errno = close_range_error;
		close_range_status = -1;
	} else
#endif
		close_range_status = (int)syscall(
			SYS_close_range, 3U, ~0U, 0U);
	if (close_range_status == 0)
		return;
#endif
	long maximum = sysconf(_SC_OPEN_MAX);
	if (maximum < 0 || maximum > 1048576)
		maximum = 65536;
	for (int fd = 3; fd < maximum; ++fd)
		close(fd);
}

#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
void rootless_shutdown_test_close_controller_descriptors(void)
{
	controller_close_descriptors();
}
#endif

static void subreaper_controller_loop(void)
{
	for (;;) {
		int status;
		pid_t child = waitpid(-1, &status, 0);
		if (child > 0)
			continue;
		if (child < 0 && errno == EINTR)
			continue;
		if (child < 0 && errno == ECHILD) {
			/* The anchor is the closure authority, not a transient reaper.
			 * It stays alive even when temporarily childless so shutdown can
			 * prove an empty anchored tree before terminating it last. */
			pause();
			continue;
		}
		_exit(125);
	}
}

static int publish_subreaper_session(
	struct rootless_shutdown_closure_capability* capability, pid_t controller)
{
	struct process_snapshot before;
	int status = process_snapshot_for_pid_at(capability->proc_fd,
		controller, &before);
	if (status != 0)
		return status;
	int pidfd = open_process_pidfd(controller);
	if (pidfd < 0)
		return pidfd;
	struct process_snapshot after;
	status = process_snapshot_for_pid_at(capability->proc_fd,
		controller, &after);
	if (status != 0 || before.start_time != after.start_time ||
		!process_is_active(&after)) {
		close(pidfd);
		return status != 0 ? status : -ESTALE;
	}
	struct stat prefix;
	if (fstat(capability->prefix_fd, &prefix) != 0) {
		status = -errno;
		close(pidfd);
		return status;
	}
	capability->anchor_pid = controller;
	capability->anchor_start_time = after.start_time;
	capability->anchor_pidfd = pidfd;
	struct rootless_shutdown_session_state state = {
		.backend = ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER,
		.prefix_device = prefix.st_dev,
		.prefix_inode = prefix.st_ino,
		.owner_uid = prefix.st_uid,
		.proc_device = capability->proc_device,
		.proc_inode = capability->proc_inode,
		.anchor_pid = controller,
		.anchor_start_time = after.start_time,
	};
	darling_runtime_prefix retained = DARLING_RUNTIME_PREFIX_INITIALIZER;
	retained->directory_fd = capability->prefix_fd;
	status = publish_session_state(retained, capability, &state);
	retained->directory_fd = -1;
	return status;
}

static int startup_checkpoint_status(unsigned phase)
{
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (startup_checkpoint != NULL)
		startup_checkpoint(phase);
	if (startup_error_phase == phase && startup_error_number != 0)
		return -startup_error_number;
#else
	(void)phase;
#endif
	sigset_t pending;
	if (sigpending(&pending) != 0)
		return -errno;
	return sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)
		? -EINTR : 0;
}

static int wait_controller_reaped(pid_t controller,
	unsigned long long deadline, unsigned retry_interval_ms)
{
	for (;;) {
		int wait_status = 0;
		pid_t waited = waitpid(controller, &wait_status, WNOHANG);
		if (waited == controller || (waited < 0 && errno == ECHILD))
			return 0;
		if (waited < 0 && errno != EINTR)
			return -errno;
		int status = deadline_not_expired(deadline);
		if (status != 0)
			return status;
		sleep_milliseconds(retry_interval_ms);
	}
}

static int rollback_subreaper_startup(
	struct rootless_shutdown_closure_capability* capability,
	pid_t controller, int state_published, int runtime_possible)
{
	unsigned long long now = 0;
	int status = monotonic_milliseconds(&now);
	if (status != 0)
		return status;
	const unsigned long long deadline = now +
		default_policy.kill_timeout_ms;
	if (capability->anchor_pidfd < 0) {
		capability->anchor_pidfd = open_process_pidfd(controller);
		if (capability->anchor_pidfd < 0 &&
			capability->anchor_pidfd != -ENOENT &&
			capability->anchor_pidfd != -ESRCH)
			return capability->anchor_pidfd;
	}
	struct process_ledger ledger = {0};
	if (state_published && runtime_possible) {
		size_t budget = 0;
		status = derive_pidfd_budget(&default_policy, &budget);
		while (status == 0) {
			unsigned active = 0;
			status = signal_subreaper_closure(0, 0, capability,
				&ledger, SIGKILL, budget, deadline,
				default_policy.poll_interval_ms, &active);
			if (status != 0 || active == 0)
				break;
			status = deadline_not_expired(deadline);
			if (status == 0)
				sleep_milliseconds(default_policy.poll_interval_ms);
		}
	} else if (capability->anchor_pidfd >= 0) {
		status = signal_process_pidfd(capability->anchor_pidfd, SIGKILL);
		if (status == -ESRCH)
			status = 0;
	}
	if (status == 0)
		status = wait_controller_reaped(controller, deadline,
			default_policy.poll_interval_ms);
	if (status == 0 && state_published)
		status = remove_owned_session_state(capability);
	ledger_release(&ledger);
	return status;
}

pid_t rootless_shutdown_fork_runtime(
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability == NULL)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown backend capability is missing");
	switch (capability->backend) {
	case ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED: {
		pid_t child = fork();
		if (child < 0)
			return shutdown_error(error, error_size, errno,
				"cannot fork rootless runtime: %s", strerror(errno));
		if (child == 0 && rootless_shutdown_enter_closure(
				capability, error, error_size) != 0)
			_exit(126);
		return child;
	}
	case ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER: {
		int ready[2] = {-1, -1};
		int command[2] = {-1, -1};
		int runtime[2] = {-1, -1};
		sigset_t blocked_signals;
		sigset_t previous_signals;
		sigemptyset(&blocked_signals);
		sigaddset(&blocked_signals, SIGINT);
		sigaddset(&blocked_signals, SIGTERM);
		if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_signals) != 0)
			return shutdown_error(error, error_size, errno,
				"cannot block startup transaction signals: %s",
				strerror(errno));
		if (pipe2(ready, O_CLOEXEC) != 0 || pipe2(command, O_CLOEXEC) != 0 ||
			pipe2(runtime, O_CLOEXEC) != 0) {
			int saved = errno;
			for (size_t index = 0; index < 2; ++index) {
				if (ready[index] >= 0) close(ready[index]);
				if (command[index] >= 0) close(command[index]);
				if (runtime[index] >= 0) close(runtime[index]);
			}
			(void)sigprocmask(SIG_SETMASK, &previous_signals, NULL);
			return shutdown_error(error, error_size, saved,
				"cannot create subreaper control pipes: %s", strerror(saved));
		}
		pid_t controller = fork();
		if (controller < 0) {
			int saved = errno;
			close(ready[0]); close(ready[1]);
			close(command[0]); close(command[1]);
			close(runtime[0]); close(runtime[1]);
			(void)sigprocmask(SIG_SETMASK, &previous_signals, NULL);
			return shutdown_error(error, error_size, saved,
				"cannot fork subreaper controller: %s", strerror(saved));
		}
		if (controller == 0) {
			close(ready[0]); close(command[1]); close(runtime[0]);
			struct subreaper_start_message message = {0};
			if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
				message.status = -errno;
			(void)write_exact(ready[1], &message, sizeof(message));
			close(ready[1]);
			char go = 0;
			if (message.status != 0 ||
				read_exact(command[0], &go, sizeof(go)) != 0 || go != 1)
				_exit(124);
			close(command[0]);
			pid_t child = fork();
			if (child < 0) {
				message.status = -errno;
				(void)write_exact(runtime[1], &message, sizeof(message));
				_exit(123);
			}
			if (child == 0) {
				close(runtime[1]);
				if (sigprocmask(SIG_SETMASK, &previous_signals, NULL) != 0)
					_exit(126);
				return 0;
			}
			message.runtime_pid = child;
			(void)write_exact(runtime[1], &message, sizeof(message));
			close(runtime[1]);
			rootless_shutdown_release_closure(capability);
			controller_close_descriptors();
			if (sigprocmask(SIG_SETMASK, &previous_signals, NULL) != 0)
				_exit(126);
			subreaper_controller_loop();
		}
		close(ready[1]); close(command[0]); close(runtime[1]);
		struct subreaper_start_message message = {0};
		int state_published = 0;
		int runtime_possible = 0;
		int status = read_exact(ready[0], &message, sizeof(message));
		close(ready[0]);
		if (status == 0)
			status = message.status;
		if (status == 0)
			status = startup_checkpoint_status(
				ROOTLESS_SHUTDOWN_TEST_STARTUP_CONTROLLER_READY);
		if (status == 0) {
			status = publish_subreaper_session(capability, controller);
			state_published = status == 0;
		}
		if (status == 0)
			status = startup_checkpoint_status(
				ROOTLESS_SHUTDOWN_TEST_STARTUP_SESSION_PUBLISHED);
		char go = status == 0 ? 1 : 0;
		int command_status = write_exact(command[1], &go, sizeof(go));
		if (command_status != 0 && status == 0)
			status = -EPIPE;
		else if (command_status == 0 && go == 1)
			runtime_possible = 1;
		close(command[1]);
		if (status == 0)
			status = startup_checkpoint_status(
				ROOTLESS_SHUTDOWN_TEST_STARTUP_COMMAND_SENT);
		if (status == 0) {
			status = read_exact(runtime[0], &message, sizeof(message));
			if (status == 0)
				status = message.status;
		} else if (runtime_possible) {
			/* Keep the controller's report channel alive until it has either
			 * published the runtime child or reported the fork failure. Closing
			 * it here would let SIGPIPE kill the subreaper before rollback can
			 * traverse the newly orphaned child. */
			struct subreaper_start_message rollback_message = {0};
			int report_status = read_exact(runtime[0], &rollback_message,
				sizeof(rollback_message));
			if (report_status != 0)
				status = report_status;
			else if (rollback_message.status != 0)
				status = rollback_message.status;
		}
		if (status == 0)
			status = startup_checkpoint_status(
				ROOTLESS_SHUTDOWN_TEST_STARTUP_RUNTIME_REPORTED);
		close(runtime[0]);
		if (status != 0) {
			int startup_status = status;
			int rollback_status = rollback_subreaper_startup(
				capability, controller, state_published, runtime_possible);
			(void)sigprocmask(SIG_SETMASK, &previous_signals, NULL);
			if (rollback_status != 0)
				return shutdown_error(error, error_size, -rollback_status,
					"cannot roll back pidfd subreaper startup: %s",
					strerror(-rollback_status));
			return shutdown_error(error, error_size, -startup_status,
				"cannot start pidfd subreaper runtime: %s",
				strerror(-startup_status));
		}
		if (sigprocmask(SIG_SETMASK, &previous_signals, NULL) != 0) {
			status = -errno;
			int rollback_status = rollback_subreaper_startup(
				capability, controller, state_published, 1);
			return shutdown_error(error, error_size,
				rollback_status != 0 ? -rollback_status : -status,
				"cannot restore startup transaction signal mask: %s",
				strerror(rollback_status != 0 ? -rollback_status : -status));
		}
		return message.runtime_pid;
	}
	case ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED:
		return shutdown_error(error, error_size, EOPNOTSUPP,
			"rootless shutdown is unsupported by this kernel environment");
	}
	return shutdown_error(error, error_size, EOPNOTSUPP,
		"rootless shutdown backend is invalid");
}

int rootless_shutdown_enter_closure(
	const struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability != NULL &&
		capability->backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER) {
		if (error != NULL && error_size != 0)
			error[0] = '\0';
		return 0;
	}
	if (capability == NULL ||
		capability->backend != ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED ||
		capability->membership_fd < 0)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure membership capability is invalid");
	char pid[32];
	int length = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
	if (length < 0 || (size_t)length >= sizeof(pid))
		return shutdown_error(error, error_size, EOVERFLOW,
			"cannot encode rootless shutdown process identity");
	ssize_t written;
	do {
		written = write(capability->membership_fd, pid, (size_t)length);
	} while (written < 0 && errno == EINTR);
	if (written != length)
		return shutdown_error(error, error_size,
			written < 0 ? errno : EIO,
			"cannot enter rootless shutdown cgroup: %s",
			written < 0 ? strerror(errno) : "short write");
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
}

static int deadline_not_expired(unsigned long long deadline)
{
	unsigned long long now = 0;
	int status = monotonic_milliseconds(&now);
	if (status != 0)
		return status;
	return now < deadline ? 0 : -ETIMEDOUT;
}

static int cgroup_read_pids(int directory_fd, pid_t** output, size_t* count,
	size_t pidfd_budget, unsigned long long deadline)
{
	int fd = openat(directory_fd, "cgroup.procs",
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	FILE* stream = fdopen(fd, "r");
	if (stream == NULL) {
		int status = -errno;
		close(fd);
		return status;
	}
	*output = NULL;
	*count = 0;
	size_t output_capacity = 0;
	char* line = NULL;
	size_t capacity = 0;
	int status = 0;
	for (;;) {
		status = deadline_not_expired(deadline);
		if (status != 0)
			break;
		ssize_t length = getline(&line, &capacity, stream);
		if (length < 0)
			break;
		char* end = NULL;
		errno = 0;
		long value = strtol(line, &end, 10);
		if (errno != 0 || value <= 0 || (pid_t)value != value ||
			(end[0] != '\n' && end[0] != '\0') ||
			(end[0] == '\n' && end[1] != '\0')) {
			status = -EPROTO;
			break;
		}
		if (*count == output_capacity) {
			size_t maximum = pidfd_budget == SIZE_MAX
				? SIZE_MAX : pidfd_budget + 1;
			size_t grown_capacity = output_capacity == 0 ? 16
				: output_capacity <= SIZE_MAX / 2
					? output_capacity * 2 : SIZE_MAX;
			if (grown_capacity > maximum)
				grown_capacity = maximum;
			if (grown_capacity <= output_capacity ||
				grown_capacity > SIZE_MAX / sizeof(**output)) {
				status = -EMFILE;
				break;
			}
			pid_t* grown = realloc(*output,
				grown_capacity * sizeof(**output));
			if (grown == NULL) {
				status = -ENOMEM;
				break;
			}
			*output = grown;
			output_capacity = grown_capacity;
		}
		(*output)[(*count)++] = (pid_t)value;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		if (membership_read_checkpoint != NULL)
			membership_read_checkpoint(*count);
#endif
		status = deadline_not_expired(deadline);
		if (status != 0)
			break;
		if (*count > pidfd_budget) {
			status = -EMFILE;
			break;
		}
	}
	if (status == 0 && ferror(stream))
		status = errno != 0 ? -errno : -EIO;
	free(line);
	fclose(stream);
	if (status != 0) {
		free(*output);
		*output = NULL;
		*count = 0;
	}
	return status;
}

static int append_unique_pid(pid_t** pids, size_t* count, size_t* capacity,
	pid_t pid, size_t budget)
{
	for (size_t index = 0; index < *count; ++index) {
		if ((*pids)[index] == pid)
			return 0;
	}
	if (*count >= budget)
		return -EMFILE;
	if (*count == *capacity) {
		size_t grown = *capacity == 0 ? 16 : *capacity * 2;
		if (grown < *capacity || grown > budget)
			grown = budget;
		if (grown <= *capacity || grown > SIZE_MAX / sizeof(**pids))
			return -EMFILE;
		pid_t* replacement = realloc(*pids, grown * sizeof(**pids));
		if (replacement == NULL)
			return -ENOMEM;
		*pids = replacement;
		*capacity = grown;
	}
	(*pids)[(*count)++] = pid;
	return 0;
}

/* Read only the retained anchor's descendant edges. No global /proc walk is
 * permitted: every path starts at a previously identity-bound PID and visits
 * that process's thread children files. */
static int proc_read_task_children(struct process_identity* parent,
	pid_t** children, size_t* child_count, size_t* child_capacity,
	size_t budget, unsigned long long deadline)
{
	if (parent == NULL || parent->proc_directory_fd < 0)
		return -EINVAL;
	if (parent->barrier == PROCESS_BARRIER_GONE)
		return 0;
	if (parent->barrier != PROCESS_BARRIER_STOPPED)
		return -EAGAIN;
	enum process_barrier_result verified = PROCESS_BARRIER_UNKNOWN;
	int status = inspect_identity_barrier(parent, &verified);
	if (status != 0)
		return status;
	parent->barrier = verified;
	if (verified == PROCESS_BARRIER_GONE)
		return 0;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (proc_children_checkpoint != NULL)
		proc_children_checkpoint(parent->pid);
#endif
	int task_fd = openat(parent->proc_directory_fd, "task",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (task_fd < 0) {
		if (errno != ENOENT)
			return -errno;
		status = inspect_identity_barrier(parent, &verified);
		if (status == 0 && verified == PROCESS_BARRIER_GONE) {
			parent->barrier = verified;
			return 0;
		}
		return status != 0 ? status : -EAGAIN;
	}
	DIR* tasks = fdopendir(task_fd);
	if (tasks == NULL) {
		int status = -errno;
		close(task_fd);
		return status;
	}
	status = 0;
	for (;;) {
		status = deadline_not_expired(deadline);
		if (status != 0)
			break;
		errno = 0;
		struct dirent* task = readdir(tasks);
		if (task == NULL) {
			status = errno == 0 ? 0 : -errno;
			break;
		}
		char* end = NULL;
		errno = 0;
		long tid = strtol(task->d_name, &end, 10);
		if (errno != 0 || *task->d_name == '\0' || *end != '\0' ||
			tid <= 0 || (pid_t)tid != tid)
			continue;
		char relative[96];
		int length = snprintf(relative, sizeof(relative), "%s/children",
			task->d_name);
		if (length < 0 || (size_t)length >= sizeof(relative)) {
			status = -EOVERFLOW;
			break;
		}
		int child_fd = openat(dirfd(tasks), relative,
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (child_fd < 0) {
			if (errno == ENOENT)
				continue;
			status = -errno;
			break;
		}
		FILE* stream = fdopen(child_fd, "r");
		if (stream == NULL) {
			status = -errno;
			close(child_fd);
			break;
		}
		for (;;) {
			status = deadline_not_expired(deadline);
			if (status != 0)
				break;
			long child;
			int scanned = fscanf(stream, "%ld", &child);
			if (scanned == EOF) {
				status = ferror(stream) ? -EIO : 0;
				break;
			}
			if (scanned != 1 || child <= 0 || (pid_t)child != child) {
				status = -EPROTO;
				break;
			}
			status = append_unique_pid(children, child_count,
				child_capacity, (pid_t)child, budget);
			if (status != 0)
				break;
		}
		fclose(stream);
		if (status != 0)
			break;
	}
	closedir(tasks);
	return status;
}

static int subreaper_contains(
	const struct rootless_shutdown_closure_capability* capability,
	pid_t wanted, size_t budget, unsigned long long deadline)
{
	struct process_ledger ledger = {0};
	int status = acquire_subreaper_closure(capability, &ledger,
		budget, deadline, default_policy.poll_interval_ms);
	int found = status == 0 && ledger_find_pid(&ledger, wanted) != NULL;
	int resume_status = resume_stopped_identities(&ledger);
	ledger_release(&ledger);
	if (status != 0)
		return status;
	return resume_status != 0 ? resume_status : found;
}

int rootless_shutdown_closure_contains(
	const struct rootless_shutdown_closure_capability* capability, pid_t pid,
	char* error, size_t error_size)
{
	if (capability == NULL || pid <= 0)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure lookup is invalid");
	int identity_status = closure_named_identity(capability);
	if (identity_status != 0)
		return shutdown_error(error, error_size, -identity_status,
			"rootless shutdown cgroup identity changed");
	pid_t* members = NULL;
	size_t count = 0;
	size_t budget = 0;
	int status = derive_pidfd_budget(&default_policy, &budget);
	unsigned long long now = 0;
	if (status == 0)
		status = monotonic_milliseconds(&now);
	if (status == 0 &&
		capability->backend == ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED)
		status = cgroup_read_pids(capability->directory_fd, &members, &count,
			budget, now + default_policy.acquisition_timeout_ms);
	else if (status == 0 &&
		capability->backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER) {
		status = subreaper_contains(capability, pid, budget,
			now + default_policy.acquisition_timeout_ms);
		if (status > 0) {
			if (error != NULL && error_size != 0)
				error[0] = '\0';
			return 0;
		}
	}
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot read rootless shutdown backend: %s", strerror(-status));
	int found = 0;
	for (size_t index = 0; index < count; ++index)
		found |= members[index] == pid;
	free(members);
	if (!found)
		return shutdown_error(error, error_size, ESRCH,
			"process %d is outside the rootless shutdown cgroup", (int)pid);
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
}

int rootless_shutdown_cleanup_empty_closure(
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability == NULL)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure capability is invalid");
	int status = closure_named_identity(capability);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"rootless shutdown backend identity changed");
	if (capability->backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER) {
		int active = 0;
		status = process_pidfd_active(capability->anchor_pidfd, &active);
		if (status != 0 || active)
			return shutdown_error(error, error_size,
				status != 0 ? -status : EBUSY,
				"rootless shutdown subreaper is not empty");
		status = remove_owned_session_state(capability);
		if (status != 0)
			return shutdown_error(error, error_size, -status,
				"cannot remove rootless shutdown session identity: %s",
				strerror(-status));
		rootless_shutdown_release_closure(capability);
		if (error != NULL && error_size != 0)
			error[0] = '\0';
		return 0;
	}
	if (capability->backend != ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED ||
		capability->parent_fd < 0 || capability->directory_fd < 0 ||
		capability->leaf[0] == '\0')
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown cgroup capability is invalid");
	int populated = 1;
	status = cgroup_populated_fd(capability->directory_fd, &populated);
	if (status != 0 || populated)
		return shutdown_error(error, error_size,
			status != 0 ? -status : EBUSY,
			"rootless shutdown cgroup is not empty");
	if (capability->membership_fd >= 0) {
		close(capability->membership_fd);
		capability->membership_fd = -1;
	}
	status = closure_named_identity(capability);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"rootless shutdown cgroup changed before removal");
	if (unlinkat(capability->parent_fd, capability->leaf, AT_REMOVEDIR) != 0)
		return shutdown_error(error, error_size, errno,
			"cannot remove rootless shutdown cgroup: %s", strerror(errno));
	status = remove_owned_session_state(capability);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot remove rootless shutdown session identity: %s",
			strerror(-status));
	rootless_shutdown_release_closure(capability);
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
}

static int process_snapshot_from_stat_fd(int fd,
	struct process_snapshot* snapshot)
{
	char stat_line[4096];
	ssize_t length;
	char* fields;
	char* save = NULL;
	char* token;
	unsigned field = 3;
	int found_session = 0;
	int found_start_time = 0;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return -errno;
	length = read(fd, stat_line, sizeof(stat_line) - 1);
	if (length <= 0) {
		return length == 0 ? -EIO : -errno;
	}
	stat_line[length] = '\0';
	fields = strrchr(stat_line, ')');
	if (fields == NULL || fields[1] != ' ')
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	for (token = strtok_r(fields + 2, " ", &save);
		token != NULL;
		token = strtok_r(NULL, " ", &save), ++field) {
		char* end = NULL;
		if (field == 3) {
			if (token[0] == '\0' || token[1] != '\0')
				return -EINVAL;
			snapshot->state = token[0];
		} else if (field == 6) {
			long value = strtol(token, &end, 10);
			if (*token == '\0' || *end != '\0' || value < 0 ||
				(pid_t)value != value)
				return -EINVAL;
			snapshot->session = (pid_t)value;
			found_session = 1;
		} else if (field == 22) {
			errno = 0;
			unsigned long long value = strtoull(token, &end, 10);
			if (errno != 0 || *token == '\0' || *end != '\0')
				return -EINVAL;
			snapshot->start_time = value;
			found_start_time = 1;
			break;
		}
	}
	return found_session && found_start_time ? 0 : -EINVAL;
}

static int process_snapshot_for_directory_fd(int process_fd,
	struct process_snapshot* snapshot)
{
	int stat_fd = openat(process_fd, "stat",
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (stat_fd < 0)
		return -errno;
	int status = process_snapshot_from_stat_fd(stat_fd, snapshot);
	int saved_errno = errno;
	close(stat_fd);
	errno = saved_errno;
	return status;
}

static int process_snapshot_for_pid_at(int proc_fd, pid_t pid,
	struct process_snapshot* snapshot)
{
	char path[64];
	if (proc_fd >= 0)
		snprintf(path, sizeof(path), "%d/stat", pid);
	else
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	int fd = openat(proc_fd >= 0 ? proc_fd : AT_FDCWD, path,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	int status = process_snapshot_from_stat_fd(fd, snapshot);
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return status;
}

static int process_snapshot_for_pid(pid_t pid,
	struct process_snapshot* snapshot)
{
	return process_snapshot_for_pid_at(-1, pid, snapshot);
}

static int process_is_active(const struct process_snapshot* snapshot)
{
	return snapshot->state != 'Z' && snapshot->state != 'X';
}

static int process_matches_identity(pid_t pid,
	const struct process_snapshot* snapshot,
	const struct process_identity* identity)
{
	return pid == identity->pid &&
		snapshot->start_time == identity->start_time;
}

static struct process_identity* ledger_find(struct process_ledger* ledger,
	pid_t pid, const struct process_snapshot* snapshot)
{
	size_t left = 0;
	size_t right = ledger->count;
	while (left < right) {
		size_t middle = left + (right - left) / 2;
		if (ledger->identities[middle].pid < pid)
			left = middle + 1;
		else
			right = middle;
	}
	if (left < ledger->count &&
		process_matches_identity(pid, snapshot, &ledger->identities[left]))
		return &ledger->identities[left];
	return NULL;
}

static int process_pidfd_active(int pidfd, int* active)
{
	struct pollfd descriptor = {
		.fd = pidfd,
		.events = POLLIN,
	};
	int result;
	do {
		result = poll(&descriptor, 1, 0);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return -errno;
	if ((descriptor.revents & POLLNVAL) != 0)
		return -EBADF;
	*active = result == 0;
	return 0;
}

static int compare_pids(const void* left, const void* right)
{
	pid_t lhs = *(const pid_t*)left;
	pid_t rhs = *(const pid_t*)right;
	return lhs < rhs ? -1 : lhs > rhs;
}

static void sort_pid_list(pid_t* members, size_t count)
{
	if (count >= 2)
		qsort(members, count, sizeof(*members), compare_pids);
}

static int pid_lists_equal(const pid_t* left, size_t left_count,
	const pid_t* right, size_t right_count)
{
	return left_count == right_count &&
		(left_count == 0 || memcmp(left, right,
			left_count * sizeof(*left)) == 0);
}

#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
static void (*pidfd_open_checkpoint)(pid_t);

void rootless_shutdown_test_set_pidfd_open_checkpoint(void (*checkpoint)(pid_t))
{
	pidfd_open_checkpoint = checkpoint;
}
#endif

static int ledger_add(struct process_ledger* ledger, pid_t pid,
	const struct process_snapshot* snapshot, int pidfd, int process_fd)
{
	if (ledger_find(ledger, pid, snapshot) != NULL) {
		close(pidfd);
		close(process_fd);
		return 0;
	}
	if (ledger->count == ledger->capacity) {
		size_t capacity = ledger->capacity == 0 ? 16 : ledger->capacity * 2;
		if (capacity < ledger->capacity ||
			capacity > SIZE_MAX / sizeof(*ledger->identities))
			return -EOVERFLOW;
		struct process_identity* grown = realloc(ledger->identities,
			capacity * sizeof(*ledger->identities));
		if (grown == NULL)
			return -ENOMEM;
		ledger->identities = grown;
		ledger->capacity = capacity;
	}
	size_t position = 0;
	while (position < ledger->count &&
		ledger->identities[position].pid < pid)
		position++;
	if (position < ledger->count && ledger->identities[position].pid == pid) {
		return -ESTALE;
	}
	memmove(&ledger->identities[position + 1],
		&ledger->identities[position],
		(ledger->count - position) * sizeof(*ledger->identities));
	ledger->identities[position] = (struct process_identity) {
		.pid = pid,
		.start_time = snapshot->start_time,
		.pidfd = pidfd,
		.proc_directory_fd = process_fd,
	};
	ledger->count++;
	ledger->observed++;
	return 0;
}

static int ledger_compact(struct process_ledger* ledger,
	const pid_t* members, size_t member_count,
	unsigned long long deadline, unsigned snapshot_ordinal)
{
	size_t kept = 0;
	size_t member = 0;
	size_t original_count = ledger->count;
	for (size_t index = 0; index < ledger->count; ++index) {
		int status = deadline_not_expired(deadline);
		if (status != 0) {
			memmove(&ledger->identities[kept],
				&ledger->identities[index],
				(original_count - index) * sizeof(*ledger->identities));
			ledger->count = kept + original_count - index;
			return status;
		}
		struct process_identity identity = ledger->identities[index];
		while (member < member_count && members[member] < identity.pid)
			member++;
		int active = 0;
		status = process_pidfd_active(identity.pidfd, &active);
		if (status != 0) {
			memmove(&ledger->identities[kept],
				&ledger->identities[index],
				(original_count - index) * sizeof(*ledger->identities));
			ledger->count = kept + original_count - index;
			return status;
		}
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		if (ledger_compact_checkpoint != NULL)
			ledger_compact_checkpoint(snapshot_ordinal, index + 1);
#else
		(void)snapshot_ordinal;
#endif
		status = deadline_not_expired(deadline);
		if (status != 0) {
			memmove(&ledger->identities[kept],
				&ledger->identities[index],
				(original_count - index) * sizeof(*ledger->identities));
			ledger->count = kept + original_count - index;
			return status;
		}
		if (!active || member == member_count ||
			members[member] != identity.pid) {
			close(identity.pidfd);
			close(identity.proc_directory_fd);
			continue;
		}
		ledger->identities[kept++] = identity;
	}
	ledger->count = kept;
	return deadline_not_expired(deadline);
}

static void ledger_release(struct process_ledger* ledger)
{
	for (size_t index = 0; index < ledger->count; ++index) {
		if (ledger->identities[index].pidfd >= 0)
			close(ledger->identities[index].pidfd);
		if (ledger->identities[index].proc_directory_fd >= 0)
			close(ledger->identities[index].proc_directory_fd);
	}
	free(ledger->identities);
	*ledger = (struct process_ledger){0};
}

static int ledger_compact_active(struct process_ledger* ledger,
	unsigned long long deadline)
{
	size_t kept = 0;
	for (size_t index = 0; index < ledger->count; ++index) {
		int status = deadline_not_expired(deadline);
		if (status != 0)
			return status;
		int active = 0;
		status = process_pidfd_active(ledger->identities[index].pidfd, &active);
		if (status != 0)
			return status;
		if (!active) {
			close(ledger->identities[index].pidfd);
			close(ledger->identities[index].proc_directory_fd);
			continue;
		}
		ledger->identities[kept++] = ledger->identities[index];
	}
	ledger->count = kept;
	return deadline_not_expired(deadline);
}

static struct process_identity* ledger_find_pid(
	struct process_ledger* ledger, pid_t pid)
{
	for (size_t index = 0; index < ledger->count; ++index) {
		if (ledger->identities[index].pid == pid)
			return &ledger->identities[index];
	}
	return NULL;
}

static int inspect_identity_barrier(struct process_identity* identity,
	enum process_barrier_result* result)
{
	int active = 0;
	int status = process_pidfd_active(identity->pidfd, &active);
	if (status != 0)
		return status;
	if (!active) {
		*result = PROCESS_BARRIER_GONE;
		return 0;
	}
	struct process_snapshot leader;
	status = process_snapshot_for_directory_fd(
		identity->proc_directory_fd, &leader);
	if (status == -ENOENT || status == -ESRCH) {
		status = process_pidfd_active(identity->pidfd, &active);
		if (status != 0)
			return status;
		if (!active) {
			*result = PROCESS_BARRIER_GONE;
			return 0;
		}
		return -EAGAIN;
	}
	if (status != 0)
		return status;
	if (leader.start_time != identity->start_time)
		return -ESTALE;
	if (!process_is_active(&leader)) {
		*result = PROCESS_BARRIER_GONE;
		return 0;
	}
	if (leader.state != 'T' && leader.state != 't')
		return -EAGAIN;
	int task_fd = openat(identity->proc_directory_fd, "task",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (task_fd < 0)
		return errno == ENOENT ? -EAGAIN : -errno;
	DIR* tasks = fdopendir(task_fd);
	if (tasks == NULL) {
		status = -errno;
		close(task_fd);
		return status;
	}
	for (;;) {
		errno = 0;
		struct dirent* task = readdir(tasks);
		if (task == NULL) {
			status = errno == 0 ? 0 : -errno;
			break;
		}
		char* end = NULL;
		errno = 0;
		long tid = strtol(task->d_name, &end, 10);
		if (errno != 0 || task->d_name[0] == '\0' || *end != '\0' ||
			tid <= 0 || (pid_t)tid != tid)
			continue;
		char path[64];
		int length = snprintf(path, sizeof(path), "%s/stat", task->d_name);
		if (length < 0 || (size_t)length >= sizeof(path)) {
			status = -EOVERFLOW;
			break;
		}
		int stat_fd = openat(dirfd(tasks), path,
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (stat_fd < 0) {
			if (errno == ENOENT)
				continue;
			status = -errno;
			break;
		}
		struct process_snapshot thread;
		status = process_snapshot_from_stat_fd(stat_fd, &thread);
		close(stat_fd);
		if (status != 0)
			break;
		if (thread.state != 'T' && thread.state != 't' &&
			process_is_active(&thread)) {
			status = -EAGAIN;
			break;
		}
	}
	closedir(tasks);
	if (status != 0)
		return status;
	status = process_pidfd_active(identity->pidfd, &active);
	if (status != 0)
		return status;
	*result = active ? PROCESS_BARRIER_STOPPED : PROCESS_BARRIER_GONE;
	return 0;
}

static int wait_identity_stopped(struct process_identity* identity,
	unsigned long long deadline, unsigned retry_interval_ms)
{
	for (;;) {
		int status = deadline_not_expired(deadline);
		if (status != 0)
			return status;
		enum process_barrier_result result = PROCESS_BARRIER_UNKNOWN;
		status = inspect_identity_barrier(identity, &result);
		if (status == 0) {
			identity->barrier = result;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
			if (proc_barrier_checkpoint != NULL)
				proc_barrier_checkpoint(identity->pid, (int)result);
#endif
			return 0;
		}
		if (status != -EAGAIN)
			return status;
		sleep_milliseconds(retry_interval_ms);
	}
}

static int stop_identity(struct process_identity* identity,
	unsigned long long deadline, unsigned retry_interval_ms)
{
	int active = 0;
	int status = process_pidfd_active(identity->pidfd, &active);
	if (status != 0)
		return status;
	if (!active) {
		identity->barrier = PROCESS_BARRIER_GONE;
		return 0;
	}
	struct process_snapshot snapshot;
	status = process_snapshot_for_directory_fd(
		identity->proc_directory_fd, &snapshot);
	if (status == -ENOENT || status == -ESRCH)
		return wait_identity_stopped(identity, deadline, retry_interval_ms);
	if (status != 0 || snapshot.start_time != identity->start_time)
		return status != 0 ? status : -ESTALE;
	if (process_is_active(&snapshot) && snapshot.state != 'T' &&
		snapshot.state != 't') {
		status = deadline_not_expired(deadline);
		if (status != 0)
			return status;
		status = signal_process_pidfd(identity->pidfd, SIGSTOP);
		if (status != 0)
			return status;
		identity->stopped_by_us = 1;
	}
	return wait_identity_stopped(identity, deadline, retry_interval_ms);
}

static int resume_stopped_identities(struct process_ledger* ledger)
{
	int outcome = 0;
	for (size_t index = 0; index < ledger->count; ++index) {
		if (!ledger->identities[index].stopped_by_us)
			continue;
		int status = signal_process_pidfd(
			ledger->identities[index].pidfd, SIGCONT);
		if (status != 0 && outcome == 0)
			outcome = status;
		ledger->identities[index].stopped_by_us = 0;
		ledger->identities[index].barrier = PROCESS_BARRIER_UNKNOWN;
	}
	return outcome;
}

static int pid_list_contains(const pid_t* pids, size_t count, pid_t pid)
{
	for (size_t index = 0; index < count; ++index) {
		if (pids[index] == pid)
			return 1;
	}
	return 0;
}

static void ledger_remove_new_pid(struct process_ledger* ledger, pid_t pid,
	size_t old_count)
{
	if (ledger->count <= old_count)
		return;
	for (size_t index = 0; index < ledger->count; ++index) {
		if (ledger->identities[index].pid != pid)
			continue;
		close(ledger->identities[index].pidfd);
		close(ledger->identities[index].proc_directory_fd);
		memmove(&ledger->identities[index], &ledger->identities[index + 1],
			(ledger->count - index - 1) * sizeof(*ledger->identities));
		ledger->count--;
		return;
	}
}

static int capture_anchored_child(
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, struct process_identity* parent, pid_t child,
	size_t pidfd_budget, unsigned long long deadline)
{
	size_t old_count = ledger->count;
	enum process_capture_result capture = PROCESS_CAPTURE_GONE;
	int status = ledger_capture_member(ledger, closure->proc_fd,
		child, pidfd_budget, &capture);
	if (status != 0)
		return status;
	pid_t* confirmation = NULL;
	size_t count = 0;
	size_t capacity = 0;
	status = proc_read_task_children(parent,
		&confirmation, &count, &capacity, pidfd_budget, deadline);
	int retained = status == 0 && pid_list_contains(confirmation, count, child);
	free(confirmation);
	if (status != 0)
		return status;
	if (!retained)
		ledger_remove_new_pid(ledger, child, old_count);
	else if (ledger_find_pid(ledger, child) == NULL &&
		capture != PROCESS_CAPTURE_INACTIVE)
		return -EAGAIN;
	return 0;
}

static int acquire_subreaper_closure(
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, size_t pidfd_budget,
	unsigned long long deadline, unsigned retry_interval_ms)
{
	int status = closure_named_identity(closure);
	if (status != 0)
		return status;
	status = ledger_compact_active(ledger, deadline);
	if (status != 0)
		return status;
	status = ledger_capture_member(ledger, closure->proc_fd,
		closure->anchor_pid, pidfd_budget, NULL);
	if (status != 0)
		return status;
	struct process_identity* anchor = ledger_find_pid(ledger,
		closure->anchor_pid);
	if (anchor == NULL || anchor->start_time != closure->anchor_start_time)
		return -ESTALE;
	pid_t* queue = NULL;
	size_t count = 0;
	size_t capacity = 0;
	status = append_unique_pid(&queue, &count, &capacity,
		closure->anchor_pid, pidfd_budget);
	size_t processed = 0;
	while (status == 0) {
		while (processed < count && status == 0) {
			pid_t parent = queue[processed++];
			struct process_identity* identity = ledger_find_pid(ledger, parent);
			if (identity == NULL)
				continue;
			status = stop_identity(identity,
				deadline, retry_interval_ms);
			if (status != 0)
				break;
			if (identity->barrier == PROCESS_BARRIER_GONE)
				continue;
			pid_t* children = NULL;
			size_t child_count = 0;
			size_t child_capacity = 0;
			status = proc_read_task_children(identity,
				&children, &child_count, &child_capacity,
				pidfd_budget, deadline);
			for (size_t child = 0; status == 0 && child < child_count; ++child) {
				status = capture_anchored_child(closure, ledger, identity,
					children[child], pidfd_budget, deadline);
				if (status == 0 && ledger_find_pid(ledger, children[child]) != NULL)
					status = append_unique_pid(&queue, &count, &capacity,
						children[child], pidfd_budget);
			}
			free(children);
		}
		if (status != 0)
			break;
		/* Every retained identity is stopped. Rescan all anchored edges to
		 * close the root-exit/reparent window; the generation is exact only
		 * when a complete pass adds no identity. */
		size_t stable_count = count;
		for (size_t index = 0; status == 0 && index < stable_count; ++index) {
			struct process_identity* identity =
				ledger_find_pid(ledger, queue[index]);
			if (identity == NULL ||
				identity->barrier == PROCESS_BARRIER_GONE)
				continue;
			if (identity->barrier != PROCESS_BARRIER_STOPPED) {
				status = -EAGAIN;
				break;
			}
			pid_t* children = NULL;
			size_t child_count = 0;
			size_t child_capacity = 0;
			status = proc_read_task_children(identity,
				&children, &child_count, &child_capacity,
				pidfd_budget, deadline);
			for (size_t child = 0; status == 0 && child < child_count; ++child) {
				status = capture_anchored_child(closure, ledger, identity,
					children[child], pidfd_budget, deadline);
				if (status == 0 && ledger_find_pid(ledger, children[child]) != NULL)
					status = append_unique_pid(&queue, &count, &capacity,
						children[child], pidfd_budget);
			}
			free(children);
		}
		if (status != 0 || count == stable_count)
			break;
	}
	free(queue);
	if (status != 0)
		(void)resume_stopped_identities(ledger);
	return status;
}

static int signal_subreaper_closure(pid_t init_process,
	unsigned long long init_start_time,
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, int signal_number, size_t pidfd_budget,
	unsigned long long deadline, unsigned retry_interval_ms,
	unsigned* active)
{
	int anchor_active = 0;
	int status = process_pidfd_active(closure->anchor_pidfd, &anchor_active);
	if (status != 0)
		return status;
	if (!anchor_active) {
		struct process_identity* prior = ledger_find_pid(ledger,
			closure->anchor_pid);
		if (prior == NULL || !prior->termination_requested)
			return -ESTALE;
		status = ledger_compact_active(ledger, deadline);
		if (status == 0)
			*active = 0;
		return status;
	}
	status = acquire_subreaper_closure(closure, ledger, pidfd_budget,
		deadline, retry_interval_ms);
	if (status != 0)
		return status;
	struct process_identity* root = NULL;
	struct process_identity* anchor = NULL;
	unsigned other_descendants = 0;
	*active = 0;
	for (size_t index = 0; index < ledger->count; ++index) {
		struct process_identity* identity = &ledger->identities[index];
		int identity_active = 0;
		status = process_pidfd_active(identity->pidfd, &identity_active);
		if (status != 0)
			goto unwind;
		if (!identity_active)
			continue;
		(*active)++;
		if (identity->pid == closure->anchor_pid &&
			identity->start_time == closure->anchor_start_time) {
			anchor = identity;
			continue;
		}
		if (identity->pid == init_process && init_start_time != 0 &&
			identity->start_time == init_start_time) {
			root = identity;
			continue;
		}
		other_descendants++;
		if (signal_number != 0) {
			status = deadline_not_expired(deadline);
			if (status != 0)
				goto unwind;
			status = signal_process_pidfd(identity->pidfd, signal_number);
			if (status != 0)
				goto unwind;
		}
	}
	if (signal_number != 0 && other_descendants == 0 && root != NULL) {
		status = deadline_not_expired(deadline);
		if (status == 0)
			status = signal_process_pidfd(root->pidfd, signal_number);
		if (status != 0)
			goto unwind;
	} else if (signal_number != 0 && other_descendants == 0 && root == NULL &&
		anchor != NULL) {
		status = deadline_not_expired(deadline);
		if (status == 0)
			status = signal_process_pidfd(anchor->pidfd, signal_number);
		if (status == 0)
			anchor->termination_requested = 1;
		if (status != 0)
			goto unwind;
	}
	status = resume_stopped_identities(ledger);
	return status;
unwind:
	(void)resume_stopped_identities(ledger);
	return status;
}

/*
 * Capture one cgroup member as an identity-bound capability. The snapshot
 * before pidfd_open detects PID reuse across acquisition; the second cgroup
 * read proves that the exact numeric identity was still a member after the
 * pidfd became authoritative. Once retained, only pidfd_send_signal is used.
 */
static int ledger_capture_member(struct process_ledger* ledger, int proc_fd,
	pid_t pid, size_t pidfd_budget, enum process_capture_result* result)
{
	if (result != NULL)
		*result = PROCESS_CAPTURE_GONE;
	char name[32];
	int length = snprintf(name, sizeof(name), "%d", (int)pid);
	if (length < 0 || (size_t)length >= sizeof(name))
		return -EOVERFLOW;
	int process_fd = openat(proc_fd, name,
		O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (process_fd < 0)
		return errno == ENOENT ? 0 : -errno;
	struct stat opened_identity;
	if (fstat(process_fd, &opened_identity) != 0) {
		int status = -errno;
		close(process_fd);
		return status;
	}
	struct process_snapshot before;
	int status = process_snapshot_for_directory_fd(process_fd, &before);
	if (status == -ENOENT || status == -ESRCH) {
		close(process_fd);
		return 0;
	}
	if (status != 0) {
		close(process_fd);
		return status;
	}
	if (!process_is_active(&before)) {
		if (result != NULL)
			*result = PROCESS_CAPTURE_INACTIVE;
		close(process_fd);
		return 0;
	}
	if (ledger_find(ledger, pid, &before) != NULL) {
		if (result != NULL)
			*result = PROCESS_CAPTURE_RETAINED;
		close(process_fd);
		return 0;
	}
	if (ledger->count >= pidfd_budget) {
		close(process_fd);
		return -EMFILE;
	}
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (pidfd_open_checkpoint != NULL)
		pidfd_open_checkpoint(pid);
#endif
	int pidfd = open_process_pidfd(pid);
	if (pidfd == -ENOENT || pidfd == -ESRCH) {
		close(process_fd);
		return 0;
	}
	if (pidfd < 0) {
		close(process_fd);
		return pidfd;
	}
	struct process_snapshot after;
	status = process_snapshot_for_directory_fd(process_fd, &after);
	if (status == -ENOENT || status == -ESRCH) {
		close(pidfd);
		close(process_fd);
		return 0;
	}
	if (status != 0) {
		close(pidfd);
		close(process_fd);
		return status;
	}
	struct stat named_identity;
	if (fstatat(proc_fd, name, &named_identity, AT_SYMLINK_NOFOLLOW) != 0) {
		status = errno == ENOENT ? -ESTALE : -errno;
		close(pidfd);
		close(process_fd);
		return status;
	}
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (snapshot_replacement_enabled && pid == snapshot_replacement_pid)
		after.start_time++;
#endif
	if (after.start_time != before.start_time ||
		named_identity.st_dev != opened_identity.st_dev ||
		named_identity.st_ino != opened_identity.st_ino) {
		close(pidfd);
		close(process_fd);
		return -ESTALE;
	}
	if (!process_is_active(&after)) {
		if (result != NULL)
			*result = PROCESS_CAPTURE_INACTIVE;
		close(pidfd);
		close(process_fd);
		return 0;
	}
	status = ledger_add(ledger, pid, &after, pidfd, process_fd);
	if (status != 0) {
		close(pidfd);
		close(process_fd);
	}
	if (status == 0 && result != NULL)
		*result = PROCESS_CAPTURE_RETAINED;
	return status;
}

static int derive_pidfd_budget(const struct rootless_shutdown_policy* policy,
	size_t* budget)
{
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
		return -errno;
	DIR* directory = opendir("/proc/self/fd");
	if (directory == NULL)
		return -errno;
	size_t open_count = 0;
	for (;;) {
		errno = 0;
		struct dirent* entry = readdir(directory);
		if (entry == NULL)
			break;
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
			open_count++;
	}
	int read_error = errno;
	closedir(directory);
	if (read_error != 0)
		return -read_error;
	const rlim_t reserve = 8;
	if (limit.rlim_cur <= reserve ||
		limit.rlim_cur - reserve <= (rlim_t)open_count)
		return -EMFILE;
	rlim_t available = limit.rlim_cur - reserve - (rlim_t)open_count;
	/* Every retained process identity owns both a pidfd and an O_PATH
	 * capability for the exact /proc/PID inode. */
	available /= 2;
	if (available == 0)
		return -EMFILE;
	size_t derived = available > SIZE_MAX ? SIZE_MAX : (size_t)available;
	*budget = policy->pidfd_budget != 0 && policy->pidfd_budget < derived
		? policy->pidfd_budget : derived;
	return 0;
}

static int ledger_contains_all_members(struct process_ledger* ledger,
	const pid_t* members, size_t member_count)
{
	size_t identity = 0;
	for (size_t index = 0; index < member_count; ++index) {
		while (identity < ledger->count &&
			ledger->identities[identity].pid < members[index])
			identity++;
		if (identity == ledger->count ||
			ledger->identities[identity].pid != members[index])
			return 0;
	}
	return 1;
}

/*
 * Acquire one stable, whole-cgroup membership generation. Two sorted
 * cgroup.procs snapshots bound the pidfd acquisition; no per-PID membership
 * reread is performed. Exited identities are compacted every round and the
 * deadline/FD budget apply before any signal is sent.
 */
static int acquire_runtime_closure(
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, size_t pidfd_budget,
	unsigned long long deadline, unsigned retry_interval_ms)
{
	for (;;) {
		int status = closure_named_identity(closure);
		if (status != 0)
			return status;
		unsigned long long now = 0;
		status = monotonic_milliseconds(&now);
		if (status != 0)
			return status;
		if (now >= deadline)
			return -ETIMEDOUT;
		pid_t* before = NULL;
		pid_t* after = NULL;
		size_t before_count = 0;
		size_t after_count = 0;
		status = cgroup_read_pids(closure->directory_fd,
			&before, &before_count, pidfd_budget, deadline);
		if (status != 0)
			goto round_done;
		sort_pid_list(before, before_count);
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		if (snapshot_sorted_checkpoint != NULL)
			snapshot_sorted_checkpoint(1);
		if (membership_checkpoint != NULL)
			membership_checkpoint();
#endif
		status = deadline_not_expired(deadline);
		if (status != 0)
			goto round_done;
		status = ledger_compact(ledger, before, before_count,
			deadline, 1);
		if (status != 0)
			goto round_done;
		status = deadline_not_expired(deadline);
		if (status != 0)
			goto round_done;
		if (before_count > pidfd_budget) {
			status = -EMFILE;
			goto round_done;
		}
		for (size_t index = 0; index < before_count; ++index) {
			status = monotonic_milliseconds(&now);
			if (status != 0 || now >= deadline) {
				if (status == 0)
					status = -ETIMEDOUT;
				goto round_done;
			}
			status = ledger_capture_member(ledger, closure->proc_fd,
				before[index], pidfd_budget, NULL);
			if (status != 0)
				goto round_done;
		}
		status = cgroup_read_pids(closure->directory_fd,
			&after, &after_count, pidfd_budget, deadline);
		if (status != 0)
			goto round_done;
		sort_pid_list(after, after_count);
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		if (snapshot_sorted_checkpoint != NULL)
			snapshot_sorted_checkpoint(2);
#endif
		status = deadline_not_expired(deadline);
		if (status != 0)
			goto round_done;
		status = ledger_compact(ledger, after, after_count,
			deadline, 2);
		if (status != 0)
			goto round_done;
		status = deadline_not_expired(deadline);
		if (status != 0)
			goto round_done;
		int complete = pid_lists_equal(before, before_count,
			after, after_count) &&
			ledger_contains_all_members(ledger, after, after_count);
		if (complete) {
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
			if (containment_checkpoint != NULL)
				containment_checkpoint();
#endif
			status = deadline_not_expired(deadline);
			if (status != 0)
				goto round_done;
			status = 0;
			free(before);
			free(after);
			return 0;
		}
		status = -EAGAIN;
round_done:
		free(before);
		free(after);
		if (status != -EAGAIN)
			return status;
		status = monotonic_milliseconds(&now);
		if (status != 0)
			return status;
		if (now >= deadline)
			return -ETIMEDOUT;
		unsigned long long remaining = deadline - now;
		unsigned delay = retry_interval_ms < remaining
			? retry_interval_ms : (unsigned)remaining;
		if (delay != 0)
			sleep_milliseconds(delay);
	}
}

/*
 * Every rootless runtime process inherits a lifecycle-owned cgroup before the
 * init exec. The kernel's populated bit is the closure barrier: a child forked
 * by the root's final SIGTERM handler is accounted before the root disappears.
 * The ledger preserves every observed (pid,start_time) identity across scan
 * rounds. The host login session and guest process groups are deliberately not
 * authorities and are never signal targets.
 */
static int signal_runtime_closure(pid_t init_process,
	unsigned long long init_start_time,
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, int signal_number, size_t pidfd_budget,
	unsigned long long acquisition_deadline, unsigned retry_interval_ms,
	unsigned* active)
{
	if (closure->backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER)
		return signal_subreaper_closure(init_process, init_start_time,
			closure, ledger, signal_number, pidfd_budget,
			acquisition_deadline, retry_interval_ms, active);
	if (closure->backend != ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED)
		return -EOPNOTSUPP;
	int status = acquire_runtime_closure(closure, ledger,
		pidfd_budget, acquisition_deadline, retry_interval_ms);
	if (status != 0)
		return status;
	unsigned descendants = 0;
	struct process_identity* root = NULL;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	size_t signal_ordinal = 0;
#endif
	*active = 0;
	for (size_t index = 0; index < ledger->count; ++index) {
		struct process_identity* identity = &ledger->identities[index];
		int identity_active = 0;
		status = process_pidfd_active(identity->pidfd, &identity_active);
		if (status != 0)
			return status;
		if (!identity_active)
			continue;
		(*active)++;
		if (identity->pid == init_process && init_start_time != 0 &&
			identity->start_time == init_start_time) {
			root = identity;
			continue;
		}
		descendants++;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		if (signal_checkpoint != NULL)
			signal_checkpoint(signal_number, signal_ordinal + 1);
#endif
		status = deadline_not_expired(acquisition_deadline);
		if (status != 0)
			return status;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		signal_ordinal++;
#endif
		status = signal_process_pidfd(identity->pidfd, signal_number);
		if (status != 0)
			return status;
	}
	if (descendants == 0 && root != NULL) {
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
		if (signal_checkpoint != NULL)
			signal_checkpoint(signal_number, signal_ordinal + 1);
#endif
		status = deadline_not_expired(acquisition_deadline);
		if (status != 0)
			return status;
		status = signal_process_pidfd(root->pidfd, signal_number);
		if (status != 0)
			return status;
	}
	if (*active == 0) {
		int populated = 1;
		status = cgroup_populated_fd(closure->directory_fd, &populated);
		if (status == 0 && populated)
			*active = 1;
	}

	return status;
}

static int monotonic_milliseconds(unsigned long long* milliseconds)
{
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (test_monotonic_clock != NULL)
		return test_monotonic_clock(milliseconds);
#endif
	struct timespec time;
	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		return -errno;
	*milliseconds = (unsigned long long)time.tv_sec * 1000ULL +
		(unsigned long long)time.tv_nsec / 1000000ULL;
	return 0;
}

static void sleep_milliseconds(unsigned milliseconds)
{
	struct timespec delay = {
		.tv_sec = (time_t)(milliseconds / 1000),
		.tv_nsec = (long)(milliseconds % 1000) * 1000000L,
	};
	while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
		;
}

static int quiesce_session_member(struct process_ledger* ledger, pid_t pid,
	const struct process_snapshot* snapshot, unsigned timeout_ms,
	unsigned poll_interval_ms, unsigned* rounds)
{
	if (pid <= 0 || timeout_ms == 0)
		return 0;
	struct process_identity* identity = ledger_find(ledger, pid, snapshot);
	if (identity == NULL)
		return 0;
	unsigned long long now = 0;
	int status = monotonic_milliseconds(&now);
	if (status != 0)
		return status;
	const unsigned long long deadline = now + timeout_ms;
	for (;;) {
		int active = 0;
		status = process_pidfd_active(identity->pidfd, &active);
		if (status != 0 || !active)
			return status;
		(*rounds)++;
		status = monotonic_milliseconds(&now);
		if (status != 0)
			return status;
		if (now >= deadline)
			return -ETIMEDOUT;
		const unsigned remaining = deadline > now
			? (unsigned)(deadline - now) : 0;
		const unsigned delay = poll_interval_ms < remaining
			? poll_interval_ms : remaining;
		if (delay != 0)
			sleep_milliseconds(delay);
	}
}

static int drain_until(pid_t init_process,
	unsigned long long init_start_time,
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, int signal_number,
	unsigned timeout_ms, unsigned poll_interval_ms, size_t pidfd_budget,
	unsigned* rounds)
{
	unsigned long long now = 0;
	int status = monotonic_milliseconds(&now);
	if (status != 0)
		return status;
	unsigned long long deadline = now + timeout_ms;
	for (;;) {
		unsigned active = 0;
		status = signal_runtime_closure(init_process,
			init_start_time, closure, ledger, signal_number,
			pidfd_budget, deadline, poll_interval_ms, &active);
		if (status != 0)
			return status;
		(*rounds)++;
		if (active == 0)
			return 0;
		status = monotonic_milliseconds(&now);
		if (status != 0)
			return status;
		if (now >= deadline)
			return -ETIMEDOUT;
		unsigned remaining = deadline > now
			? (unsigned)(deadline - now) : 0;
		unsigned delay = poll_interval_ms < remaining
			? poll_interval_ms : remaining;
		if (delay != 0)
			sleep_milliseconds(delay);
	}
}

static int bind_runtime_closure(const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	struct rootless_shutdown_closure_capability local =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	struct rootless_shutdown_session_state state;
	int status = load_session_state(prefix, &local, &state);
	if (status == 0) {
		switch (state.backend) {
		case ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED:
			local.backend = state.backend;
			status = open_recorded_cgroup(&state, &local);
			break;
		case ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER:
			status = open_recorded_subreaper(&state, &local);
			break;
		case ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED:
			status = -EOPNOTSUPP;
			break;
		}
	}
	if (status != 0)
		goto fail;
	*capability = local;
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
fail:
	rootless_shutdown_release_closure(&local);
	return shutdown_error(error, error_size, -status,
		"cannot bind rootless shutdown backend: %s", strerror(-status));
}

static int preflight_runtime_endpoints(const darling_runtime_prefix prefix,
	const char* const* endpoints, size_t endpoint_count,
	char* error, size_t error_size)
{
	for (size_t index = 0; index < endpoint_count; ++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix, endpoints[index],
				&status, error, error_size) != 0 && errno != ENOENT)
			return -1;
	}
	return 0;
}

static int require_guest_runtime_endpoints_absent(
	const darling_runtime_prefix prefix, char* error, size_t error_size)
{
	for (size_t index = 0;
		index < sizeof(guest_runtime_endpoints) /
			sizeof(guest_runtime_endpoints[0]); ++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix,
				guest_runtime_endpoints[index], &status,
				error, error_size) == 0) {
			errno = EBUSY;
			if (error != NULL && error_size != 0)
				snprintf(error, error_size,
					"guest runtime endpoint survived graceful shutdown: %s",
					guest_runtime_endpoints[index]);
			return -1;
		}
		if (errno != ENOENT)
			return -1;
	}
	return 0;
}

static int remove_host_runtime_endpoints(const darling_runtime_prefix prefix,
	char* error, size_t error_size)
{
	const size_t endpoint_count = sizeof(host_runtime_endpoints) /
		sizeof(host_runtime_endpoints[0]);
	if (preflight_runtime_endpoints(prefix, host_runtime_endpoints,
			endpoint_count, error, error_size) != 0)
		return -1;
	for (size_t index = 0; index < endpoint_count; ++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix,
				host_runtime_endpoints[index], &status,
				error, error_size) != 0) {
			if (errno == ENOENT)
				continue;
			return -1;
		}
		if (darling_runtime_mode_unlink_relative(prefix,
				host_runtime_endpoints[index],
				0, true, error, error_size) != 0)
			return -1;
	}
	for (size_t index = 0; index < endpoint_count; ++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix,
				host_runtime_endpoints[index], &status,
				error, error_size) == 0) {
			errno = EBUSY;
			if (error != NULL && error_size != 0)
				snprintf(error, error_size,
					"runtime endpoint reappeared during shutdown: %s",
					host_runtime_endpoints[index]);
			return -1;
		}
		if (errno != ENOENT)
			return -1;
	}
	return 0;
}

int shutdown_rootless_runtime(pid_t session_member, pid_t init_process,
	const darling_runtime_prefix prefix,
	const struct rootless_shutdown_policy* requested_policy,
	struct rootless_shutdown_result* result, char* error, size_t error_size)
{
	struct rootless_shutdown_result local = {
		.phase = ROOTLESS_SHUTDOWN_RUNNING,
	};
	struct process_ledger ledger = {0};
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	struct process_snapshot member_snapshot = {0};
	struct process_snapshot init_snapshot = {0};
	int outcome = 0;
	const struct rootless_shutdown_policy* policy = requested_policy != NULL
		? requested_policy : &default_policy;
	const unsigned acquisition_timeout_ms = policy->acquisition_timeout_ms != 0
		? policy->acquisition_timeout_ms
		: default_policy.acquisition_timeout_ms;
	if (result == NULL || prefix == NULL || prefix->directory_fd < 0 ||
		session_member < 0 || init_process <= 0 ||
		policy->poll_interval_ms == 0) {
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown input is invalid");
	}
	*result = local;
	size_t pidfd_budget = 0;
	int status = derive_pidfd_budget(policy, &pidfd_budget);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot establish rootless shutdown pidfd budget: %s",
			strerror(-status));
	int member_present = session_member > 0;
	int init_present = 1;
	status = 0;
	if (member_present) {
		status = process_snapshot_for_pid(session_member, &member_snapshot);
		if (status == -ENOENT || status == -ESRCH)
			member_present = 0;
		else if (status != 0)
			return shutdown_error(error, error_size, -status,
				"cannot inspect rootless session member: %s",
				strerror(-status));
	}
	status = process_snapshot_for_pid(init_process, &init_snapshot);
	if (status == -ENOENT || status == -ESRCH)
		init_present = 0;
	else if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot inspect Darling init process: %s", strerror(-status));
	if (init_present && !process_is_active(&init_snapshot))
		init_present = 0;
	if (member_present && !process_is_active(&member_snapshot))
		member_present = 0;
	local.session = member_present ? member_snapshot.session :
		(init_present ? init_snapshot.session : 0);
	status = bind_runtime_closure(prefix, &closure,
		error, error_size);
	if (status != 0)
		return status;
	local.backend = closure.backend;
	local.anchor_pid = closure.anchor_pid;
	local.anchor_start_time = closure.anchor_start_time;
	struct stat closure_identity;
	int evidence_fd = closure.backend ==
		ROOTLESS_SHUTDOWN_BACKEND_CGROUP_DELEGATED
		? closure.directory_fd : closure.proc_fd;
	if (fstat(evidence_fd, &closure_identity) != 0) {
		outcome = shutdown_error(error, error_size, errno,
			"cannot inspect rootless shutdown capability: %s", strerror(errno));
		goto finish;
	}
	local.closure_inode = closure_identity.st_ino;
	local.capability_device = closure_identity.st_dev;
	local.capability_inode = closure_identity.st_ino;
	if ((init_present &&
		 rootless_shutdown_closure_contains(&closure, init_process,
			error, error_size) != 0) ||
		(member_present &&
		 rootless_shutdown_closure_contains(&closure, session_member,
			error, error_size) != 0)) {
		outcome = -1;
		goto finish;
	}

	for (;;) {
		switch (local.phase) {
		case ROOTLESS_SHUTDOWN_RUNNING:
			local.phase = ROOTLESS_SHUTDOWN_CLOSURE_BOUND;
			break;
		case ROOTLESS_SHUTDOWN_CLOSURE_BOUND: {
			unsigned active = 0;
			unsigned long long now = 0;
			status = monotonic_milliseconds(&now);
			if (status != 0) {
				outcome = shutdown_error(error, error_size, -status,
					"cannot establish closure acquisition deadline: %s",
					strerror(-status));
				goto finish;
			}
			status = signal_runtime_closure(init_process,
				init_present ? init_snapshot.start_time : 0, &closure,
				&ledger, 0, pidfd_budget,
				now + acquisition_timeout_ms, policy->poll_interval_ms, &active);
			local.identities_observed = ledger.observed;
			if (status != 0) {
				outcome = shutdown_error(error, error_size,
					-status,
					"cannot bind rootless shutdown closure: %s",
					strerror(-status));
				goto finish;
			}
			local.phase = active == 0
				? ROOTLESS_SHUTDOWN_DRAINED
				: ROOTLESS_SHUTDOWN_QUIESCING;
			break;
		}
		case ROOTLESS_SHUTDOWN_QUIESCING:
			status = member_present
				? quiesce_session_member(&ledger, session_member,
					&member_snapshot,
					policy->quiesce_timeout_ms,
					policy->poll_interval_ms,
					&local.quiesce_rounds)
				: 0;
			if (status != 0 && status != -ETIMEDOUT) {
				outcome = shutdown_error(error, error_size, -status,
					"cannot observe graceful rootless quiesce: %s",
					strerror(-status));
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_TERM;
			break;
		case ROOTLESS_SHUTDOWN_TERM: {
			unsigned active = 0;
			unsigned long long now = 0;
			status = monotonic_milliseconds(&now);
			if (status != 0) {
				outcome = shutdown_error(error, error_size, -status,
					"cannot establish signal acquisition deadline: %s",
					strerror(-status));
				goto finish;
			}
			status = signal_runtime_closure(init_process,
				init_present ? init_snapshot.start_time : 0, &closure,
				&ledger, SIGTERM, pidfd_budget,
				now + acquisition_timeout_ms, policy->poll_interval_ms, &active);
			local.term_rounds++;
			local.identities_observed = ledger.observed;
			if (status != 0) {
				outcome = shutdown_error(error, error_size, -status,
					"cannot signal rootless session: %s",
					strerror(-status));
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_DRAINING;
			break;
		}
		case ROOTLESS_SHUTDOWN_DRAINING:
			status = drain_until(init_process,
				init_present ? init_snapshot.start_time : 0, &closure,
				&ledger, SIGTERM,
				policy->term_timeout_ms, policy->poll_interval_ms, pidfd_budget,
				&local.term_rounds);
			local.identities_observed = ledger.observed;
			if (status == 0) {
				local.phase = ROOTLESS_SHUTDOWN_DRAINED;
				break;
			}
			if (status != -ETIMEDOUT) {
				outcome = shutdown_error(error, error_size, -status,
					"cannot drain rootless session after SIGTERM: %s",
					strerror(-status));
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_KILL;
			break;
		case ROOTLESS_SHUTDOWN_KILL:
			if (policy->kill_timeout_ms == 0) {
				unsigned active = 0;
				unsigned long long now = 0;
				status = monotonic_milliseconds(&now);
				if (status != 0) {
					outcome = shutdown_error(error, error_size, -status,
						"cannot establish kill acquisition deadline: %s",
						strerror(-status));
					goto finish;
				}
				status = signal_runtime_closure(init_process,
					init_present ? init_snapshot.start_time : 0, &closure,
					&ledger, SIGKILL, pidfd_budget,
					now + acquisition_timeout_ms,
					policy->poll_interval_ms, &active);
				local.kill_rounds++;
				local.identities_observed = ledger.observed;
				if (status != 0) {
					outcome = shutdown_error(error, error_size, -status,
						"cannot signal rootless session: %s",
						strerror(-status));
					goto finish;
				}
				outcome = shutdown_error(error, error_size, ETIMEDOUT,
					"rootless session did not drain before the kill deadline");
				goto finish;
			}
			status = drain_until(init_process,
				init_present ? init_snapshot.start_time : 0, &closure,
				&ledger, SIGKILL,
				policy->kill_timeout_ms, policy->poll_interval_ms, pidfd_budget,
				&local.kill_rounds);
			local.identities_observed = ledger.observed;
			if (status != 0) {
				if (status == -ETIMEDOUT) {
					outcome = shutdown_error(error, error_size, ETIMEDOUT,
						"rootless session did not drain before the kill deadline");
				} else {
					outcome = shutdown_error(error, error_size, -status,
						"cannot drain rootless session after SIGKILL: %s",
						strerror(-status));
				}
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_DRAINED;
			break;
		case ROOTLESS_SHUTDOWN_DRAINED:
			if (require_guest_runtime_endpoints_absent(
					prefix, error, error_size) != 0) {
				outcome = -1;
				(void)rootless_shutdown_cleanup_empty_closure(
					&closure, NULL, 0);
				goto finish;
			}
			if (remove_host_runtime_endpoints(prefix,
					error, error_size) != 0) {
				outcome = -1;
				(void)rootless_shutdown_cleanup_empty_closure(
					&closure, NULL, 0);
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED;
			break;
		case ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED:
			if (rootless_shutdown_cleanup_empty_closure(&closure,
					error, error_size) != 0) {
				outcome = -1;
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_STOPPED;
			break;
		case ROOTLESS_SHUTDOWN_STOPPED:
			if (error != NULL && error_size != 0)
				error[0] = '\0';
			outcome = 0;
			goto finish;
		}
		*result = local;
	}

finish:
	ledger_release(&ledger);
	rootless_shutdown_release_closure(&closure);
	*result = local;
	return outcome;
}
