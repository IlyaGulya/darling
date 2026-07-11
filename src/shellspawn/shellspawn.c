/*
This file is part of Darling.

Copyright (C) 2017 Lubos Dolezel

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include "shellspawn.h"
#include "duct_signals.h"
#include "wait_status.h"

#define DBG 0

int g_serverSocket = -1;
struct sigaction sigchld_oldaction;

void setupSocket(void);
void listenForConnections(void);
void spawnShell(int fd);
void setupSigchild(void);
void restoreSigchild(void);
void reapAll(void);

enum shell_wait_result
{
	SHELL_WAIT_EXITED,
	SHELL_WAIT_CLIENT_CLOSED,
	SHELL_WAIT_ERROR,
};

static void closeShellFds(int shellfd[3])
{
	for (int i = 0; i < 3; i++)
	{
		if (shellfd[i] != -1)
		{
			close(shellfd[i]);
			shellfd[i] = -1;
		}
	}
}

static void closeFd(int* fd)
{
	if (*fd != -1)
	{
		close(*fd);
		*fd = -1;
	}
}

static int shellExitCode(int status)
{
	return shellspawn_exit_code_from_wait_status(status);
}

static int reapShell(pid_t shell_pid, int* status)
{
	pid_t wait_result;
	do
	{
		wait_result = waitpid(shell_pid, status, 0);
	}
	while (wait_result == -1 && errno == EINTR);

	return wait_result == shell_pid ? 0 : -1;
}

static enum shell_wait_result waitForShell(pid_t shell_pid, int fd, const int shellfd[3], int* status)
{
	for (;;)
	{
		pid_t wait_result = waitpid(shell_pid, status, WNOHANG);
		if (wait_result == shell_pid)
			return SHELL_WAIT_EXITED;
		if (wait_result == -1)
		{
			if (errno == EINTR)
				continue;
			return SHELL_WAIT_ERROR;
		}

		struct pollfd pollfd = {
			.fd = fd,
			.events = POLLIN,
		};
		// Polling bounds the quick-exit race when SIGCHLD does not interrupt poll.
		int poll_result = poll(&pollfd, 1, 100);
		if (poll_result == -1)
		{
			if (errno == EINTR)
				continue;
			return SHELL_WAIT_ERROR;
		}
		if (poll_result == 0)
			continue;
		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return SHELL_WAIT_CLIENT_CLOSED;
		if (!(pollfd.revents & POLLIN))
			continue;

		struct shellspawn_cmd cmd;
		if (read(fd, &cmd, sizeof(cmd)) != sizeof(cmd))
			return SHELL_WAIT_CLIENT_CLOSED;

		switch (cmd.cmd)
		{
			case SHELLSPAWN_SIGNAL:
			{
				int linux_signal;
				if (cmd.data_length != sizeof(int))
				{
					errno = EPROTO;
					return SHELL_WAIT_ERROR;
				}
				if (read(fd, &linux_signal, sizeof(int)) != sizeof(int))
					return SHELL_WAIT_CLIENT_CLOSED;

				int darwin_signal = signum_linux_to_bsd(linux_signal);
				if (DBG) printf("rcvd signal %d -> %d\n", linux_signal, darwin_signal);
				if (darwin_signal != 0)
				{
					int fg_pid = tcgetpgrp(shellfd[0]);
					if (fg_pid != -1)
						kill(fg_pid, darwin_signal);
					else
						kill(-shell_pid, darwin_signal);
				}
				break;
			}
			default:
				errno = EPROTO;
				return SHELL_WAIT_ERROR;
		}
	}
}
static void rootlessTestDelaySocketReady(void)
{
	const char* rootless = getenv("DARLING_ROOTLESS");
	const char* value = getenv("DARLING_TEST_SHELLSPAWN_READY_DELAY_MS");
	if (rootless == NULL || strcmp(rootless, "1") != 0 || value == NULL || *value == '\0')
		return;

	char* end = NULL;
	errno = 0;
	long delay = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || delay < 0 || delay > 30000)
	{
		fprintf(stderr, "Invalid DARLING_TEST_SHELLSPAWN_READY_DELAY_MS: %s\n", value);
		exit(EXIT_FAILURE);
	}

	poll(NULL, 0, (int)delay);
}

static void rootlessTestMarkSocketPending(void)
{
	const char* rootless = getenv("DARLING_ROOTLESS");
	const char* delay = getenv("DARLING_TEST_SHELLSPAWN_READY_DELAY_MS");
	const char* path = getenv("WEST_ROOTLESS_BOOTSTRAP_READY_FILE");
	if (rootless == NULL || strcmp(rootless, "1") != 0 || delay == NULL || *delay == '\0'
		|| path == NULL || *path == '\0')
		return;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd == -1)
	{
		perror("Opening rootless shellspawn readiness marker");
		exit(EXIT_FAILURE);
	}
	const char marker[] = "shellspawn-pending\n";
	if (write(fd, marker, sizeof(marker) - 1) != (ssize_t)(sizeof(marker) - 1))
	{
		perror("Writing rootless shellspawn readiness marker");
		close(fd);
		exit(EXIT_FAILURE);
	}
	close(fd);
}
int main(int argc, const char** argv)
{
	// shellspawn (daemon) --fork()--> shellspawn (child) --fork()--> exec /bin/bash
	// in order to read the exit status of the shell process,
	// we have to allow it to become a zombie, therefore we need to
	// restore the sigaction of SIGCHLD of the child shellspawn
	setupSigchild();
	rootlessTestDelaySocketReady();
	rootlessTestMarkSocketPending();
	setupSocket();
	listenForConnections();

	if (g_serverSocket != -1)
		close(g_serverSocket);
	return 0;
}

void setupSocket(void)
{
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = SHELLSPAWN_SOCKPATH
	};

	g_serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_serverSocket == -1)
	{
		perror("Creating unix socket");
		exit(EXIT_FAILURE);
	}

	fcntl(g_serverSocket, F_SETFD, FD_CLOEXEC);
	unlink(SHELLSPAWN_SOCKPATH);

	if (bind(g_serverSocket, (struct sockaddr*) &addr, sizeof(addr)) == -1)
	{
		perror("Binding the unix socket");
		exit(EXIT_FAILURE);
	}

	chmod(addr.sun_path, 0600);

	if (listen(g_serverSocket, 16384) == -1)
	{
		perror("Listening on unix socket");
		exit(EXIT_FAILURE);
	}
}

void listenForConnections(void)
{
	int sock;
	struct sockaddr_un addr;
	socklen_t len = sizeof(addr);

	while (true)
	{
		sock = accept(g_serverSocket, (struct sockaddr*) &addr, &len);
		if (sock == -1)
			break;

		if (fork() == 0)
		{
			restoreSigchild();
			fcntl(sock, F_SETFD, FD_CLOEXEC);
			spawnShell(sock);
			exit(EXIT_SUCCESS);
		}
		else
		{
			close(sock);
		}
	}
}

void spawnShell(int fd)
{
	pid_t shell_pid = -1;
	int shellfd[3] = { -1, -1, -1 };
	int pipefd[2] = { -1, -1 };
	int rv;
	char** argv = NULL;
	int argc = 2;
	struct msghdr msg;
	struct iovec iov;
	char cmsgbuf[CMSG_SPACE(sizeof(int)) * 3];
	int wstatus;
	int error;
	struct shellspawn_result result;
	struct shellspawn_result failure;

	bool read_cmds = true;

	argv = (char**) malloc(sizeof(char*) * 3);
	argv[0] = "/bin/bash";
	argv[1] = "--login";

	char* alloc_exec = NULL;

	// Read commands from client
	while (read_cmds)
	{
		struct shellspawn_cmd cmd;
		char* param = NULL;

		memset(&msg, 0, sizeof(msg));
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);

		iov.iov_base = &cmd;
		iov.iov_len = sizeof(cmd);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		if (recvmsg(fd, &msg, 0) != sizeof(cmd))
		{
			if (DBG) puts("bad recvmsg");
			goto err;
		}

		if (cmd.data_length != 0)
		{
			param = (char*) malloc(cmd.data_length + 1);
			if (read(fd, param, cmd.data_length) != cmd.data_length)
				goto err;
			param[cmd.data_length] = '\0';
		}

		switch (cmd.cmd)
		{
			case SHELLSPAWN_ADDARG:
			{
				if (param != NULL)
				{
					argv = (char**) realloc(argv, sizeof(char*) * (argc + 1));
					argv[argc] = param;
					if (DBG) printf("add arg: %s\n", param);
					argc++;
				}
				break;
			}
			case SHELLSPAWN_SETENV:
			{
				if (param != NULL)
				{
					if (DBG) printf("set env: %s\n", param);
					putenv(param);
				}
				break;
			}
			case SHELLSPAWN_CHDIR:
			{
				if (param != NULL)
				{
					if (DBG) printf("chdir: %s\n", param);
					chdir(param);
					free(param);
				}
				break;
			}
			case SHELLSPAWN_GO:
			{
				struct cmsghdr *cmptr = CMSG_FIRSTHDR(&msg);

				if (cmptr == NULL)
				{
					if (DBG) puts("bad cmptr");
					goto err;
				}
				if (cmptr->cmsg_level != SOL_SOCKET
						|| cmptr->cmsg_type != SCM_RIGHTS)
				{
					if (DBG) puts("bad cmsg level/type");
					goto err;
				}
				if (cmptr->cmsg_len != CMSG_LEN(sizeof(int) * 3))
				{
					if (DBG) printf("bad cmsg_len: %d\n", cmptr->cmsg_len);
					goto err;
				}

				memcpy(shellfd, CMSG_DATA(cmptr), sizeof(int) * 3);

				if (DBG) printf("go, fds={ %d, %d, %d }\n", shellfd[0], shellfd[1], shellfd[2]);
				free(param);
				read_cmds = false;
				break;
			}
			case SHELLSPAWN_SETUIDGID:
			{
				int* ids = (int*) param;
				if (cmd.data_length < 2*sizeof(int))
				{
					free(param);
					break;
				}

				setuid(ids[0]);
				setgid(ids[1]);
				free(param);

				break;
			}
			case SHELLSPAWN_SETEXEC:
			{
				argc = 0;
				argv = realloc(argv, 0);
				alloc_exec = param;
				if (DBG) printf("setexec: %s\n", param);
				break;
			}
		}
	}

	// Add terminating NULL
	argv = (char**) realloc(argv, sizeof(char*) * (argc + 1));
	argv[argc] = NULL;

	if (pipe(pipefd) == -1)
		goto err;

	setsid();
	setpgrp();

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	dup2(shellfd[0], STDIN_FILENO);
	dup2(shellfd[1], STDOUT_FILENO);
	dup2(shellfd[2], STDERR_FILENO);

	ioctl(STDIN_FILENO, TIOCSCTTY, STDIN_FILENO);

	shell_pid = fork();
	if (shell_pid == 0)
	{
		close(fd);

		fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

		// In future, we may support spawning something else than Bash
		// and check the provided shell against /etc/shells
		execv(alloc_exec ? alloc_exec : "/bin/bash", argv);

		rv = errno;
		write(pipefd[1], &rv, sizeof(rv));
		close(pipefd[1]);

		exit(EXIT_FAILURE);
	}

	if (alloc_exec)
	{
		free(alloc_exec);
		alloc_exec = NULL;
	}

	// Check that exec succeeded
	closeFd(&pipefd[1]); // close the write end
	ssize_t exec_status;
	do
	{
		exec_status = read(pipefd[0], &rv, sizeof(rv));
	}
	while (exec_status == -1 && errno == EINTR);
	if (exec_status == sizeof(rv))
	{
		errno = rv;
		goto err;
	}
	if (exec_status != 0)
	{
		errno = exec_status == -1 ? errno : EPROTO;
		goto err;
	}
	closeFd(&pipefd[0]);

	enum shell_wait_result wait_result = waitForShell(shell_pid, fd, shellfd, &wstatus);
	if (wait_result == SHELL_WAIT_ERROR)
		goto err;
	if (wait_result == SHELL_WAIT_CLIENT_CLOSED)
	{
		kill(shell_pid, SIGKILL);
		if (reapShell(shell_pid, &wstatus) == -1)
			goto err;
	}

	closeShellFds(shellfd);
	result = (struct shellspawn_result){
		.kind = SHELLSPAWN_RESULT_EXIT,
		.value = shellExitCode(wstatus),
	};
	if (wait_result == SHELL_WAIT_EXITED)
		write(fd, &result, sizeof(result));

	if (DBG) printf("Shell terminated with exit code %d\n", result.value);
	close(fd);

	reapAll();
	return;
err:
	error = errno;
	if (DBG) fprintf(stderr, "Error spawning shell: %s\n", strerror(errno));

	closeFd(&pipefd[0]);
	closeFd(&pipefd[1]);
	closeShellFds(shellfd);

	if (shell_pid != -1)
		kill(shell_pid, SIGKILL);

	failure = (struct shellspawn_result){
		.kind = SHELLSPAWN_RESULT_ERROR,
		.value = error,
	};
	write(fd, &failure, sizeof(failure));
	close(fd);
	reapAll();
}

void setupSigchild(void)
{
	struct sigaction sigchld_action = {
		.sa_handler = SIG_DFL,
		.sa_flags = SA_NOCLDWAIT
	};
	sigaction(SIGCHLD, &sigchld_action, &sigchld_oldaction);
}

void restoreSigchild(void)
{
	sigaction(SIGCHLD, &sigchld_oldaction, NULL);
}

void reapAll(void)
{
    while (waitpid((pid_t)(-1), 0, WNOHANG) > 0);
}
