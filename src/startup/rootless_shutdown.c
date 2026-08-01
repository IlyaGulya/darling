#include "rootless_shutdown.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct process_snapshot {
	pid_t parent;
	pid_t session;
	unsigned long long start_time;
	char state;
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
	int found_parent = 0;
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
		} else if (field == 4) {
			long value = strtol(token, &end, 10);
			if (*token == '\0' || *end != '\0' || value < 0 ||
				(pid_t)value != value)
				return -EINVAL;
			snapshot->parent = (pid_t)value;
			found_parent = 1;
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
	return found_parent && found_session && found_start_time ? 0 : -EINVAL;
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

static const struct process_record* find_process_record(
	const struct process_record* records, size_t count, pid_t pid)
{
	for (size_t index = 0; index < count; ++index) {
		if (records[index].pid == pid)
			return &records[index];
	}
	return NULL;
}

static int process_descends_from(const struct process_record* records,
	size_t count, const struct process_record* process, pid_t root)
{
	pid_t parent = process->snapshot.parent;
	for (size_t depth = 0; depth <= count; ++depth) {
		if (parent == root)
			return 1;
		if (parent <= 1)
			return 0;
		const struct process_record* ancestor = find_process_record(
			records, count, parent);
		if (ancestor == NULL || ancestor->snapshot.parent == parent)
			return 0;
		parent = ancestor->snapshot.parent;
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

/*
 * Darlingserver is the retained rootless child-subreaper.  The host login
 * session is not a runtime capability: launchd can temporarily share it with
 * the caller, so signalling every process with the same SID would kill
 * unrelated host work.  Descendants are drained while the identity-bound
 * subreaper remains alive; only after the tree is empty is the root signalled.
 */
static int signal_runtime_tree(pid_t init_process,
	unsigned long long init_start_time, int signal_number, unsigned* active)
{
	struct process_record* records = NULL;
	size_t count = 0;
	int status = read_process_table(&records, &count);
	if (status != 0)
		return status;
	const struct process_record* root = find_process_record(
		records, count, init_process);
	if (root == NULL || root->snapshot.start_time != init_start_time ||
		!process_is_active(&root->snapshot)) {
		free(records);
		*active = 0;
		return 0;
	}
	unsigned descendants = 0;
	for (size_t index = 0; index < count; ++index) {
		if (records[index].pid == init_process ||
			!process_is_active(&records[index].snapshot) ||
			!process_descends_from(records, count, &records[index], init_process))
			continue;
		descendants++;
		status = signal_identity_bound_process(&records[index], signal_number);
		if (status != 0) {
			free(records);
			return status;
		}
	}
	if (descendants == 0 &&
		(status = signal_identity_bound_process(root, signal_number)) != 0) {
		free(records);
		return status;
	}
	*active = descendants + 1;
	free(records);
	return 0;
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
	unsigned long long init_start_time, int signal_number,
	unsigned timeout_ms, unsigned poll_interval_ms, unsigned* rounds)
{
	unsigned long long now;
	int status = monotonic_milliseconds(&now);
	if (status != 0)
		return status;
	unsigned long long deadline = now + timeout_ms;
	for (;;) {
		unsigned active = 0;
		status = signal_runtime_tree(init_process,
			init_start_time, signal_number, &active);
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
	struct process_snapshot member_snapshot;
	struct process_snapshot init_snapshot;
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
	struct process_record* initial_records = NULL;
	size_t initial_count = 0;
	status = read_process_table(&initial_records, &initial_count);
	if (status != 0)
		return shutdown_error(error, error_size, -status,
			"cannot inspect rootless process tree: %s", strerror(-status));
	const struct process_record* member_record = find_process_record(
		initial_records, initial_count, session_member);
	if (session_member != init_process &&
		(member_record == NULL || !process_descends_from(initial_records,
			initial_count, member_record, init_process))) {
		free(initial_records);
		return shutdown_error(error, error_size, EINVAL,
			"rootless session member is outside the Darling process tree");
	}
	free(initial_records);

	for (;;) {
		switch (local.phase) {
		case ROOTLESS_SHUTDOWN_RUNNING:
			local.phase = ROOTLESS_SHUTDOWN_TERM;
			break;
		case ROOTLESS_SHUTDOWN_TERM: {
			unsigned active = 0;
			status = signal_runtime_tree(init_process,
				init_snapshot.start_time, SIGTERM, &active);
			local.term_rounds++;
			if (status != 0) {
				*result = local;
				return shutdown_error(error, error_size, -status,
					"cannot signal rootless session: %s",
					strerror(-status));
			}
			local.phase = ROOTLESS_SHUTDOWN_DRAINING;
			break;
		}
		case ROOTLESS_SHUTDOWN_DRAINING:
			status = drain_until(init_process,
				init_snapshot.start_time, SIGTERM,
				policy->term_timeout_ms, policy->poll_interval_ms,
				&local.term_rounds);
			if (status == 0) {
				local.phase = ROOTLESS_SHUTDOWN_DRAINED;
				break;
			}
			if (status != -ETIMEDOUT) {
				*result = local;
				return shutdown_error(error, error_size, -status,
					"cannot drain rootless session after SIGTERM: %s",
					strerror(-status));
			}
			local.phase = ROOTLESS_SHUTDOWN_KILL;
			break;
		case ROOTLESS_SHUTDOWN_KILL:
			if (policy->kill_timeout_ms == 0) {
				unsigned active = 0;
				status = signal_runtime_tree(init_process,
					init_snapshot.start_time, SIGKILL, &active);
				local.kill_rounds++;
				*result = local;
				if (status != 0)
					return shutdown_error(error, error_size, -status,
						"cannot signal rootless session: %s",
						strerror(-status));
				return shutdown_error(error, error_size, ETIMEDOUT,
					"rootless session did not drain before the kill deadline");
			}
			status = drain_until(init_process,
				init_snapshot.start_time, SIGKILL,
				policy->kill_timeout_ms, policy->poll_interval_ms,
				&local.kill_rounds);
			if (status != 0) {
				*result = local;
				if (status == -ETIMEDOUT)
					return shutdown_error(error, error_size, ETIMEDOUT,
						"rootless session did not drain before the kill deadline");
				return shutdown_error(error, error_size, -status,
					"cannot drain rootless session after SIGKILL: %s",
					strerror(-status));
			}
			local.phase = ROOTLESS_SHUTDOWN_DRAINED;
			break;
		case ROOTLESS_SHUTDOWN_DRAINED:
			if (remove_runtime_endpoints(prefix, error, error_size) != 0) {
				*result = local;
				return -1;
			}
			local.phase = ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED;
			break;
		case ROOTLESS_SHUTDOWN_ENDPOINTS_REMOVED:
			local.phase = ROOTLESS_SHUTDOWN_STOPPED;
			break;
		case ROOTLESS_SHUTDOWN_STOPPED:
			*result = local;
			if (error != NULL && error_size != 0)
				error[0] = '\0';
			return 0;
		}
		*result = local;
	}
}
