#include "../rootless_shutdown.h"
#include "../runtime_mode_prefix.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

enum fixture_mode {
	FIXTURE_GRACEFUL,
	FIXTURE_STUBBORN,
	FIXTURE_LATE_FORK,
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
};

static int late_fork_pipe = -1;

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
		(void)write(late_fork_pipe, &child, sizeof(child));
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

static int prepare_endpoints(const char* prefix)
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

	snprintf(path, sizeof(path), "%s/.init.pid", prefix);
	if (write_file(path, "123\n") != 0)
		return -1;
	static const char* sockets[] = {
		".darlingserver.sock",
		".darlingserver.stat.sock",
		"var/run/shellspawn.sock",
		"var/tmp/launchd/sock",
	};
	for (size_t index = 0; index < sizeof(sockets) / sizeof(sockets[0]); ++index) {
		snprintf(path, sizeof(path), "%s/%s", prefix, sockets[index]);
		if (bind_socket(path) != 0)
			return -1;
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

static struct session_fixture spawn_fixture(enum fixture_mode mode)
{
	int pipefd[2];
	struct session_fixture fixture = { .init = -1, .leader = -1,
		.worker = -1, .late_pipe = -1 };
	if (pipe(pipefd) != 0)
		return fixture;
	fixture.init = fork();
	if (fixture.init < 0)
		return fixture;
	if (fixture.init == 0) {
		close(pipefd[0]);
		if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
			_exit(9);
		if (mode != FIXTURE_GRACEFUL)
			signal(SIGTERM, SIG_IGN);
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
			for (;;)
				pause();
		}
		for (;;)
			pause();
	}
	close(pipefd[1]);
	struct fixture_payload payload;
	if (read_exact(pipefd[0], &payload, sizeof(payload)) != 0) {
		close(pipefd[0]);
		return fixture;
	}
	fixture.leader = payload.leader;
	fixture.worker = payload.worker;
	fixture.late_pipe = mode == FIXTURE_LATE_FORK ? pipefd[0] : -1;
	if (fixture.late_pipe < 0)
		close(pipefd[0]);
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

static int run_shutdown_case(
	const char* prefix_path,
	darling_runtime_prefix prefix,
	enum fixture_mode mode,
	int expect_kill
)
{
	if (prepare_endpoints(prefix_path) != 0)
		return 20;
	struct session_fixture fixture = spawn_fixture(mode);
	if (fixture.init <= 0 || fixture.leader <= 0 || fixture.worker <= 0)
		return 21;
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(
		fixture.leader,
		fixture.init,
		prefix,
		NULL,
		&result,
		error,
		sizeof(error));
	if (rc != 0 || result.phase != ROOTLESS_SHUTDOWN_STOPPED ||
		!!result.kill_rounds != !!expect_kill || !endpoints_removed(prefix_path)) {
		fprintf(stderr, "shutdown case failed mode=%d rc=%d phase=%d "
			"term=%u kill=%u error=%s\n", mode, rc, result.phase,
			result.term_rounds, result.kill_rounds, error);
		cleanup_fixture(&fixture);
		return 22;
	}
	pid_t late_pid = -1;
	if (mode == FIXTURE_LATE_FORK) {
		struct pollfd pollfd = { .fd = fixture.late_pipe, .events = POLLIN };
		if (poll(&pollfd, 1, 2000) <= 0 ||
			read_exact(fixture.late_pipe, &late_pid, sizeof(late_pid)) != 0) {
			cleanup_fixture(&fixture);
			return 23;
		}
	}
	(void)waitpid(fixture.init, NULL, 0);
	fixture.init = -1;
	if (wait_pid_gone(fixture.leader) != 0 ||
		wait_pid_gone(fixture.worker) != 0 ||
		(late_pid > 0 && wait_pid_gone(late_pid) != 0)) {
		cleanup_fixture(&fixture);
		return 24;
	}
	cleanup_fixture(&fixture);
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

	/* Missing endpoint parents mean the endpoints are already absent. */
	char missing_path[512];
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp/launchd", prefix_path);
	if (rmdir(missing_path) != 0)
		return 32;
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp", prefix_path);
	if (rmdir(missing_path) != 0)
		return 33;
	snprintf(missing_path, sizeof(missing_path), "%s/var/run", prefix_path);
	if (rmdir(missing_path) != 0)
		return 34;
	struct session_fixture missing_fixture = spawn_fixture(FIXTURE_GRACEFUL);
	struct rootless_shutdown_result missing_result;
	if (missing_fixture.init <= 0 || missing_fixture.leader <= 0 ||
		shutdown_rootless_runtime(missing_fixture.leader, missing_fixture.init,
			prefix, NULL, &missing_result, error, sizeof(error)) != 0 ||
		missing_result.phase != ROOTLESS_SHUTDOWN_STOPPED) {
		cleanup_fixture(&missing_fixture);
		return 35;
	}
	(void)waitpid(missing_fixture.init, NULL, 0);
	missing_fixture.init = -1;
	cleanup_fixture(&missing_fixture);
	snprintf(missing_path, sizeof(missing_path), "%s/var/run", prefix_path);
	if (mkdir(missing_path, 0700) != 0)
		return 36;
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp", prefix_path);
	if (mkdir(missing_path, 0700) != 0)
		return 37;
	snprintf(missing_path, sizeof(missing_path), "%s/var/tmp/launchd", prefix_path);
	if (mkdir(missing_path, 0700) != 0)
		return 38;

	if (prepare_endpoints(prefix_path) != 0)
		return 40;
	pid_t timeout_member = fork();
	if (timeout_member == 0) {
		if (setsid() < 0)
			_exit(1);
		signal(SIGTERM, SIG_IGN);
		for (;;)
			pause();
	}
	if (timeout_member < 0)
		return 41;
	usleep(20000);
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
	if (timeout_rc != -ETIMEDOUT ||
		timeout_result.phase != ROOTLESS_SHUTDOWN_KILL ||
		!endpoint_exists(prefix_path, ".init.pid")) {
		fprintf(stderr, "timeout contract failed rc=%d phase=%d error=%s\n",
			timeout_rc, timeout_result.phase, error);
		return 42;
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
				timeout_endpoints[index], 0, false,
				error, sizeof(error)) != 0)
			return 43;
	}

	darling_runtime_mode_close_prefix(prefix);
	char path[512];
	snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix_path);
	if (rmdir(path) != 0)
		return 44;
	snprintf(path, sizeof(path), "%s/var/tmp", prefix_path);
	if (rmdir(path) != 0)
		return 45;
	snprintf(path, sizeof(path), "%s/var/run", prefix_path);
	if (rmdir(path) != 0)
		return 46;
	snprintf(path, sizeof(path), "%s/var", prefix_path);
	if (rmdir(path) != 0 || rmdir(prefix_path) != 0)
		return 47;
	puts("ROOTLESS_SHUTDOWN_LIFECYCLE_OK cycles=3 stubborn=PASS "
		"late_fork=PASS timeout=PASS endpoints=5");
	return 0;
}
