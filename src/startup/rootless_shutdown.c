#include "rootless_shutdown.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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
};

struct process_ledger {
	struct process_identity* identities;
	size_t count;
	size_t capacity;
};

static const struct rootless_shutdown_policy default_policy = {
	.term_timeout_ms = 1000,
	.kill_timeout_ms = 5000,
	.poll_interval_ms = 20,
};

static const char* const runtime_endpoints[] = {
	".init.pid",
	".darlingserver.sock",
	".darlingserver.stat.sock",
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

static int open_delegated_cgroup_parent(int* output_fd)
{
	char path[PATH_MAX];
	int status = cgroup_path_for_pid(0, path, sizeof(path));
	if (status != 0)
		return status;
	int current = open("/sys/fs/cgroup",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (current < 0)
		return -errno;
	int selected = -1;
	char copy[PATH_MAX];
	strcpy(copy, path);
	char* save = NULL;
	for (char* component = strtok_r(copy, "/", &save);
		component != NULL; component = strtok_r(NULL, "/", &save)) {
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
		}
	}
	if (selected < 0)
		status = -EACCES;
	else {
		*output_fd = selected;
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
	if (capability->membership_fd >= 0)
		close(capability->membership_fd);
	if (capability->directory_fd >= 0)
		close(capability->directory_fd);
	if (capability->parent_fd >= 0)
		close(capability->parent_fd);
	*capability = (struct rootless_shutdown_closure_capability)
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
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
		opened.st_uid != geteuid())
		return -ESTALE;
	return 0;
}

int rootless_shutdown_prepare_closure(const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	if (capability == NULL || capability->parent_fd >= 0 ||
		capability->directory_fd >= 0 || capability->membership_fd >= 0)
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown closure capability is already owned");
	struct rootless_shutdown_closure_capability local =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	int status = cgroup_leaf_for_prefix(prefix, local.leaf, sizeof(local.leaf));
	if (status != 0)
		goto fail;
	status = open_delegated_cgroup_parent(&local.parent_fd);
	if (status != 0)
		goto fail;
	if (mkdirat(local.parent_fd, local.leaf, 0700) != 0 && errno != EEXIST) {
		status = -errno;
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
	local.membership_fd = openat(local.directory_fd, "cgroup.procs",
		O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (local.membership_fd < 0) {
		status = -errno;
		goto fail;
	}
	status = closure_named_identity(&local);
	if (status != 0)
		goto fail;
	*capability = local;
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
fail:
	if (local.directory_fd >= 0) {
		int populated = 1;
		if (cgroup_populated_fd(local.directory_fd, &populated) == 0 &&
			!populated && local.parent_fd >= 0)
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

static int cgroup_read_pids(int directory_fd, pid_t** output, size_t* count)
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
	char* line = NULL;
	size_t capacity = 0;
	int status = 0;
	while (getline(&line, &capacity, stream) >= 0) {
		char* end = NULL;
		errno = 0;
		long value = strtol(line, &end, 10);
		if (errno != 0 || value <= 0 || (pid_t)value != value ||
			(end[0] != '\n' && end[0] != '\0') ||
			(end[0] == '\n' && end[1] != '\0')) {
			status = -EPROTO;
			break;
		}
		pid_t* grown = realloc(*output, (*count + 1) * sizeof(**output));
		if (grown == NULL) {
			status = -ENOMEM;
			break;
		}
		*output = grown;
		(*output)[(*count)++] = (pid_t)value;
	}
	if (status == 0 && ferror(stream))
		status = -errno;
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
	int status = cgroup_read_pids(capability->directory_fd, &members, &count);
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

struct process_record {
	pid_t pid;
	struct process_snapshot snapshot;
};

static int read_process_table(struct process_record** records, size_t* count)
{
	DIR* proc = opendir("/proc");
	struct dirent* entry;

	if (proc == NULL)
		return -errno;
	*records = NULL;
	*count = 0;
	int saved_errno = 0;
	for (;;) {
		errno = 0;
		entry = readdir(proc);
		if (entry == NULL) {
			saved_errno = errno;
			break;
		}
		char* end;
		long value;
		pid_t pid;
		struct process_record candidate;

		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;
		errno = 0;
		value = strtol(entry->d_name, &end, 10);
		if (errno != 0 || *end != '\0' || value <= 0 ||
			(pid_t)value != value)
			continue;
		pid = (pid_t)value;
		candidate.pid = pid;
		int status = process_snapshot_for_pid(pid, &candidate.snapshot);
		if (status == -ENOENT || status == -ESRCH)
			continue;
		if (status != 0) {
			free(*records);
			*records = NULL;
			*count = 0;
			closedir(proc);
			return status;
		}
		struct process_record* grown = realloc(*records,
			(*count + 1) * sizeof(**records));
		if (grown == NULL) {
			free(*records);
			*records = NULL;
			*count = 0;
			closedir(proc);
			return -ENOMEM;
		}
		*records = grown;
		(*records)[(*count)++] = candidate;
	}
	closedir(proc);
	if (saved_errno != 0) {
		free(*records);
		*records = NULL;
		*count = 0;
		return -saved_errno;
	}
	return 0;
}

static int signal_identity_bound_process(
	const struct process_record* expected, int signal_number)
{
	struct process_snapshot current;
	int status = process_snapshot_for_pid(expected->pid, &current);
	if (status == -ENOENT || status == -ESRCH)
		return 0;
	if (status != 0)
		return status;
	if (current.start_time != expected->snapshot.start_time ||
		!process_is_active(&current))
		return 0;
	if (signal_number != 0 && kill(expected->pid, signal_number) != 0 &&
		errno != ESRCH)
		return -errno;
	return 0;
}

static int process_matches_identity(const struct process_record* process,
	const struct process_identity* identity)
{
	return process->pid == identity->pid &&
		process->snapshot.start_time == identity->start_time;
}

static int ledger_contains(const struct process_ledger* ledger,
	const struct process_record* process)
{
	for (size_t index = 0; index < ledger->count; ++index) {
		if (process_matches_identity(process, &ledger->identities[index]))
			return 1;
	}
	return 0;
}

static int ledger_add(struct process_ledger* ledger,
	const struct process_record* process)
{
	if (ledger_contains(ledger, process))
		return 0;
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
	ledger->identities[ledger->count++] = (struct process_identity) {
		.pid = process->pid,
		.start_time = process->snapshot.start_time,
	};
	return 0;
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
	struct process_ledger* ledger, int signal_number, unsigned* active)
{
	int status = closure_named_identity(closure);
	if (status != 0)
		return status;
	pid_t* members = NULL;
	size_t member_count = 0;
	status = cgroup_read_pids(closure->directory_fd, &members, &member_count);
	if (status != 0)
		return status;
	struct process_record* records = NULL;
	size_t count = 0;
	status = read_process_table(&records, &count);
	if (status != 0) {
		free(members);
		return status;
	}
	unsigned char* selected = count == 0 ? NULL : calloc(count, 1);
	if (count != 0 && selected == NULL) {
		free(members);
		free(records);
		return -ENOMEM;
	}

	for (size_t index = 0; index < count; ++index) {
		int member = 0;
		for (size_t member_index = 0; member_index < member_count; ++member_index)
			member |= members[member_index] == records[index].pid;
		if (member || ledger_contains(ledger, &records[index]) ||
			(records[index].pid == init_process &&
			 records[index].snapshot.start_time == init_start_time))
			selected[index] = 1;
	}

	unsigned descendants = 0;
	const struct process_record* root = NULL;
	for (size_t index = 0; index < count; ++index) {
		if (!selected[index])
			continue;
		status = ledger_add(ledger, &records[index]);
		if (status != 0)
			goto out;
		if (!process_is_active(&records[index].snapshot))
			continue;
		if (records[index].pid == init_process &&
			records[index].snapshot.start_time == init_start_time) {
			root = &records[index];
			continue;
		}
		descendants++;
		status = signal_identity_bound_process(&records[index], signal_number);
		if (status != 0)
			goto out;
	}
	if (descendants == 0 && root != NULL) {
		status = signal_identity_bound_process(root, signal_number);
		if (status != 0)
			goto out;
	}
	*active = descendants + (root != NULL ? 1U : 0U);
	if (*active == 0) {
		int populated = 1;
		status = cgroup_populated_fd(closure->directory_fd, &populated);
		if (status == 0 && populated)
			*active = 1;
	}

out:
	free(members);
	free(selected);
	free(records);
	return status;
}

static int monotonic_milliseconds(unsigned long long* milliseconds)
{
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

static int drain_until(pid_t init_process,
	unsigned long long init_start_time,
	const struct rootless_shutdown_closure_capability* closure,
	struct process_ledger* ledger, int signal_number,
	unsigned timeout_ms, unsigned poll_interval_ms, unsigned* rounds)
{
	unsigned long long now;
	int status = monotonic_milliseconds(&now);
	if (status != 0)
		return status;
	unsigned long long deadline = now + timeout_ms;
	for (;;) {
		unsigned active = 0;
		status = signal_runtime_closure(init_process,
			init_start_time, closure, ledger, signal_number, &active);
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

static int bind_runtime_closure(pid_t init_process,
	const darling_runtime_prefix prefix,
	struct rootless_shutdown_closure_capability* capability,
	char* error, size_t error_size)
{
	char path[PATH_MAX];
	char expected_leaf[NAME_MAX + 1];
	int status = cgroup_path_for_pid(init_process, path, sizeof(path));
	if (status != 0)
		goto fail_without_capability;
	status = cgroup_leaf_for_prefix(prefix, expected_leaf, sizeof(expected_leaf));
	if (status != 0)
		goto fail_without_capability;
	char copy[PATH_MAX];
	strcpy(copy, path);
	int current = open("/sys/fs/cgroup",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (current < 0) {
		status = -errno;
		goto fail_without_capability;
	}
	struct rootless_shutdown_closure_capability local =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char* save = NULL;
	char* component = strtok_r(copy, "/", &save);
	if (component == NULL) {
		status = -EPROTO;
		goto fail;
	}
	for (;;) {
		char* next_component = strtok_r(NULL, "/", &save);
		if (next_component == NULL) {
			if (strcmp(component, expected_leaf) != 0) {
				status = -EPROTO;
				goto fail;
			}
			local.parent_fd = duplicate_cloexec(current);
			if (local.parent_fd < 0) {
				status = local.parent_fd;
				local.parent_fd = -1;
				goto fail;
			}
			local.directory_fd = openat(current, component,
				O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (local.directory_fd < 0) {
				status = -errno;
				goto fail;
			}
			strcpy(local.leaf, component);
			break;
		}
		int next = openat(current, component,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0) {
			status = -errno;
			goto fail;
		}
		close(current);
		current = next;
		component = next_component;
	}
	struct stat opened;
	struct stat named;
	if (fstat(local.directory_fd, &opened) != 0 ||
		fstatat(local.parent_fd, local.leaf, &named, AT_SYMLINK_NOFOLLOW) != 0) {
		status = -errno;
		goto fail;
	}
	if (!S_ISDIR(opened.st_mode) || !S_ISDIR(named.st_mode) ||
		opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
		opened.st_uid != geteuid()) {
		status = -EPERM;
		goto fail;
	}
	close(current);
	*capability = local;
	if (error != NULL && error_size != 0)
		error[0] = '\0';
	return 0;
fail:
	close(current);
	rootless_shutdown_release_closure(&local);
fail_without_capability:
	return shutdown_error(error, error_size, -status,
		"cannot bind rootless shutdown cgroup: %s", strerror(-status));
}

static int preflight_runtime_endpoints(const darling_runtime_prefix prefix,
	char* error, size_t error_size)
{
	for (size_t index = 0;
		index < sizeof(runtime_endpoints) / sizeof(runtime_endpoints[0]);
		++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix,
				runtime_endpoints[index],
				&status, error, error_size) != 0 && errno != ENOENT)
			return -1;
	}
	return 0;
}

static int remove_runtime_endpoints(const darling_runtime_prefix prefix,
	char* error, size_t error_size)
{
	if (preflight_runtime_endpoints(prefix, error, error_size) != 0)
		return -1;
	for (size_t index = 0;
		index < sizeof(runtime_endpoints) / sizeof(runtime_endpoints[0]);
		++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix,
				runtime_endpoints[index], &status,
				error, error_size) != 0) {
			if (errno == ENOENT)
				continue;
			return -1;
		}
		if (darling_runtime_mode_unlink_relative(prefix,
				runtime_endpoints[index],
				0, true, error, error_size) != 0)
			return -1;
	}
	for (size_t index = 0;
		index < sizeof(runtime_endpoints) / sizeof(runtime_endpoints[0]);
		++index) {
		struct stat status;
		if (darling_runtime_mode_stat_relative(prefix,
				runtime_endpoints[index], &status,
				error, error_size) == 0) {
			errno = EBUSY;
			if (error != NULL && error_size != 0)
				snprintf(error, error_size,
					"runtime endpoint reappeared during shutdown: %s",
					runtime_endpoints[index]);
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
	struct process_snapshot member_snapshot;
	struct process_snapshot init_snapshot;
	int outcome = 0;
	const struct rootless_shutdown_policy* policy = requested_policy != NULL
		? requested_policy : &default_policy;
	if (result == NULL || prefix == NULL || prefix->directory_fd < 0 ||
		session_member <= 0 || init_process <= 0 ||
		policy->poll_interval_ms == 0) {
		return shutdown_error(error, error_size, EINVAL,
			"rootless shutdown input is invalid");
	}
	*result = local;
	int status = process_snapshot_for_pid(session_member, &member_snapshot);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot inspect rootless session member: %s", strerror(-status));
	status = process_snapshot_for_pid(init_process, &init_snapshot);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot inspect Darling init process: %s", strerror(-status));
	local.session = member_snapshot.session;
	status = bind_runtime_closure(init_process, prefix, &closure,
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
	if (!process_is_active(&init_snapshot)) {
		outcome = shutdown_error(error, error_size, ESRCH,
			"Darling init process is no longer active");
		goto finish;
	}
	if (rootless_shutdown_closure_contains(&closure, init_process,
			error, error_size) != 0 ||
		(process_is_active(&member_snapshot) &&
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
			status = signal_runtime_closure(init_process,
				init_snapshot.start_time, &closure,
				&ledger, 0, &active);
			local.identities_observed = ledger.count;
			if (status != 0 || active == 0) {
				outcome = shutdown_error(error, error_size,
					status != 0 ? -status : ESRCH,
					"cannot bind rootless shutdown closure: %s",
					strerror(status != 0 ? -status : ESRCH));
				goto finish;
			}
			local.phase = ROOTLESS_SHUTDOWN_TERM;
			break;
		}
		case ROOTLESS_SHUTDOWN_TERM: {
			unsigned active = 0;
			status = signal_runtime_closure(init_process,
				init_snapshot.start_time, &closure,
				&ledger, SIGTERM, &active);
			local.term_rounds++;
			local.identities_observed = ledger.count;
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
				init_snapshot.start_time, &closure,
				&ledger, SIGTERM,
				policy->term_timeout_ms, policy->poll_interval_ms,
				&local.term_rounds);
			local.identities_observed = ledger.count;
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
				status = signal_runtime_closure(init_process,
					init_snapshot.start_time, &closure,
					&ledger, SIGKILL, &active);
				local.kill_rounds++;
				local.identities_observed = ledger.count;
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
				init_snapshot.start_time, &closure,
				&ledger, SIGKILL,
				policy->kill_timeout_ms, policy->poll_interval_ms,
				&local.kill_rounds);
			local.identities_observed = ledger.count;
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
			if (remove_runtime_endpoints(prefix, error, error_size) != 0) {
				outcome = -1;
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
	free(ledger.identities);
	rootless_shutdown_release_closure(&closure);
	*result = local;
	return outcome;
}
