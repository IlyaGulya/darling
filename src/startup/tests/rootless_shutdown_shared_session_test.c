#include "../rootless_shutdown.h"
#include "../runtime_mode_prefix.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct fixture {
	pid_t session_host;
	pid_t sentinel;
	pid_t runtime_root;
	pid_t runtime_worker;
};

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

static int wait_gone(pid_t pid)
{
	for (int attempt = 0; attempt < 500; ++attempt) {
		if (kill(pid, 0) != 0 && errno == ESRCH)
			return 0;
		usleep(10000);
	}
	return -1;
}

int main(void)
{
	char prefix_path[] = "/tmp/darling-rootless-shared-session.XXXXXX";
	darling_runtime_prefix prefix = DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char path[512];
	int pipefd[2];
	if (mkdtemp(prefix_path) == NULL ||
		darling_runtime_mode_open_prefix(prefix_path, prefix,
			error, sizeof(error)) != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/var", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 2;
	snprintf(path, sizeof(path), "%s/var/run", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 3;
	snprintf(path, sizeof(path), "%s/var/tmp", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 4;
	snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix_path);
	if (mkdir(path, 0700) != 0 ||
		rootless_shutdown_prepare_closure(prefix, &closure,
			error, sizeof(error)) != 0 || pipe(pipefd) != 0)
		return 5;

	pid_t session_host = fork();
	if (session_host < 0)
		return 6;
	if (session_host == 0) {
		close(pipefd[0]);
		if (setsid() < 0)
			_exit(10);
		signal(SIGCHLD, SIG_IGN);
		pid_t sentinel = fork();
		if (sentinel < 0)
			_exit(11);
		if (sentinel == 0) {
			rootless_shutdown_release_closure(&closure);
			for (;;)
				pause();
		}
		pid_t root = rootless_shutdown_fork_runtime(&closure,
			error, sizeof(error));
		if (root < 0)
			_exit(12);
		if (root == 0) {
			rootless_shutdown_release_closure(&closure);
			pid_t worker = fork();
			if (worker < 0)
				_exit(13);
			if (worker == 0) {
				for (;;)
					pause();
			}
			struct fixture fixture = {
				.session_host = getppid(),
				.sentinel = sentinel,
				.runtime_root = getpid(),
				.runtime_worker = worker,
			};
			if (write(pipefd[1], &fixture, sizeof(fixture)) !=
					(ssize_t)sizeof(fixture))
				_exit(14);
			for (;;)
				pause();
		}
		rootless_shutdown_release_closure(&closure);
		for (;;)
			pause();
	}
	close(pipefd[1]);
	struct fixture fixture;
	if (read_exact(pipefd[0], &fixture, sizeof(fixture)) != 0)
		return 7;
	close(pipefd[0]);
	if (fixture.session_host != session_host)
		return 8;
	if (rootless_shutdown_closure_contains(&closure, fixture.runtime_root,
			error, sizeof(error)) != 0)
		return 8;
	rootless_shutdown_release_closure(&closure);

	struct rootless_shutdown_result result;
	int rc = shutdown_rootless_runtime(
		fixture.runtime_worker, fixture.runtime_root, prefix, NULL,
		&result, error, sizeof(error));
	if (rc != 0 || result.phase != ROOTLESS_SHUTDOWN_STOPPED ||
		wait_gone(fixture.runtime_worker) != 0 ||
		wait_gone(fixture.runtime_root) != 0 ||
		kill(fixture.session_host, 0) != 0 || kill(fixture.sentinel, 0) != 0) {
		fprintf(stderr, "shared-session shutdown failed rc=%d phase=%d error=%s\n",
			rc, result.phase, error);
		return 9;
	}

	(void)kill(fixture.sentinel, SIGKILL);
	(void)kill(fixture.session_host, SIGKILL);
	(void)waitpid(fixture.session_host, NULL, 0);
	darling_runtime_mode_close_prefix(prefix);
	snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix_path);
	if (rmdir(path) != 0)
		return 15;
	snprintf(path, sizeof(path), "%s/var/tmp", prefix_path);
	if (rmdir(path) != 0)
		return 16;
	snprintf(path, sizeof(path), "%s/var/run", prefix_path);
	if (rmdir(path) != 0)
		return 17;
	snprintf(path, sizeof(path), "%s/var", prefix_path);
	if (rmdir(path) != 0 || rmdir(prefix_path) != 0)
		return 18;
	puts("ROOTLESS_SHUTDOWN_SHARED_SESSION_OK unrelated=ALIVE tree=DRAINED");
	return 0;
}
