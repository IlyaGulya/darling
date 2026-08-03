#include "../rootless_shutdown.h"
#include "../runtime_mode_prefix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

enum fixture_mode {
	FIXTURE_GRACEFUL,
	FIXTURE_STUBBORN,
	FIXTURE_LATE_FORK,
	FIXTURE_ROOT_LATE_FORK,
	FIXTURE_ZOMBIE_SESSION,
	FIXTURE_INIT_EXITED,
	FIXTURE_PIDFD_EXIT,
	FIXTURE_SESSION_EXITED,
	FIXTURE_DIFFERENT_PARENT,
	FIXTURE_CHURN,
};

struct fixture_payload {
	pid_t leader;
	pid_t worker;
};

struct session_fixture {
	pid_t init;
	pid_t leader;
	pid_t worker;
	int late_pipe;
	int churn_command;
	int churn_reply;
};

static void cleanup_fixture(struct session_fixture* fixture);

static int late_fork_pipe = -1;
static pid_t pidfd_exit_target = -1;
static int pidfd_exit_reaped;
static volatile sig_atomic_t proc_root_scan_attempts;
static int churn_command_fd = -1;
static int churn_reply_fd = -1;
static pid_t churn_latest_pid = -1;
static int churn_checkpoint_failed;
static unsigned long long test_monotonic_now;
static size_t membership_read_count;
static size_t membership_read_advance_at;
static unsigned snapshot_sorted_count;
static unsigned snapshot_sorted_advance_at;
static unsigned compact_checkpoint_snapshot;
static size_t compact_checkpoint_index;
static int containment_advance_deadline;
static int signal_checkpoint_number;
static size_t signal_checkpoint_count;
static size_t signal_checkpoint_advance_at;
static int publish_swap_prefix_fd = -1;
static int publish_swap_performed;
static int cgroup_checkpoint_parent_fd = -1;
static char cgroup_checkpoint_leaf[NAME_MAX + 1];
static int cgroup_swap_phase;
static int cgroup_swap_performed;
static dev_t cgroup_replacement_device;
static ino_t cgroup_replacement_inode;
static unsigned cgroup_event_churn_count;
static unsigned cgroup_event_churn_generated;
static size_t prebind_drain_event_count;
static size_t prebind_drain_advance_at;

extern DIR* __real_opendir(const char* path);

DIR* __wrap_opendir(const char* path)
{
	if (strcmp(path, "/proc") == 0) {
		proc_root_scan_attempts++;
		errno = EACCES;
		return NULL;
	}
	return __real_opendir(path);
}

static void exit_at_pidfd_open(pid_t pid)
{
	if (pid != pidfd_exit_target)
		return;
	pidfd_exit_target = -1;
	if (kill(pid, SIGKILL) == 0 && waitpid(pid, NULL, 0) == pid)
		pidfd_exit_reaped = 1;
}

static int test_monotonic_clock(unsigned long long* milliseconds)
{
	*milliseconds = test_monotonic_now;
	return 0;
}

static void observe_membership_read(size_t count)
{
	membership_read_count = count;
	if (membership_read_advance_at != 0 &&
		count >= membership_read_advance_at)
		test_monotonic_now += 2;
}

static void observe_snapshot_sorted(unsigned ordinal)
{
	snapshot_sorted_count = ordinal;
	if (snapshot_sorted_advance_at != 0 &&
		ordinal >= snapshot_sorted_advance_at)
		test_monotonic_now += 2;
}

static void observe_ledger_compact(unsigned snapshot, size_t index)
{
	if (snapshot == compact_checkpoint_snapshot &&
		index >= compact_checkpoint_index)
		test_monotonic_now += 2;
}

static void observe_containment(void)
{
	if (containment_advance_deadline)
		test_monotonic_now += 2;
}

static void observe_signal_iteration(int signal_number, size_t index)
{
	if (signal_number != signal_checkpoint_number)
		return;
	signal_checkpoint_count = index;
	if (signal_checkpoint_advance_at != 0 &&
		index >= signal_checkpoint_advance_at)
		test_monotonic_now += 2;
}

static void capture_or_swap_created_cgroup(
	unsigned phase, int parent_fd, const char* leaf)
{
	if (cgroup_checkpoint_parent_fd < 0)
		cgroup_checkpoint_parent_fd = fcntl(
			parent_fd, F_DUPFD_CLOEXEC, 3);
	if (strlen(leaf) < sizeof(cgroup_checkpoint_leaf))
		strcpy(cgroup_checkpoint_leaf, leaf);
	if (phase == ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_CAPABILITY_OPEN &&
		cgroup_event_churn_count != 0) {
		for (unsigned index = 0; index < cgroup_event_churn_count; ++index) {
			char noise[NAME_MAX + 1];
			int length = snprintf(noise, sizeof(noise),
				"darling-prebind-noise-%ld-%u", (long)getpid(), index);
			if (length <= 0 || (size_t)length >= sizeof(noise) ||
				mkdirat(parent_fd, noise, 0700) != 0)
				break;
			cgroup_event_churn_generated++;
			if (unlinkat(parent_fd, noise, AT_REMOVEDIR) != 0)
				break;
			cgroup_event_churn_generated++;
		}
	}
	if ((int)phase != cgroup_swap_phase || cgroup_swap_performed)
		return;
	if (unlinkat(parent_fd, leaf, AT_REMOVEDIR) != 0 ||
		mkdirat(parent_fd, leaf, 0700) != 0)
		return;
	struct stat replacement;
	if (fstatat(parent_fd, leaf, &replacement,
			AT_SYMLINK_NOFOLLOW) != 0)
		return;
	cgroup_replacement_device = replacement.st_dev;
	cgroup_replacement_inode = replacement.st_ino;
	cgroup_swap_performed = 1;
}

static void advance_prebind_drain(size_t event_count)
{
	prebind_drain_event_count = event_count;
	if (prebind_drain_advance_at != 0 &&
		event_count >= prebind_drain_advance_at)
		test_monotonic_now += 1000;
}

static void reset_cgroup_create_fixture(void)
{
	rootless_shutdown_test_set_cgroup_create_checkpoint(NULL);
	rootless_shutdown_test_set_cgroup_create_error(0, 0);
	rootless_shutdown_test_set_prebind_event_budget(0);
	rootless_shutdown_test_set_prebind_event_checkpoint(NULL);
	if (cgroup_checkpoint_parent_fd >= 0) {
		if (cgroup_checkpoint_leaf[0] != '\0')
			(void)unlinkat(cgroup_checkpoint_parent_fd,
				cgroup_checkpoint_leaf, AT_REMOVEDIR);
		close(cgroup_checkpoint_parent_fd);
	}
	cgroup_checkpoint_parent_fd = -1;
	cgroup_checkpoint_leaf[0] = '\0';
	cgroup_swap_phase = 0;
	cgroup_swap_performed = 0;
	cgroup_replacement_device = 0;
	cgroup_replacement_inode = 0;
	cgroup_event_churn_count = 0;
	cgroup_event_churn_generated = 0;
	prebind_drain_event_count = 0;
	prebind_drain_advance_at = 0;
}

static void replace_published_session_state(void)
{
	char content[8192];
	int source = openat(publish_swap_prefix_fd,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (source < 0)
		return;
	ssize_t length = read(source, content, sizeof(content));
	close(source);
	if (length <= 0)
		return;
	const char* replacement = ".rootless-shutdown-session-v1.replacement";
	(void)unlinkat(publish_swap_prefix_fd, replacement, 0);
	int target = openat(publish_swap_prefix_fd, replacement,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (target < 0)
		return;
	int valid = fchmod(target, 0600) == 0 &&
		write(target, content, (size_t)length) == length && fsync(target) == 0;
	close(target);
	if (!valid)
		return;
	if (unlinkat(publish_swap_prefix_fd,
			ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, 0) == 0 &&
		renameat(publish_swap_prefix_fd, replacement,
			publish_swap_prefix_fd,
			ROOTLESS_SHUTDOWN_SESSION_STATE_NAME) == 0) {
		(void)fsync(publish_swap_prefix_fd);
		publish_swap_performed = 1;
	}
}

static void late_fork_handler(int signal_number)
{
	(void)signal_number;
	pid_t child = fork();
	if (child == 0) {
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	}
	if (child > 0)
		if (write(late_fork_pipe, &child, sizeof(child)) < 0)
			_exit(16);
	_exit(0);
}

static int write_file(const char* path, const char* content)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	size_t length = strlen(content);
	ssize_t written = write(fd, content, length);
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return written == (ssize_t)length ? 0 : -1;
}

static int bind_socket(const char* path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(address.sun_path)) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(address.sun_path, path);
	int result = bind(fd, (const struct sockaddr*)&address, sizeof(address));
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return result;
}

static int prepare_endpoint_directories(const char* prefix)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/var", prefix);
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return -1;
	snprintf(path, sizeof(path), "%s/var/run", prefix);
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return -1;
	snprintf(path, sizeof(path), "%s/var/tmp", prefix);
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return -1;
	snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix);
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return -1;

	return 0;
}

static int prepare_endpoints(const char* prefix, int guest_owned)
{
	if (prepare_endpoint_directories(prefix) != 0)
		return -1;
	char path[512];
	snprintf(path, sizeof(path), "%s/.init.pid", prefix);
	if (write_file(path, "123\n") != 0)
		return -1;
	static const char* sockets[] = {
		".darlingserver.sock",
		".darlingserver.stat.sock",
	};
	for (size_t index = 0; index < sizeof(sockets) / sizeof(sockets[0]); ++index) {
		snprintf(path, sizeof(path), "%s/%s", prefix, sockets[index]);
		if (bind_socket(path) != 0)
			return -1;
	}
	if (guest_owned) {
		static const char* guest_sockets[] = {
			"var/run/shellspawn.sock",
			"var/tmp/launchd/sock",
		};
		for (size_t index = 0;
			index < sizeof(guest_sockets) / sizeof(guest_sockets[0]);
			++index) {
			snprintf(path, sizeof(path), "%s/%s", prefix,
				guest_sockets[index]);
			if (bind_socket(path) != 0)
				return -1;
		}
	}
	return 0;
}

static int endpoint_exists(const char* prefix, const char* relative)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", prefix, relative);
	struct stat status;
	return lstat(path, &status) == 0;
}

static int endpoints_removed(const char* prefix)
{
	static const char* endpoints[] = {
		".init.pid",
		".darlingserver.sock",
		".darlingserver.stat.sock",
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME,
		"var/run/shellspawn.sock",
		"var/tmp/launchd/sock",
	};
	for (size_t index = 0;
		index < sizeof(endpoints) / sizeof(endpoints[0]);
		++index) {
		if (endpoint_exists(prefix, endpoints[index]))
			return 0;
	}
	return 1;
}

static int read_exact(int fd, void* output, size_t size)
{
	char* cursor = output;
	while (size != 0) {
		ssize_t count = read(fd, cursor, size);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		cursor += count;
		size -= (size_t)count;
	}
	return 0;
}

static int count_open_fds(void)
{
	DIR* directory = opendir("/proc/self/fd");
	if (directory == NULL)
		return -1;
	int count = 0;
	for (;;) {
		errno = 0;
		struct dirent* entry = readdir(directory);
		if (entry == NULL)
			break;
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
			count++;
	}
	int saved_errno = errno;
	closedir(directory);
	return saved_errno == 0 ? count : -1;
}

static void advance_membership_churn(void)
{
	char command = 'F';
	pid_t child = -1;
	if (churn_command_fd < 0 || churn_reply_fd < 0 ||
		write(churn_command_fd, &command, sizeof(command)) != sizeof(command) ||
		read_exact(churn_reply_fd, &child, sizeof(child)) != 0 || child <= 0) {
		churn_checkpoint_failed = 1;
		return;
	}
	churn_latest_pid = child;
}

static struct session_fixture spawn_fixture(
	enum fixture_mode mode,
	darling_runtime_prefix prefix
)
{
	int pipefd[2];
	int churn_commands[2] = {-1, -1};
	int churn_replies[2] = {-1, -1};
	struct session_fixture fixture = { .init = -1, .leader = -1,
		.worker = -1, .late_pipe = -1,
		.churn_command = -1, .churn_reply = -1 };
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[256] = {0};
	if (rootless_shutdown_prepare_closure(prefix, &closure,
			error, sizeof(error)) != 0) {
		fprintf(stderr, "fixture closure prepare failed: %s\n", error);
		return fixture;
	}
	if (pipe(pipefd) != 0 ||
		(mode == FIXTURE_CHURN &&
		 (pipe(churn_commands) != 0 || pipe(churn_replies) != 0))) {
		(void)rootless_shutdown_cleanup_empty_closure(&closure, NULL, 0);
		return fixture;
	}
	fixture.init = rootless_shutdown_fork_runtime(&closure,
		error, sizeof(error));
	if (fixture.init < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		(void)rootless_shutdown_cleanup_empty_closure(&closure, NULL, 0);
		return fixture;
	}
	if (fixture.init == 0) {
		close(pipefd[0]);
		if (mode == FIXTURE_CHURN) {
			close(churn_commands[1]);
			close(churn_replies[0]);
		}
		rootless_shutdown_release_closure(&closure);
		if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
			_exit(9);
		if (mode == FIXTURE_ROOT_LATE_FORK) {
			late_fork_pipe = pipefd[1];
			struct sigaction action = {0};
			action.sa_handler = late_fork_handler;
			sigemptyset(&action.sa_mask);
			if (sigaction(SIGTERM, &action, NULL) != 0)
				_exit(15);
		} else if (mode != FIXTURE_GRACEFUL &&
			mode != FIXTURE_ZOMBIE_SESSION &&
			mode != FIXTURE_INIT_EXITED &&
			mode != FIXTURE_PIDFD_EXIT &&
			mode != FIXTURE_SESSION_EXITED &&
			mode != FIXTURE_DIFFERENT_PARENT) {
			signal(SIGTERM, SIG_IGN);
		}
		pid_t leader = fork();
		if (leader < 0)
			_exit(10);
		if (leader == 0) {
			if (setsid() < 0)
				_exit(11);
			if (mode == FIXTURE_STUBBORN)
				signal(SIGTERM, SIG_IGN);
			if (mode == FIXTURE_LATE_FORK) {
				late_fork_pipe = pipefd[1];
				struct sigaction action = {0};
				action.sa_handler = late_fork_handler;
				sigemptyset(&action.sa_mask);
				if (sigaction(SIGTERM, &action, NULL) != 0)
					_exit(12);
			}
			pid_t worker = fork();
			if (worker < 0)
				_exit(13);
			if (worker == 0) {
				if (mode == FIXTURE_STUBBORN)
					signal(SIGTERM, SIG_IGN);
				for (;;)
					pause();
			}
			struct fixture_payload payload = {
				.leader = getpid(),
				.worker = worker,
			};
			if (write(pipefd[1], &payload, sizeof(payload)) != sizeof(payload))
				_exit(14);
			if (mode == FIXTURE_ZOMBIE_SESSION)
				_exit(0);
			if (mode == FIXTURE_SESSION_EXITED)
				_exit(0);
			if (mode == FIXTURE_CHURN) {
				pid_t churn_child = -1;
				for (;;) {
					char command;
					if (read_exact(churn_commands[0],
							&command, sizeof(command)) != 0)
						_exit(17);
					if (churn_child > 0) {
						(void)kill(churn_child, SIGKILL);
						(void)waitpid(churn_child, NULL, 0);
					}
					churn_child = fork();
					if (churn_child == 0) {
						signal(SIGTERM, SIG_IGN);
						for (;;)
							pause();
					}
					if (churn_child < 0 ||
						write(churn_replies[1], &churn_child,
							sizeof(churn_child)) != sizeof(churn_child))
						_exit(18);
				}
			}
			for (;;)
				pause();
		}
		if (mode == FIXTURE_SESSION_EXITED)
			(void)waitpid(leader, NULL, 0);
		if (mode == FIXTURE_INIT_EXITED)
			_exit(0);
		for (;;)
			pause();
	}
	close(pipefd[1]);
	if (mode == FIXTURE_CHURN) {
		close(churn_commands[0]);
		close(churn_replies[1]);
		fixture.churn_command = churn_commands[1];
		fixture.churn_reply = churn_replies[0];
	}
	struct fixture_payload payload;
	if (read_exact(pipefd[0], &payload, sizeof(payload)) != 0) {
		close(pipefd[0]);
		cleanup_fixture(&fixture);
		(void)rootless_shutdown_cleanup_empty_closure(&closure, NULL, 0);
		return fixture;
	}
	fixture.leader = payload.leader;
	fixture.worker = payload.worker;
	fixture.late_pipe =
		mode == FIXTURE_LATE_FORK || mode == FIXTURE_ROOT_LATE_FORK
			? pipefd[0] : -1;
	if (fixture.late_pipe < 0)
		close(pipefd[0]);
	if (mode != FIXTURE_INIT_EXITED &&
		rootless_shutdown_closure_contains(&closure, fixture.init,
			error, sizeof(error)) != 0) {
		fprintf(stderr, "fixture closure membership failed: %s\n", error);
		cleanup_fixture(&fixture);
		(void)rootless_shutdown_cleanup_empty_closure(&closure, NULL, 0);
		fixture.init = -1;
		return fixture;
	}
	rootless_shutdown_release_closure(&closure);
	return fixture;
}

static void cleanup_fixture(struct session_fixture* fixture)
{
	if (fixture->worker > 0)
		(void)kill(fixture->worker, SIGKILL);
	if (fixture->leader > 0)
		(void)kill(fixture->leader, SIGKILL);
	if (fixture->init > 0) {
		(void)kill(fixture->init, SIGKILL);
		(void)waitpid(fixture->init, NULL, 0);
	}
	if (fixture->late_pipe >= 0)
		close(fixture->late_pipe);
	if (fixture->churn_command >= 0)
		close(fixture->churn_command);
	if (fixture->churn_reply >= 0)
		close(fixture->churn_reply);
}

static int wait_pid_gone(pid_t pid)
{
	for (int attempt = 0; attempt < 500; ++attempt) {
		if (kill(pid, 0) != 0 && errno == ESRCH)
			return 0;
		usleep(10000);
	}
	return -1;
}

static int wait_pid_state(pid_t pid, char expected)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	for (int attempt = 0; attempt < 500; ++attempt) {
		FILE* file = fopen(path, "r");
		if (file != NULL) {
			char line[4096];
			if (fgets(line, sizeof(line), file) != NULL) {
				char* fields = strrchr(line, ')');
				if (fields != NULL && fields[1] == ' ' && fields[2] == expected) {
					fclose(file);
					return 0;
				}
			}
			fclose(file);
		}
		usleep(10000);
	}
	return -1;
}

static int run_shutdown_case(
	const char* prefix_path,
	darling_runtime_prefix prefix,
	enum fixture_mode mode,
	int expect_kill
)
{
	if (prepare_endpoints(prefix_path, 0) != 0)
		return 20;
	struct session_fixture fixture = spawn_fixture(mode, prefix);
	if (fixture.init <= 0 || fixture.leader <= 0 || fixture.worker <= 0)
		return 21;
	pid_t shutdown_init = fixture.init;
	if (mode == FIXTURE_INIT_EXITED) {
		if (waitpid(fixture.init, NULL, 0) != fixture.init) {
			cleanup_fixture(&fixture);
			return 22;
		}
		fixture.init = -1;
	}
	if (mode == FIXTURE_ZOMBIE_SESSION &&
		wait_pid_state(fixture.leader, 'Z') != 0) {
		cleanup_fixture(&fixture);
		return 22;
	}
	if (mode == FIXTURE_SESSION_EXITED &&
		wait_pid_gone(fixture.leader) != 0) {
		cleanup_fixture(&fixture);
		return 22;
	}
	if (mode == FIXTURE_PIDFD_EXIT) {
		pidfd_exit_target = fixture.init;
		pidfd_exit_reaped = 0;
		rootless_shutdown_test_set_pidfd_open_checkpoint(exit_at_pidfd_open);
	}
	if (mode == FIXTURE_DIFFERENT_PARENT)
		rootless_shutdown_test_set_parent_lookup_error(EACCES);
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(
		fixture.leader,
		shutdown_init,
		prefix,
		NULL,
		&result,
		error,
		sizeof(error));
	if (mode == FIXTURE_PIDFD_EXIT) {
		rootless_shutdown_test_set_pidfd_open_checkpoint(NULL);
		pidfd_exit_target = -1;
		if (pidfd_exit_reaped)
			fixture.init = -1;
	}
	if (mode == FIXTURE_DIFFERENT_PARENT)
		rootless_shutdown_test_set_parent_lookup_error(0);
	pid_t late_pid = -1;
	if (mode == FIXTURE_LATE_FORK || mode == FIXTURE_ROOT_LATE_FORK) {
		struct pollfd pollfd = { .fd = fixture.late_pipe, .events = POLLIN };
		if (poll(&pollfd, 1, 2000) <= 0 ||
			read_exact(fixture.late_pipe, &late_pid, sizeof(late_pid)) != 0) {
			cleanup_fixture(&fixture);
			return 23;
		}
	}
	if (rc != 0 || result.phase != ROOTLESS_SHUTDOWN_STOPPED ||
		!!result.kill_rounds != !!expect_kill || !endpoints_removed(prefix_path) ||
		result.closure_inode == 0 ||
		(mode == FIXTURE_PIDFD_EXIT && !pidfd_exit_reaped) ||
		(mode == FIXTURE_ROOT_LATE_FORK && result.identities_observed < 4) ||
		(late_pid > 0 && wait_pid_gone(late_pid) != 0)) {
		fprintf(stderr, "shutdown case failed mode=%d rc=%d phase=%d "
			"term=%u kill=%u late_pid=%d late_alive=%d error=%s\n",
			mode, rc, result.phase, result.term_rounds, result.kill_rounds,
			(int)late_pid, late_pid > 0 && kill(late_pid, 0) == 0, error);
		if (late_pid > 0)
			(void)kill(late_pid, SIGKILL);
		cleanup_fixture(&fixture);
		return 22;
	}
	if (fixture.init > 0) {
		(void)waitpid(fixture.init, NULL, 0);
		fixture.init = -1;
	}
	if (wait_pid_gone(fixture.leader) != 0 ||
		wait_pid_gone(fixture.worker) != 0 ||
		(late_pid > 0 && wait_pid_gone(late_pid) != 0)) {
		cleanup_fixture(&fixture);
		return 24;
	}
	cleanup_fixture(&fixture);
	return 0;
}

static int preflight_before_mutation_case(
	const char* prefix_path, darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[256] = {0};
	rootless_shutdown_test_set_pidfd_preflight_error(ENOSYS);
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	rootless_shutdown_test_set_pidfd_preflight_error(0);
	int state_exists = endpoint_exists(prefix_path,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME);
	rootless_shutdown_release_closure(&closure);
	if (rc != -ENOSYS || state_exists) {
		fprintf(stderr, "pidfd preflight contract failed rc=%d state=%d error=%s\n",
			rc, state_exists, error);
		return 1;
	}
	return 0;
}

static int cgroup_same_name_aba_case(
	const char* prefix_path, darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	if (rootless_shutdown_prepare_closure(prefix, &closure,
			error, sizeof(error)) != 0)
		return 1;
	ino_t original = closure.cgroup_inode;
	if (closure.membership_fd >= 0) {
		close(closure.membership_fd);
		closure.membership_fd = -1;
	}
	if (unlinkat(closure.parent_fd, closure.leaf, AT_REMOVEDIR) != 0 ||
		mkdirat(closure.parent_fd, closure.leaf, 0700) != 0) {
		rootless_shutdown_release_closure(&closure);
		return 2;
	}
	struct stat replacement;
	if (fstatat(closure.parent_fd, closure.leaf, &replacement,
			AT_SYMLINK_NOFOLLOW) != 0 || replacement.st_ino == original) {
		rootless_shutdown_release_closure(&closure);
		return 3;
	}
	struct rootless_shutdown_result result;
	int rc = shutdown_rootless_runtime(0, getpid(), prefix, NULL,
		&result, error, sizeof(error));
	int replacement_survived = fstatat(closure.parent_fd, closure.leaf,
		&replacement, AT_SYMLINK_NOFOLLOW) == 0;
	(void)unlinkat(closure.parent_fd, closure.leaf, AT_REMOVEDIR);
	(void)unlinkat(prefix->directory_fd,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, 0);
	(void)fsync(prefix->directory_fd);
	rootless_shutdown_release_closure(&closure);
	if (rc == 0 || !replacement_survived ||
		endpoint_exists(prefix_path, ROOTLESS_SHUTDOWN_SESSION_STATE_NAME)) {
		fprintf(stderr, "cgroup ABA contract failed rc=%d survived=%d error=%s\n",
			rc, replacement_survived, error);
		return 4;
	}
	return 0;
}

static int missing_session_identity_case(darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	if (rootless_shutdown_prepare_closure(prefix, &closure,
			error, sizeof(error)) != 0)
		return 1;
	if (unlinkat(prefix->directory_fd,
			ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, 0) != 0)
		return 2;
	struct rootless_shutdown_result result;
	int rc = shutdown_rootless_runtime(0, getpid(), prefix, NULL,
		&result, error, sizeof(error));
	struct stat cgroup;
	int cgroup_survived = fstatat(closure.parent_fd, closure.leaf,
		&cgroup, AT_SYMLINK_NOFOLLOW) == 0;
	if (closure.membership_fd >= 0) {
		close(closure.membership_fd);
		closure.membership_fd = -1;
	}
	(void)unlinkat(closure.parent_fd, closure.leaf, AT_REMOVEDIR);
	rootless_shutdown_release_closure(&closure);
	if (rc == 0 || !cgroup_survived) {
		fprintf(stderr, "missing session identity contract failed rc=%d "
			"cgroup_survived=%d error=%s\n", rc, cgroup_survived, error);
		return 3;
	}
	return 0;
}

static int finish_fixture_after_failed_acquisition(
	const char* prefix_path, darling_runtime_prefix prefix,
	struct session_fixture* fixture)
{
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture->leader, fixture->init,
		prefix, NULL, &result, error, sizeof(error));
	if (rc != 0 || result.phase != ROOTLESS_SHUTDOWN_STOPPED ||
		!endpoints_removed(prefix_path)) {
		fprintf(stderr, "failed-acquisition cleanup failed rc=%d phase=%d %s\n",
			rc, result.phase, error);
		cleanup_fixture(fixture);
		return 1;
	}
	(void)waitpid(fixture->init, NULL, 0);
	fixture->init = -1;
	cleanup_fixture(fixture);
	return 0;
}

static int acquisition_failure_case(
	const char* prefix_path, darling_runtime_prefix prefix,
	int replacement, int churn, size_t pidfd_budget)
{
	int fd_baseline = count_open_fds();
	if (fd_baseline < 0)
		return 1;
	if (prepare_endpoints(prefix_path, 0) != 0)
		return 2;
	struct session_fixture fixture = spawn_fixture(
		churn ? FIXTURE_CHURN : FIXTURE_GRACEFUL, prefix);
	if (fixture.init <= 0 || fixture.leader <= 0 || fixture.worker <= 0)
		return 3;
	if (replacement)
		rootless_shutdown_test_set_snapshot_replacement(fixture.worker, 1);
	if (churn) {
		churn_command_fd = fixture.churn_command;
		churn_reply_fd = fixture.churn_reply;
		churn_latest_pid = -1;
		churn_checkpoint_failed = 0;
		rootless_shutdown_test_set_membership_checkpoint(
			advance_membership_churn);
	}
	const struct rootless_shutdown_policy policy = {
		.acquisition_timeout_ms = 80,
		.pidfd_budget = pidfd_budget,
		.term_timeout_ms = 100,
		.kill_timeout_ms = 100,
		.poll_interval_ms = 1,
	};
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.leader, fixture.init,
		prefix, &policy, &result, error, sizeof(error));
	rootless_shutdown_test_set_snapshot_replacement(-1, 0);
	rootless_shutdown_test_set_membership_checkpoint(NULL);
	churn_command_fd = -1;
	churn_reply_fd = -1;
	int all_survived = kill(fixture.init, 0) == 0 &&
		kill(fixture.leader, 0) == 0 && kill(fixture.worker, 0) == 0;
	int expected = replacement ? -ESTALE :
		(pidfd_budget != 0 ? -EMFILE : -ETIMEDOUT);
	if (rc != expected || result.phase != ROOTLESS_SHUTDOWN_CLOSURE_BOUND ||
		!all_survived || (churn &&
		 (churn_checkpoint_failed || churn_latest_pid <= 0))) {
		fprintf(stderr, "acquisition failure contract failed replacement=%d "
			"churn=%d budget=%zu rc=%d phase=%d survived=%d error=%s\n",
			replacement, churn, pidfd_budget, rc, result.phase,
			all_survived, error);
		cleanup_fixture(&fixture);
		return 4;
	}
	if (finish_fixture_after_failed_acquisition(
			prefix_path, prefix, &fixture) != 0)
		return 5;
	int fd_post = count_open_fds();
	if (fd_post != fd_baseline) {
		fprintf(stderr, "pidfd compaction leak baseline=%d post=%d\n",
			fd_baseline, fd_post);
		return 6;
	}
	return 0;
}

static int low_rlimit_case(
	const char* prefix_path, darling_runtime_prefix prefix)
{
	struct rlimit before;
	if (getrlimit(RLIMIT_NOFILE, &before) != 0)
		return 1;
	struct rlimit limited = before;
	if (limited.rlim_cur > 32)
		limited.rlim_cur = 32;
	if (setrlimit(RLIMIT_NOFILE, &limited) != 0)
		return 2;
	int rc = run_shutdown_case(prefix_path, prefix, FIXTURE_GRACEFUL, 0);
	int saved_errno = errno;
	int restore = setrlimit(RLIMIT_NOFILE, &before);
	errno = saved_errno;
	return rc == 0 && restore == 0 ? 0 : 3;
}

static int hostile_umask_cgroup_case(darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	mode_t previous = umask(0777);
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	int saved_errno = errno;
	umask(previous);
	errno = saved_errno;
	struct stat state = {0};
	int mode_ok = rc == 0 && fstat(closure.directory_fd, &state) == 0 &&
		(state.st_mode & 0777) == 0700;
	if (rc == 0)
		rc = rootless_shutdown_cleanup_empty_closure(&closure,
			error, sizeof(error));
	rootless_shutdown_release_closure(&closure);
	if (!mode_ok || rc != 0) {
		fprintf(stderr, "hostile umask cgroup contract failed rc=%d mode=%04o %s\n",
			rc, (unsigned)(state.st_mode & 0777), error);
		return 1;
	}
	return 0;
}

static int session_publication_swap_case(
	const char* prefix_path, darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	publish_swap_prefix_fd = prefix->directory_fd;
	publish_swap_performed = 0;
	rootless_shutdown_test_set_session_publish_checkpoint(
		replace_published_session_state);
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	rootless_shutdown_test_set_session_publish_checkpoint(NULL);
	publish_swap_prefix_fd = -1;
	struct stat replacement = {0};
	int replacement_survived = fstatat(prefix->directory_fd,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, &replacement,
		AT_SYMLINK_NOFOLLOW) == 0;
	if (replacement_survived) {
		(void)unlinkat(prefix->directory_fd,
			ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, 0);
		(void)fsync(prefix->directory_fd);
	}
	rootless_shutdown_release_closure(&closure);
	if (rc != -ESTALE || !publish_swap_performed || !replacement_survived) {
		fprintf(stderr, "session publication swap contract failed rc=%d "
			"swapped=%d survived=%d prefix=%s error=%s\n", rc,
			publish_swap_performed, replacement_survived, prefix_path, error);
		return 1;
	}
	return 0;
}

static int cgroup_create_fault_case(
	const char* prefix_path, darling_runtime_prefix prefix, unsigned phase)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	reset_cgroup_create_fixture();
	rootless_shutdown_test_set_cgroup_create_checkpoint(
		capture_or_swap_created_cgroup);
	rootless_shutdown_test_set_cgroup_create_error(phase, EIO);
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	rootless_shutdown_test_set_cgroup_create_error(0, 0);
	rootless_shutdown_test_set_cgroup_create_checkpoint(NULL);
	struct stat remaining;
	int cgroup_exists = cgroup_checkpoint_parent_fd >= 0 &&
		cgroup_checkpoint_leaf[0] != '\0' &&
		fstatat(cgroup_checkpoint_parent_fd, cgroup_checkpoint_leaf,
			&remaining, AT_SYMLINK_NOFOLLOW) == 0;
	int state_exists = endpoint_exists(prefix_path,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME);
	rootless_shutdown_release_closure(&closure);
	reset_cgroup_create_fixture();
	if (rc != -EIO || cgroup_exists || state_exists) {
		fprintf(stderr, "cgroup create fault contract failed phase=%u "
			"rc=%d cgroup=%d state=%d error=%s\n", phase, rc,
			cgroup_exists, state_exists, error);
		return 1;
	}
	return 0;
}

static int cgroup_create_swap_case(
	const char* prefix_path, darling_runtime_prefix prefix, unsigned phase)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	reset_cgroup_create_fixture();
	cgroup_swap_phase = (int)phase;
	rootless_shutdown_test_set_cgroup_create_checkpoint(
		capture_or_swap_created_cgroup);
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	rootless_shutdown_test_set_cgroup_create_checkpoint(NULL);
	struct stat replacement;
	int replacement_survived = cgroup_checkpoint_parent_fd >= 0 &&
		cgroup_checkpoint_leaf[0] != '\0' &&
		fstatat(cgroup_checkpoint_parent_fd, cgroup_checkpoint_leaf,
			&replacement, AT_SYMLINK_NOFOLLOW) == 0 &&
		replacement.st_dev == cgroup_replacement_device &&
		replacement.st_ino == cgroup_replacement_inode;
	int state_exists = endpoint_exists(prefix_path,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME);
	int swap_performed = cgroup_swap_performed;
	rootless_shutdown_release_closure(&closure);
	reset_cgroup_create_fixture();
	if (rc != -ESTALE || !swap_performed ||
		!replacement_survived || state_exists) {
		fprintf(stderr, "cgroup create swap contract failed phase=%u "
			"rc=%d swapped=%d survived=%d state=%d error=%s\n", phase,
			rc, swap_performed, replacement_survived,
			state_exists, error);
		return 1;
	}
	return 0;
}

static int cgroup_prebind_drain_bound_case(
	const char* prefix_path, darling_runtime_prefix prefix, int deadline)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	reset_cgroup_create_fixture();
	rootless_shutdown_test_set_cgroup_create_checkpoint(
		capture_or_swap_created_cgroup);
	if (deadline) {
		test_monotonic_now = 1000;
		prebind_drain_advance_at = 1;
		rootless_shutdown_test_set_monotonic_clock(test_monotonic_clock);
		rootless_shutdown_test_set_prebind_event_checkpoint(
			advance_prebind_drain);
	} else {
		cgroup_event_churn_count = 8;
		rootless_shutdown_test_set_prebind_event_budget(4);
	}
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	rootless_shutdown_test_set_monotonic_clock(NULL);
	rootless_shutdown_test_set_prebind_event_checkpoint(NULL);
	rootless_shutdown_test_set_prebind_event_budget(0);
	rootless_shutdown_test_set_cgroup_create_checkpoint(NULL);
	struct stat remaining;
	int cgroup_survived = cgroup_checkpoint_parent_fd >= 0 &&
		cgroup_checkpoint_leaf[0] != '\0' &&
		fstatat(cgroup_checkpoint_parent_fd, cgroup_checkpoint_leaf,
			&remaining, AT_SYMLINK_NOFOLLOW) == 0;
	int state_exists = endpoint_exists(prefix_path,
		ROOTLESS_SHUTDOWN_SESSION_STATE_NAME);
	unsigned generated = cgroup_event_churn_generated;
	size_t drained = prebind_drain_event_count;
	rootless_shutdown_release_closure(&closure);
	reset_cgroup_create_fixture();
	int expected = deadline ? -ETIMEDOUT : -EOVERFLOW;
	if (rc != expected || !cgroup_survived || state_exists ||
		(deadline ? drained < 1 : generated <= 4)) {
		fprintf(stderr, "cgroup prebind drain bound failed deadline=%d "
			"rc=%d survived=%d state=%d generated=%u drained=%zu error=%s\n",
			deadline, rc, cgroup_survived, state_exists, generated,
			drained, error);
		return 1;
	}
	return 0;
}

static int bounded_acquisition_case(
	const char* prefix_path, darling_runtime_prefix prefix,
	size_t pidfd_budget, size_t deadline_after_read,
	unsigned deadline_after_sorted, size_t maximum_reads,
	unsigned deadline_during_compact, size_t compact_index,
	int deadline_after_containment)
{
	int fd_baseline = count_open_fds();
	if (fd_baseline < 0)
		return 1;
	if (prepare_endpoints(prefix_path, 0) != 0)
		return 2;
	struct session_fixture fixture = spawn_fixture(FIXTURE_GRACEFUL, prefix);
	if (fixture.init <= 0 || fixture.leader <= 0 || fixture.worker <= 0)
		return 3;
	test_monotonic_now = 1000;
	membership_read_count = 0;
	membership_read_advance_at = deadline_after_read;
	snapshot_sorted_count = 0;
	snapshot_sorted_advance_at = deadline_after_sorted;
	compact_checkpoint_snapshot = deadline_during_compact;
	compact_checkpoint_index = compact_index;
	containment_advance_deadline = deadline_after_containment;
	rootless_shutdown_test_set_monotonic_clock(test_monotonic_clock);
	rootless_shutdown_test_set_membership_read_checkpoint(
		observe_membership_read);
	rootless_shutdown_test_set_snapshot_sorted_checkpoint(
		observe_snapshot_sorted);
	rootless_shutdown_test_set_ledger_compact_checkpoint(
		observe_ledger_compact);
	rootless_shutdown_test_set_containment_checkpoint(
		observe_containment);
	const struct rootless_shutdown_policy policy = {
		.acquisition_timeout_ms = 1,
		.pidfd_budget = pidfd_budget,
		.term_timeout_ms = 100,
		.kill_timeout_ms = 100,
		.poll_interval_ms = 1,
	};
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.leader, fixture.init,
		prefix, &policy, &result, error, sizeof(error));
	rootless_shutdown_test_set_snapshot_sorted_checkpoint(NULL);
	rootless_shutdown_test_set_ledger_compact_checkpoint(NULL);
	rootless_shutdown_test_set_containment_checkpoint(NULL);
	rootless_shutdown_test_set_membership_read_checkpoint(NULL);
	rootless_shutdown_test_set_monotonic_clock(NULL);
	membership_read_advance_at = 0;
	snapshot_sorted_advance_at = 0;
	compact_checkpoint_snapshot = 0;
	compact_checkpoint_index = 0;
	containment_advance_deadline = 0;
	int expected = pidfd_budget != 0 ? -EMFILE : -ETIMEDOUT;
	int all_survived = kill(fixture.init, 0) == 0 &&
		kill(fixture.leader, 0) == 0 && kill(fixture.worker, 0) == 0;
	if (rc != expected || result.phase != ROOTLESS_SHUTDOWN_CLOSURE_BOUND ||
		!all_survived || (maximum_reads != 0 &&
		 membership_read_count > maximum_reads) ||
		(deadline_after_sorted != 0 &&
		 snapshot_sorted_count < deadline_after_sorted)) {
		fprintf(stderr, "bounded acquisition contract failed budget=%zu rc=%d "
			"phase=%d reads=%zu sorted=%u survived=%d error=%s\n",
			pidfd_budget, rc, result.phase, membership_read_count,
			snapshot_sorted_count, all_survived, error);
		cleanup_fixture(&fixture);
		return 4;
	}
	if (finish_fixture_after_failed_acquisition(
			prefix_path, prefix, &fixture) != 0)
		return 5;
	int fd_post = count_open_fds();
	if (fd_post != fd_baseline) {
		fprintf(stderr, "bounded acquisition fd leak baseline=%d post=%d\n",
			fd_baseline, fd_post);
		return 6;
	}
	return 0;
}

static int signal_iteration_deadline_case(
	const char* prefix_path, darling_runtime_prefix prefix)
{
	int fd_baseline = count_open_fds();
	if (fd_baseline < 0 || prepare_endpoints(prefix_path, 0) != 0)
		return 1;
	struct session_fixture fixture = spawn_fixture(FIXTURE_STUBBORN, prefix);
	if (fixture.init <= 0 || fixture.leader <= 0 || fixture.worker <= 0)
		return 2;
	test_monotonic_now = 1000;
	signal_checkpoint_number = SIGTERM;
	signal_checkpoint_count = 0;
	signal_checkpoint_advance_at = 1;
	rootless_shutdown_test_set_monotonic_clock(test_monotonic_clock);
	rootless_shutdown_test_set_signal_checkpoint(observe_signal_iteration);
	const struct rootless_shutdown_policy policy = {
		.acquisition_timeout_ms = 1,
		.term_timeout_ms = 1,
		.kill_timeout_ms = 100,
		.poll_interval_ms = 1,
	};
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.leader, fixture.init,
		prefix, &policy, &result, error, sizeof(error));
	rootless_shutdown_test_set_signal_checkpoint(NULL);
	rootless_shutdown_test_set_monotonic_clock(NULL);
	signal_checkpoint_number = 0;
	signal_checkpoint_advance_at = 0;
	int all_survived = kill(fixture.init, 0) == 0 &&
		kill(fixture.leader, 0) == 0 && kill(fixture.worker, 0) == 0;
	if (rc != -ETIMEDOUT || result.phase != ROOTLESS_SHUTDOWN_TERM ||
		signal_checkpoint_count != 1 || !all_survived) {
		fprintf(stderr, "signal deadline contract failed rc=%d phase=%d "
			"checkpoint=%zu survived=%d error=%s\n", rc, result.phase,
			signal_checkpoint_count, all_survived, error);
		cleanup_fixture(&fixture);
		return 3;
	}
	if (finish_fixture_after_failed_acquisition(
			prefix_path, prefix, &fixture) != 0)
		return 4;
	int fd_post = count_open_fds();
	if (fd_post != fd_baseline) {
		fprintf(stderr, "signal deadline fd leak baseline=%d post=%d\n",
			fd_baseline, fd_post);
		return 5;
	}
	return 0;
}

int main(void)
{
	char prefix_path[] = "/tmp/darling-rootless-shutdown.XXXXXX";
	if (mkdtemp(prefix_path) == NULL)
		return 1;
	darling_runtime_prefix prefix = DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	if (darling_runtime_mode_open_prefix(
			prefix_path, prefix, error, sizeof(error)) != 0) {
		fprintf(stderr, "open prefix failed: %s\n", error);
		return 2;
	}
	struct rootless_shutdown_closure_capability backend_probe =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	if (rootless_shutdown_prepare_closure(prefix, &backend_probe,
			error, sizeof(error)) != 0)
		return 81;
	if (backend_probe.backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER) {
		rootless_shutdown_release_closure(&backend_probe);
		darling_runtime_mode_close_prefix(prefix);
		if (rmdir(prefix_path) != 0)
			return 82;
		puts("ROOTLESS_SHUTDOWN_LIFECYCLE_OK backend=PIDFD_SUBREAPER "
			"cgroup_specific=NOT_APPLICABLE");
		return 0;
	}
	if (rootless_shutdown_cleanup_empty_closure(&backend_probe,
			error, sizeof(error)) != 0)
		return 83;
	if (preflight_before_mutation_case(prefix_path, prefix) != 0)
		return 56;
	if (cgroup_same_name_aba_case(prefix_path, prefix) != 0)
		return 57;
	if (missing_session_identity_case(prefix) != 0)
		return 64;
	if (hostile_umask_cgroup_case(prefix) != 0)
		return 65;
	if (session_publication_swap_case(prefix_path, prefix) != 0)
		return 66;
	if (cgroup_create_fault_case(prefix_path, prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_CAPABILITY_OPEN) != 0)
		return 75;
	if (cgroup_create_swap_case(prefix_path, prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_CAPABILITY_OPEN) != 0)
		return 76;
	if (cgroup_prebind_drain_bound_case(prefix_path, prefix, 0) != 0)
		return 79;
	if (cgroup_prebind_drain_bound_case(prefix_path, prefix, 1) != 0)
		return 80;
	if (cgroup_create_fault_case(prefix_path, prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MODE) != 0)
		return 70;
	if (cgroup_create_fault_case(prefix_path, prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_READABLE_OPEN) != 0)
		return 71;
	if (cgroup_create_swap_case(prefix_path, prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MODE) != 0)
		return 72;
	if (cgroup_create_swap_case(prefix_path, prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_READABLE_OPEN) != 0)
		return 73;
	if (bounded_acquisition_case(prefix_path, prefix, 2, 0, 0, 3,
			0, 0, 0) != 0)
		return 67;
	if (bounded_acquisition_case(prefix_path, prefix, 0, 1, 0, 1,
			0, 0, 0) != 0)
		return 68;
	if (bounded_acquisition_case(prefix_path, prefix, 0, 0, 2, 0,
			0, 0, 0) != 0)
		return 69;
	if (bounded_acquisition_case(prefix_path, prefix, 0, 0, 0, 0,
			2, 1, 0) != 0)
		return 74;
	if (bounded_acquisition_case(prefix_path, prefix, 0, 0, 0, 0,
			0, 0, 1) != 0)
		return 77;
	if (signal_iteration_deadline_case(prefix_path, prefix) != 0)
		return 78;

	for (int cycle = 0; cycle < 3; ++cycle) {
		int rc = run_shutdown_case(
			prefix_path, prefix, FIXTURE_GRACEFUL, 0);
		if (rc != 0)
			return rc;
	}
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_STUBBORN, 1) != 0)
		return 30;
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_LATE_FORK, 1) != 0)
		return 31;
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_ROOT_LATE_FORK, 1) != 0)
		return 32;
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_ZOMBIE_SESSION, 0) != 0)
		return 33;
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_INIT_EXITED, 0) != 0)
		return 53;
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_PIDFD_EXIT, 0) != 0)
		return 54;
	if (run_shutdown_case(prefix_path, prefix,
			FIXTURE_SESSION_EXITED, 0) != 0)
		return 58;
	if (run_shutdown_case(prefix_path, prefix,
			FIXTURE_DIFFERENT_PARENT, 0) != 0)
		return 59;
	if (low_rlimit_case(prefix_path, prefix) != 0)
		return 60;
	if (acquisition_failure_case(prefix_path, prefix, 1, 0, 0) != 0)
		return 61;
	if (acquisition_failure_case(prefix_path, prefix, 0, 1, 0) != 0)
		return 62;
	if (acquisition_failure_case(prefix_path, prefix, 0, 0, 2) != 0)
		return 63;
	if (proc_root_scan_attempts != 0)
		return 55;

	/* Missing endpoint parents mean the endpoints are already absent. */
	char missing_path[512];
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp/launchd", prefix_path);
	if (rmdir(missing_path) != 0)
		return 34;
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp", prefix_path);
	if (rmdir(missing_path) != 0)
		return 35;
	snprintf(missing_path, sizeof(missing_path), "%s/var/run", prefix_path);
	if (rmdir(missing_path) != 0)
		return 36;
	struct session_fixture missing_fixture = spawn_fixture(FIXTURE_GRACEFUL, prefix);
	struct rootless_shutdown_result missing_result;
	if (missing_fixture.init <= 0 || missing_fixture.leader <= 0 ||
		shutdown_rootless_runtime(missing_fixture.leader, missing_fixture.init,
			prefix, NULL, &missing_result, error, sizeof(error)) != 0 ||
		missing_result.phase != ROOTLESS_SHUTDOWN_STOPPED) {
		cleanup_fixture(&missing_fixture);
		return 37;
	}
	(void)waitpid(missing_fixture.init, NULL, 0);
	missing_fixture.init = -1;
	cleanup_fixture(&missing_fixture);
	snprintf(missing_path, sizeof(missing_path), "%s/var/run", prefix_path);
	if (mkdir(missing_path, 0700) != 0)
		return 38;
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp", prefix_path);
	if (mkdir(missing_path, 0700) != 0)
		return 39;
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp/launchd", prefix_path);
	if (mkdir(missing_path, 0700) != 0)
		return 40;

	if (prepare_endpoints(prefix_path, 0) != 0)
		return 41;
	struct rootless_shutdown_closure_capability timeout_closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	if (rootless_shutdown_prepare_closure(prefix, &timeout_closure,
			error, sizeof(error)) != 0)
		return 42;
	pid_t timeout_member = fork();
	if (timeout_member == 0) {
		if (rootless_shutdown_enter_closure(&timeout_closure,
				error, sizeof(error)) != 0)
			_exit(2);
		rootless_shutdown_release_closure(&timeout_closure);
		if (setsid() < 0)
			_exit(1);
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	}
	if (timeout_member < 0)
		return 42;
	usleep(20000);
	if (rootless_shutdown_closure_contains(&timeout_closure, timeout_member,
			error, sizeof(error)) != 0)
		return 42;
	const struct rootless_shutdown_policy timeout_policy = {
		.term_timeout_ms = 0,
		.kill_timeout_ms = 0,
		.poll_interval_ms = 1,
	};
	struct rootless_shutdown_result timeout_result;
	int timeout_rc = shutdown_rootless_runtime(
		timeout_member,
		timeout_member,
		prefix,
		&timeout_policy,
		&timeout_result,
		error,
		sizeof(error));
	(void)waitpid(timeout_member, NULL, 0);
	if (rootless_shutdown_cleanup_empty_closure(&timeout_closure,
			error, sizeof(error)) != 0)
		return 43;
	if (timeout_rc != -ETIMEDOUT ||
		timeout_result.phase != ROOTLESS_SHUTDOWN_KILL ||
		!endpoint_exists(prefix_path, ".init.pid")) {
		fprintf(stderr, "timeout contract failed rc=%d phase=%d error=%s\n",
			timeout_rc, timeout_result.phase, error);
		return 43;
	}
	static const char* timeout_endpoints[] = {
		".init.pid",
		".darlingserver.sock",
		".darlingserver.stat.sock",
		"var/run/shellspawn.sock",
		"var/tmp/launchd/sock",
	};
	for (size_t index = 0;
		index < sizeof(timeout_endpoints) / sizeof(timeout_endpoints[0]);
		++index) {
		if (darling_runtime_mode_unlink_relative(prefix,
				timeout_endpoints[index], 0, true,
				error, sizeof(error)) != 0)
			return 44;
	}

	/* Guest processes must unlink their own E-UNION endpoints before the
	 * launcher publishes STOPPED. A surviving guest endpoint fails closed so a
	 * host unlink cannot leave a stale durable sidecar record. */
	if (prepare_endpoints(prefix_path, 1) != 0)
		return 49;
	struct session_fixture guest_endpoint_fixture =
		spawn_fixture(FIXTURE_GRACEFUL, prefix);
	if (guest_endpoint_fixture.init <= 0 || guest_endpoint_fixture.leader <= 0)
		return 50;
	struct rootless_shutdown_result guest_endpoint_result;
	int guest_endpoint_rc = shutdown_rootless_runtime(
		guest_endpoint_fixture.leader,
		guest_endpoint_fixture.init,
		prefix,
		NULL,
		&guest_endpoint_result,
		error,
		sizeof(error));
	(void)waitpid(guest_endpoint_fixture.init, NULL, 0);
	guest_endpoint_fixture.init = -1;
	cleanup_fixture(&guest_endpoint_fixture);
	if (guest_endpoint_rc == 0 ||
		guest_endpoint_result.phase != ROOTLESS_SHUTDOWN_DRAINED ||
		!endpoint_exists(prefix_path, "var/run/shellspawn.sock") ||
		!endpoint_exists(prefix_path, "var/tmp/launchd/sock")) {
		fprintf(stderr, "guest endpoint fail-closed contract failed rc=%d "
			"phase=%d error=%s\n", guest_endpoint_rc,
			guest_endpoint_result.phase, error);
		return 51;
	}
	static const char* failed_endpoints[] = {
		".init.pid",
		".darlingserver.sock",
		".darlingserver.stat.sock",
		"var/run/shellspawn.sock",
		"var/tmp/launchd/sock",
	};
	for (size_t index = 0;
		index < sizeof(failed_endpoints) / sizeof(failed_endpoints[0]);
		++index) {
		if (darling_runtime_mode_unlink_relative(prefix,
				failed_endpoints[index], 0, true,
				error, sizeof(error)) != 0)
			return 52;
	}

	darling_runtime_mode_close_prefix(prefix);
	char path[512];
	snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix_path);
	if (rmdir(path) != 0)
		return 45;
	snprintf(path, sizeof(path), "%s/var/tmp", prefix_path);
	if (rmdir(path) != 0)
		return 46;
	snprintf(path, sizeof(path), "%s/var/run", prefix_path);
	if (rmdir(path) != 0)
		return 47;
	snprintf(path, sizeof(path), "%s/var", prefix_path);
	if (rmdir(path) != 0 || rmdir(prefix_path) != 0)
		return 48;
	puts("ROOTLESS_SHUTDOWN_LIFECYCLE_OK cycles=3 stubborn=PASS "
		"late_fork=PASS root_late_fork=PASS zombie_session=PASS "
		"init_enoent=PASS member_enoent=PASS pidfd_exit_race=PASS "
		"pid_replacement=PASS cgroup_aba=PASS delegated_parent=BOUND "
		"session_identity=REQUIRED pidfd_preflight=EARLY "
		"low_rlimit=PASS churn=BOUNDED compaction_deadline=PASS "
		"containment_deadline=PASS signal_deadline=PASS "
		"cgroup_create_faults=PASS cgroup_create_swap=PASS "
		"cgroup_prebind=PASS prebind_drain=BOUNDED empty_sort=PASS "
		"proc_root_scan=ABSENT "
		"timeout=PASS endpoints=5 "
		"guest_endpoint_fail_closed=PASS");
	return 0;
}
