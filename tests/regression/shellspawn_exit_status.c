#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wait_status.h"

static int child_exit_status(void)
{
	pid_t child = fork();
	int status;

	if (child == 0)
		_exit(42);
	if (child < 0 || waitpid(child, &status, 0) != child)
		return 1;
	return shellspawn_exit_code_from_wait_status(status) == 42 ? 0 : 1;
}

static int child_signal_status(void)
{
	pid_t child = fork();
	int status;

	if (child == 0) {
		raise(SIGABRT);
		_exit(1);
	}
	if (child < 0 || waitpid(child, &status, 0) != child)
		return 1;
	return shellspawn_exit_code_from_wait_status(status) == 128 + SIGABRT ? 0 : 1;
}

int main(void)
{
	if (child_exit_status() != 0) {
		fprintf(stderr, "normal child exit was not preserved\n");
		return 1;
	}
	if (child_signal_status() != 0) {
		fprintf(stderr, "signal child did not map to 128 + signal\n");
		return 1;
	}
	puts("SHELLSPAWN_EXIT_STATUS_OK");
	return 0;
}
