#include "rootless_shutdown.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct process_snapshot {
	pid_t session;
	unsigned long long start_time;
	char state;
};

struct process_identity {
	pid_t pid;
	unsigned long long start_time;
	int pidfd;
};

struct process_ledger {
	struct process_identity* identities;
	size_t count;
	size_t capacity;
	size_t observed;
};

struct rootless_shutdown_session_state {
	dev_t prefix_device;
	ino_t prefix_inode;
	uid_t owner_uid;
	dev_t cgroup_device;
	ino_t cgroup_inode;
	char cgroup_path[PATH_MAX];
};

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
static pid_t snapshot_replacement_pid = -1;
static int snapshot_replacement_enabled;
static void (*membership_checkpoint)(void);
static void (*membership_read_checkpoint)(size_t);
static void (*snapshot_sorted_checkpoint)(unsigned);
static int (*test_monotonic_clock)(unsigned long long*);
static void (*session_publish_checkpoint)(void);

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
#endif

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
		"DARLING_ROOTLESS_SHUTDOWN_SESSION_V1\n"
		"version=1\n"
		"prefix_device=%" PRIuMAX "\n"
		"prefix_inode=%" PRIuMAX "\n"
		"owner_uid=%" PRIuMAX "\n"
		"cgroup_device=%" PRIuMAX "\n"
		"cgroup_inode=%" PRIuMAX "\n"
		"cgroup_path=%s\n",
		(uintmax_t)state->prefix_device,
		(uintmax_t)state->prefix_inode,
		(uintmax_t)state->owner_uid,
		(uintmax_t)state->cgroup_device,
		(uintmax_t)state->cgroup_inode,
		state->cgroup_path);
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
	char* lines[8] = {0};
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
	if (count != sizeof(lines) / sizeof(lines[0]) ||
		strcmp(lines[0], "DARLING_ROOTLESS_SHUTDOWN_SESSION_V1") != 0 ||
		strcmp(lines[1], "version=1") != 0)
		return -EPROTO;
	uintmax_t prefix_device;
	uintmax_t prefix_inode;
	uintmax_t owner_uid;
	uintmax_t cgroup_device;
	uintmax_t cgroup_inode;
	if (parse_uintmax_line(lines[2], "prefix_device=", &prefix_device) != 0 ||
		parse_uintmax_line(lines[3], "prefix_inode=", &prefix_inode) != 0 ||
		parse_uintmax_line(lines[4], "owner_uid=", &owner_uid) != 0 ||
		parse_uintmax_line(lines[5], "cgroup_device=", &cgroup_device) != 0 ||
		parse_uintmax_line(lines[6], "cgroup_inode=", &cgroup_inode) != 0 ||
		strncmp(lines[7], "cgroup_path=", 12) != 0 ||
		lines[7][12] == '\0' ||
		prefix_device != (uintmax_t)(dev_t)prefix_device ||
		prefix_inode != (uintmax_t)(ino_t)prefix_inode ||
		owner_uid != (uintmax_t)(uid_t)owner_uid ||
		cgroup_device != (uintmax_t)(dev_t)cgroup_device ||
		cgroup_inode != (uintmax_t)(ino_t)cgroup_inode ||
		strlen(lines[7] + 12) >= sizeof(state->cgroup_path) ||
		validate_cgroup_path(lines[7] + 12) != 0)
		return -EPROTO;
	*state = (struct rootless_shutdown_session_state) {
		.prefix_device = (dev_t)prefix_device,
		.prefix_inode = (ino_t)prefix_inode,
		.owner_uid = (uid_t)owner_uid,
		.cgroup_device = (dev_t)cgroup_device,
		.cgroup_inode = (ino_t)cgroup_inode,
	};
	strcpy(state->cgroup_path, lines[7] + 12);
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
	if (capability == NULL || capability->parent_fd < 0 ||
		capability->directory_fd < 0 || capability->leaf[0] == '\0')
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
	return closure_named_identity(capability);
}

int rootless_shutdown_prepare_closure(const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability == NULL || prefix == NULL || prefix->directory_fd < 0 ||
		capability->prefix_fd >= 0 || capability->parent_fd >= 0 ||
		capability->directory_fd >= 0 || capability->membership_fd >= 0 ||
		capability->state_fd >= 0)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure capability is already owned");
	struct rootless_shutdown_closure_capability local =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	int created = 0;
	int state_published = 0;
	int status = rootless_shutdown_pidfd_preflight();
	if (status != 0)
		goto fail;
	local.prefix_fd = duplicate_cloexec(prefix->directory_fd);
	if (local.prefix_fd < 0) {
		status = local.prefix_fd;
		goto fail;
	}
	status = cgroup_leaf_for_prefix(prefix, local.leaf, sizeof(local.leaf));
	if (status != 0)
		goto fail;
	char parent_path[PATH_MAX];
	status = open_delegated_cgroup_parent(&local.parent_fd,
		parent_path, sizeof(parent_path));
	if (status != 0)
		goto fail;
	if (mkdirat(local.parent_fd, local.leaf, 0700) != 0) {
		status = -errno;
		goto fail;
	}
	created = 1;
	if (fchmodat(local.parent_fd, local.leaf, 0700, 0) != 0) {
		status = -errno;
		goto fail;
	}
	int length = snprintf(local.path, sizeof(local.path), "%s/%s",
		parent_path, local.leaf);
	if (length < 0 || (size_t)length >= sizeof(local.path)) {
		status = -ENAMETOOLONG;
		goto fail;
	}
	local.directory_fd = openat(local.parent_fd, local.leaf,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (local.directory_fd < 0) {
		status = -errno;
		goto fail;
	}
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
	if ((cgroup_identity.st_mode & 0777) != 0700 ||
		cgroup_identity.st_uid != geteuid()) {
		status = -EPERM;
		goto fail;
	}
	local.cgroup_device = cgroup_identity.st_dev;
	local.cgroup_inode = cgroup_identity.st_ino;
	local.membership_fd = openat(local.directory_fd, "cgroup.procs",
		O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (local.membership_fd < 0) {
		status = -errno;
		goto fail;
	}
	status = closure_named_identity(&local);
	if (status != 0)
		goto fail;
	struct rootless_shutdown_session_state state = {
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
	*capability = local;
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
fail:
	if (state_published)
		(void)remove_owned_session_state(&local);
	if (created && local.directory_fd >= 0) {
		int populated = 1;
		if (cgroup_populated_fd(local.directory_fd, &populated) == 0 &&
			!populated && local.parent_fd >= 0 &&
			closure_named_identity(&local) == 0)
			(void)unlinkat(local.parent_fd, local.leaf, AT_REMOVEDIR);
	}
	rootless_shutdown_release_closure(&local);
	return shutdown_error(error, error_size, -status,
		"cannot prepare rootless shutdown cgroup: %s", strerror(-status));
}

int rootless_shutdown_enter_closure(
	const struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability == NULL || capability->membership_fd < 0)
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

int rootless_shutdown_closure_contains(
	const struct rootless_shutdown_closure_capability* capability, pid_t pid,
	char* error, size_t error_size)
{
	if (capability == NULL || capability->directory_fd < 0 || pid <= 0)
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
	if (status == 0)
		status = cgroup_read_pids(capability->directory_fd, &members, &count,
			budget, now + default_policy.acquisition_timeout_ms);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot read rootless shutdown cgroup: %s", strerror(-status));
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
	if (capability == NULL || capability->parent_fd < 0 ||
		capability->directory_fd < 0 || capability->leaf[0] == '\0')
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure capability is invalid");
	int status = closure_named_identity(capability);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"rootless shutdown cgroup identity changed");
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

static int process_snapshot_for_pid(pid_t pid, struct process_snapshot* snapshot)
{
	char path[64];
	char stat_line[4096];
	int fd;
	ssize_t length;
	char* fields;
	char* save = NULL;
	char* token;
	unsigned field = 3;
	int found_session = 0;
	int found_start_time = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	length = read(fd, stat_line, sizeof(stat_line) - 1);
	int saved_errno = errno;
	close(fd);
	if (length <= 0) {
		if (length == 0)
			saved_errno = EIO;
		return -saved_errno;
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
	const struct process_snapshot* snapshot, int pidfd)
{
	if (ledger_find(ledger, pid, snapshot) != NULL) {
		close(pidfd);
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
	};
	ledger->count++;
	ledger->observed++;
	return 0;
}

static int ledger_compact(struct process_ledger* ledger,
	const pid_t* members, size_t member_count)
{
	size_t kept = 0;
	size_t member = 0;
	for (size_t index = 0; index < ledger->count; ++index) {
		struct process_identity identity = ledger->identities[index];
		while (member < member_count && members[member] < identity.pid)
			member++;
		int active = 0;
		int status = process_pidfd_active(identity.pidfd, &active);
		if (status != 0)
			return status;
		if (!active || member == member_count ||
			members[member] != identity.pid) {
			close(identity.pidfd);
			continue;
		}
		ledger->identities[kept++] = identity;
	}
	ledger->count = kept;
	return 0;
}

static void ledger_release(struct process_ledger* ledger)
{
	for (size_t index = 0; index < ledger->count; ++index) {
		if (ledger->identities[index].pidfd >= 0)
			close(ledger->identities[index].pidfd);
	}
	free(ledger->identities);
	*ledger = (struct process_ledger){0};
}

/*
 * Capture one cgroup member as an identity-bound capability. The snapshot
 * before pidfd_open detects PID reuse across acquisition; the second cgroup
 * read proves that the exact numeric identity was still a member after the
 * pidfd became authoritative. Once retained, only pidfd_send_signal is used.
 */
static int ledger_capture_member(struct process_ledger* ledger, pid_t pid,
	size_t pidfd_budget)
{
	struct process_snapshot before;
	int status = process_snapshot_for_pid(pid, &before);
	if (status == -ENOENT || status == -ESRCH)
		return 0;
	if (status != 0)
		return status;
	if (!process_is_active(&before))
		return 0;
	if (ledger_find(ledger, pid, &before) != NULL)
		return 0;
	if (ledger->count >= pidfd_budget)
		return -EMFILE;
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (pidfd_open_checkpoint != NULL)
		pidfd_open_checkpoint(pid);
#endif
	int pidfd = open_process_pidfd(pid);
	if (pidfd == -ENOENT || pidfd == -ESRCH)
		return 0;
	if (pidfd < 0)
		return pidfd;
	struct process_snapshot after;
	status = process_snapshot_for_pid(pid, &after);
	if (status == -ENOENT || status == -ESRCH) {
		close(pidfd);
		return 0;
	}
	if (status != 0) {
		close(pidfd);
		return status;
	}
#ifdef DARLING_ROOTLESS_SHUTDOWN_TESTING
	if (snapshot_replacement_enabled && pid == snapshot_replacement_pid)
		after.start_time++;
#endif
	if (after.start_time != before.start_time || !process_is_active(&after)) {
		close(pidfd);
		return 0;
	}
	status = ledger_add(ledger, pid, &after, pidfd);
	if (status != 0)
		close(pidfd);
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
	const rlim_t reserve = 16;
	if (limit.rlim_cur <= reserve ||
		limit.rlim_cur - reserve <= (rlim_t)open_count)
		return -EMFILE;
	rlim_t available = limit.rlim_cur - reserve - (rlim_t)open_count;
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
		status = ledger_compact(ledger, before, before_count);
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
			status = ledger_capture_member(ledger, before[index], pidfd_budget);
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
		status = ledger_compact(ledger, after, after_count);
		if (status != 0)
			goto round_done;
		if (pid_lists_equal(before, before_count, after, after_count) &&
			ledger_contains_all_members(ledger, after, after_count)) {
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
 * authorities; launchd relies on ordinary kill(0) process-group semantics.
 */
static int signal_runtime_closure(pid_t init_process,
	unsigned long long init_start_time,
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, int signal_number, size_t pidfd_budget,
	unsigned long long acquisition_deadline, unsigned retry_interval_ms,
	unsigned* active)
{
	int status = acquire_runtime_closure(closure, ledger,
		pidfd_budget, acquisition_deadline, retry_interval_ms);
	if (status != 0)
		return status;
	unsigned descendants = 0;
	struct process_identity* root = NULL;
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
		status = signal_process_pidfd(identity->pidfd, signal_number);
		if (status != 0)
			return status;
	}
	if (descendants == 0 && root != NULL) {
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
	if (status == 0)
		status = open_recorded_cgroup(&state, &local);
	if (status != 0)
		goto fail;
	*capability = local;
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
fail:
	rootless_shutdown_release_closure(&local);
	return shutdown_error(error, error_size, -status,
		"cannot bind rootless shutdown cgroup: %s", strerror(-status));
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
	struct stat closure_identity;
	if (fstat(closure.directory_fd, &closure_identity) != 0) {
		outcome = shutdown_error(error, error_size, errno,
			"cannot inspect rootless shutdown cgroup: %s", strerror(errno));
		goto finish;
	}
	local.closure_inode = closure_identity.st_ino;
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
