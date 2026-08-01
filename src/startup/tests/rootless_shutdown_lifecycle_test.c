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
	FIXTURE_ROOT_LATE_FORK,
	FIXTURE_ZOMBIE_SESSION,
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

static void cleanup_fixture(struct session_fixture* fixture);

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

static struct session_fixture spawn_fixture(
	enum fixture_mode mode,
	darling_runtime_prefix prefix
)
{
	int pipefd[2];
	struct session_fixture fixture = { .init = -1, .leader = -1,
		.worker = -1, .late_pipe = -1 };
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[256] = {0};
	if (rootless_shutdown_prepare_closure(prefix, &closure,
			error, sizeof(error)) != 0) {
		fprintf(stderr, "fixture closure prepare failed: %s\n", error);
		return fixture;
	}
	if (pipe(pipefd) != 0) {
		(void)rootless_shutdown_cleanup_empty_closure(&closure, NULL, 0);
		return fixture;
	}
	fixture.init = fork();
	if (fixture.init < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		(void)rootless_shutdown_cleanup_empty_closure(&closure, NULL, 0);
		return fixture;
	}
	if (fixture.init == 0) {
		close(pipefd[0]);
		if (rootless_shutdown_enter_closure(&closure,
				error, sizeof(error)) != 0)
			_exit(8);
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
			mode != FIXTURE_ZOMBIE_SESSION) {
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
	if (rootless_shutdown_closure_contains(&closure, fixture.init,
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
	if (mode == FIXTURE_ZOMBIE_SESSION &&
		wait_pid_state(fixture.leader, 'Z') != 0) {
		cleanup_fixture(&fixture);
		return 22;
	}
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
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_ROOT_LATE_FORK, 1) != 0)
		return 32;
	if (run_shutdown_case(prefix_path, prefix, FIXTURE_ZOMBIE_SESSION, 0) != 0)
		return 33;

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

	/* Host cleanup must never unlink guest-owned endpoints behind E-UNION's
	 * durable sidecar. A surviving guest endpoint is a fail-closed shutdown,
	 * not permission to leave a stale INDEX record pointing at a removed inode. */
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
		"timeout=PASS endpoints=5 guest_endpoint_fail_closed=PASS");
	return 0;
}
