#include "../rootless_shutdown.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct session_fixture {
	pid_t worker;
};

int main(void)
{
	int pipefd[2];
	pid_t leader;
	struct session_fixture fixture;
	struct rootless_shutdown_result result;
	darling_runtime_prefix prefix = DARLING_RUNTIME_PREFIX_INITIALIZER;
	char prefix_path[] = "/tmp/darling-rootless-session.XXXXXX";
	char error[256] = {0};
	int status;

	if (mkdtemp(prefix_path) == NULL ||
		darling_runtime_mode_open_prefix(prefix_path, prefix,
			error, sizeof(error)) != 0)
		return 1;
	char path[512];
	snprintf(path, sizeof(path), "%s/var", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/var/run", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/var/tmp", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix_path);
	if (mkdir(path, 0700) != 0)
		return 1;
	if (pipe(pipefd) != 0)
		return 1;
	leader = fork();
	if (leader < 0)
		return 1;
	if (leader == 0) {
		close(pipefd[0]);
		if (setsid() < 0)
			return 2;
		fixture.worker = fork();
		if (fixture.worker < 0)
			return 3;
		if (fixture.worker == 0) {
			for (;;)
				pause();
		}
		if (write(pipefd[1], &fixture, sizeof(fixture)) != sizeof(fixture))
			return 4;
		for (;;)
			pause();
	}
	close(pipefd[1]);
	if (read(pipefd[0], &fixture, sizeof(fixture)) != sizeof(fixture))
		return 1;
	close(pipefd[0]);
	if (shutdown_rootless_runtime(leader, leader, prefix, NULL,
			&result, error, sizeof(error)) != 0 ||
		result.phase != ROOTLESS_SHUTDOWN_STOPPED)
		return 1;
	if (waitpid(leader, &status, 0) != leader || !WIFSIGNALED(status))
		return 1;
	for (int attempt = 0; attempt < 20; attempt++) {
		if (kill(fixture.worker, 0) < 0 && errno == ESRCH) {
			darling_runtime_mode_close_prefix(prefix);
			snprintf(path, sizeof(path), "%s/var/tmp/launchd", prefix_path);
			if (rmdir(path) != 0)
				return 1;
			snprintf(path, sizeof(path), "%s/var/tmp", prefix_path);
			if (rmdir(path) != 0)
				return 1;
			snprintf(path, sizeof(path), "%s/var/run", prefix_path);
			if (rmdir(path) != 0)
				return 1;
			snprintf(path, sizeof(path), "%s/var", prefix_path);
			if (rmdir(path) != 0 || rmdir(prefix_path) != 0)
				return 1;
			puts("ROOTLESS_SHUTDOWN_SESSION_OK");
			return 0;
		}
		usleep(10000);
	}
	return 1;
}
