#include "../rootless_shutdown.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
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
	int status;

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
	if (shutdown_rootless_process_session(leader) != 0)
		return 1;
	if (waitpid(leader, &status, 0) != leader || !WIFSIGNALED(status))
		return 1;
	for (int attempt = 0; attempt < 20; attempt++) {
		if (kill(fixture.worker, 0) < 0 && errno == ESRCH) {
			puts("ROOTLESS_SHUTDOWN_SESSION_OK");
			return 0;
		}
		usleep(10000);
	}
	return 1;
}
