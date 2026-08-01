/*
This file is part of Darling.

Copyright (C) 2016-2023 Lubos Dolezel

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

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdbool.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <pty.h>
#include <pwd.h>
#include <dirent.h>
#include "../shellspawn/shellspawn.h"
#include "darling.h"
#include "darling-config.h"
#include "runtime_credentials.h"
#include "runtime_mode.h"
#include "runtime_mode_prefix.h"
#include "rootless_shutdown.h"

// Between Linux 4.9 and 4.11, a strange bug has been introduced
// which prevents connecting to Unix sockets if the socket was
// created in a different mount namespace or under overlayfs
// (dunno which one is really responsible for this).
#define USE_LINUX_4_11_HACK 1
#define ROOTLESS_SHELLSPAWN_READY_TIMEOUT_MS 30000

uid_t g_originalUid;
gid_t g_originalGid;
bool g_fixPermissions = false;
char g_workingDirectory[4096];

static enum darling_runtime_mode g_runtimeMode = DARLING_RUNTIME_MODE_INVALID;
static darling_runtime_prefix g_runtimePrefix =
	DARLING_RUNTIME_PREFIX_INITIALIZER;

static void closeRuntimePrefix(void)
{
	darling_runtime_mode_close_prefix(g_runtimePrefix);
}

static bool rootlessModeEnabled(void)
{
	return darling_runtime_mode_is_rootless(g_runtimeMode);
}

static long rootlessShellspawnReadyTimeoutMs(void)
{
	const char* value = getenv("DARLING_ROOTLESS_SHELLSPAWN_READY_TIMEOUT_MS");
	if (value == NULL || *value == '\0')
		return ROOTLESS_SHELLSPAWN_READY_TIMEOUT_MS;

	char* end = NULL;
	errno = 0;
	long timeout_ms = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || timeout_ms < 1000 || timeout_ms > 300000)
	{
		fprintf(stderr, "Invalid DARLING_ROOTLESS_SHELLSPAWN_READY_TIMEOUT_MS: %s\n", value);
		exit(1);
	}
	return timeout_ms;
}

static void removePrivilegedRuntimeStateFiles(void)
{
	char error[512] = {0};
	static const char* entries[] = {
		".init.pid",
		"var/run/shellspawn.sock",
		".darlingserver.sock",
	};
	for (size_t index = 0;
		index < sizeof(entries) / sizeof(entries[0]); ++index) {
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				entries[index], 0, true, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove Darling runtime state %s: %s\n",
				entries[index], error);
			exit(1);
		}
	}
}

int main(int argc, char ** argv)
{
	pid_t pidInit;
	const char* requested_prefix;

	if (argc <= 1)
	{
		showHelp(argv[0]);
		return 1;
	}

	char runtimeModeError[512] = {0};
	struct darling_runtime_cli cli;
	if (darling_runtime_mode_parse_cli(
			argc,
			argv,
			&cli,
			runtimeModeError,
			sizeof(runtimeModeError)
		) != 0) {
		fprintf(stderr, "Cannot parse Darling launcher options: %s\n",
			runtimeModeError);
		return 1;
	}
	if (cli.show_help) {
		showHelp(argv[0]);
		return 0;
	}
	if (cli.show_version) {
		showVersion(argv[0]);
		return 0;
	}
	if (cli.command_index < 0 || cli.command_index >= argc) {
		showHelp(argv[0]);
		return 1;
	}
	const bool recreatePrefix =
		strcmp(argv[cli.command_index], "recreate-prefix") == 0;
	if (recreatePrefix != cli.confirm_prefix_recreate) {
		fprintf(stderr,
			recreatePrefix
				? "PREFIX_RECREATE_REQUIRED: recreate-prefix requires"
				  " exact --confirm-prefix-recreate confirmation\n"
				: "--confirm-prefix-recreate is valid only with"
				  " recreate-prefix\n");
		return 1;
	}

	if (darling_runtime_mode_select_process(
			&cli,
			DARLING_RUNTIME_EUNION_CAPABLE != 0,
			&g_runtimeMode,
			runtimeModeError,
			sizeof(runtimeModeError)
		) != 0) {
		fprintf(stderr, "Cannot select Darling runtime mode: %s\n",
			runtimeModeError);
		return 1;
	}

	const bool rootless = rootlessModeEnabled();

	if (!rootless && geteuid() != 0)
	{
		missingSetuidRoot();
		return 1;
	}

	g_originalUid = getuid();
	g_originalGid = getgid();

	if (!rootless)
	{
		setuid(0);
		setgid(0);
	}
	else
	{
		if (darling_runtime_drop_rootless_credentials(
				g_originalUid,
				g_originalGid,
				runtimeModeError,
				sizeof(runtimeModeError)
			) != 0) {
			fprintf(stderr, "Cannot enter rootless runtime mode: %s\n",
				runtimeModeError);
			return 1;
		}
		if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
		{
			fprintf(stderr, "Cannot enable rootless child reaping: %s\n", strerror(errno));
			return 1;
		}
	}
	if (darling_runtime_mode_publish(
			g_runtimeMode,
			runtimeModeError,
			sizeof(runtimeModeError)
		) != 0) {
		fprintf(stderr, "Cannot publish Darling runtime mode: %s\n",
			runtimeModeError);
		return 1;
	}

	requested_prefix = getenv("DPREFIX");
	if (!requested_prefix)
		requested_prefix = defaultPrefixPath();
	if (!requested_prefix)
		return 1;
	if (strlen(requested_prefix) > 255)
	{
		fprintf(stderr, "Prefix path too long\n");
		return 1;
	}
	unsetenv("DPREFIX");
	getcwd(g_workingDirectory, sizeof(g_workingDirectory));

	if (darling_runtime_mode_open_prefix(
			requested_prefix,
			g_runtimePrefix,
			runtimeModeError,
			sizeof(runtimeModeError)
		) != 0) {
		fprintf(stderr, "Cannot use Darling prefix: %s\n",
			runtimeModeError);
		return 1;
	}
	/* The retained directory capability is authoritative from this point. */
	requested_prefix = NULL;
	if (atexit(closeRuntimePrefix) != 0) {
		fprintf(stderr, "Cannot register Darling prefix fd cleanup\n");
		return 1;
	}
	struct passwd* prefix_owner = getpwuid(g_originalUid);
	if (prefix_owner == NULL) {
		fprintf(stderr,
			"Failed to find Linux /etc/passwd entry for current user\n");
		return 1;
	}
	if (!rootless) {
		if (setegid(g_originalGid) != 0 ||
			seteuid(g_originalUid) != 0) {
			fprintf(stderr,
				"Cannot enter invoking-user credentials for prefix lifecycle: %s\n",
				strerror(errno));
			(void)seteuid(0);
			(void)setegid(0);
			return 1;
		}
	}
	struct darling_runtime_prefix_lifecycle_result lifecycle;
	int lifecycle_result = recreatePrefix
		? darling_runtime_prefix_recreate(
			g_runtimePrefix,
			g_runtimeMode,
			prefix_owner->pw_name,
			g_originalUid,
			g_originalGid,
			&lifecycle,
			runtimeModeError,
			sizeof(runtimeModeError))
		: darling_runtime_prefix_prepare(
			g_runtimePrefix,
			g_runtimeMode,
			prefix_owner->pw_name,
			g_originalUid,
			g_originalGid,
			&lifecycle,
			runtimeModeError,
			sizeof(runtimeModeError));
	if (!rootless) {
		if (seteuid(0) != 0 || setegid(0) != 0) {
			fprintf(stderr,
				"Cannot restore privileged launcher credentials: %s\n",
				strerror(errno));
			return 1;
		}
	}
	if (lifecycle_result == DARLING_RUNTIME_PREFIX_RECREATE_REQUIRED) {
		fprintf(stderr,
			"PREFIX_RECREATE_REQUIRED: %s; run with"
			" --confirm-prefix-recreate recreate-prefix\n",
			runtimeModeError);
		return DARLING_RUNTIME_PREFIX_RECREATE_REQUIRED;
	}
	if (lifecycle_result != 0) {
		fprintf(stderr, "Cannot prepare Darling prefix lifecycle: %s\n",
			runtimeModeError);
		return 1;
	}
	if (recreatePrefix) {
		fprintf(stderr,
			"PREFIX_RECREATED format=sidecar-v1 generation=%llu\n",
			(unsigned long long)lifecycle.state.generation);
		return 0;
	}
	g_fixPermissions =
		lifecycle.action == DARLING_RUNTIME_PREFIX_CREATED ||
		lifecycle.action == DARLING_RUNTIME_PREFIX_REPAIRED ||
		lifecycle.action == DARLING_RUNTIME_PREFIX_RECREATED;
	checkPrefixOwner();

	const int commandIndex = cli.command_index;

	pidInit = getInitProcess();

	if (strcmp(argv[commandIndex], "shutdown") == 0)
	{
		if (pidInit == 0)
		{
			fprintf(stderr, "Darling container is not running\n");
			return 1;
		}

		// TODO: when we have a working launchd,
		// this is where we ask it to shut down nicely

		char path_buf[128];
		FILE* file;
		pid_t launchd_pid;
		snprintf(path_buf, sizeof(path_buf), "/proc/%d/task/%d/children", pidInit, pidInit);
		file = fopen(path_buf, "r");
		if (!file || fscanf(file, "%d", &launchd_pid) != 1) {
			fprintf(stderr, "Failed to shutdown Darling container\n");
			if (file) {
				fclose(file);
			}
			return 1;
		}
		fclose(file);

		if (rootless) {
			struct rootless_shutdown_result shutdown_state;
			int shutdown_result = shutdown_rootless_runtime(
				launchd_pid, pidInit, g_runtimePrefix, NULL,
				&shutdown_state, runtimeModeError,
				sizeof(runtimeModeError));
			if (shutdown_result != 0) {
				fprintf(stderr,
					"Failed to stop rootless Darling session at phase %s: %s\n",
					rootless_shutdown_phase_name(shutdown_state.phase),
					runtimeModeError[0] == '\0'
						? strerror(-shutdown_result) : runtimeModeError);
				return 1;
			}
		} else {
			kill(launchd_pid, SIGKILL);
			kill(pidInit, SIGKILL);
			removePrivilegedRuntimeStateFiles();
		}
		return 0;
	}

	// If prefix's init is not running, start it up
	if (pidInit == 0)
	{
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				"var/run/shellspawn.sock", 0, true,
				runtimeModeError, sizeof(runtimeModeError)) != 0) {
			fprintf(stderr, "Cannot clear stale shellspawn socket: %s\n",
				runtimeModeError);
			return 1;
		}
		setupWorkdir();
		pidInit = spawnInitProcess();
		putInitPid(pidInit);
		if (!rootless)
		{
			// The namespace-based launcher keeps its existing bounded startup wait.
			for (int i = 0; i < 15; i++)
			{
				struct stat socketStatus;
				errno = 0;
				int statusResult =
					darling_runtime_mode_stat_relative(g_runtimePrefix,
						"var/run/shellspawn.sock", &socketStatus,
						runtimeModeError, sizeof(runtimeModeError));
				if (statusResult == 0) {
					if (!S_ISSOCK(socketStatus.st_mode)) {
						fprintf(stderr,
							"Shellspawn endpoint is not a socket\n");
						return 1;
					}
					break;
				}
				if (errno != ENOENT) {
					fprintf(stderr,
						"Cannot inspect shellspawn endpoint safely: %s\n",
						runtimeModeError);
					return 1;
				}
				sleep(1);
			}
		}
	}

#if USE_LINUX_4_11_HACK
	if (!rootless)
		joinNamespace(pidInit, CLONE_NEWNS, "mnt");
#endif

	if (!rootless)
		seteuid(g_originalUid);

	if (strcmp(argv[commandIndex], "shell") == 0)
	{
		// Spawn the shell
		if (argc > commandIndex + 1)
			spawnShell(pidInit, (const char**) &argv[commandIndex + 1]);
		else
			spawnShell(pidInit, NULL);
	}
	else
	{
		bool doExec = strcmp(argv[commandIndex], "exec") == 0;
		int argvIndex = doExec ? commandIndex + 1 : commandIndex;

		if (doExec && argc <= commandIndex + 1)
		{
			printf("'exec' subcommand requires a binary to execute.\n");
			return 1;
		}

		char *fullPath;
		char *path = realpath(argv[argvIndex], NULL);

		if (path == NULL)
		{
			printf("'%s' is not a supported command or a file.\n", argv[argvIndex]);
			return 1;
		}

		fullPath = malloc(strlen(SYSTEM_ROOT) + strlen(path) + 1);
		strcpy(fullPath, SYSTEM_ROOT);
		strcat(fullPath, path);
		free(path);

		argv[argvIndex] = fullPath;

		if (doExec)
			spawnBinary(pidInit, argv[argvIndex], (const char**) &argv[argvIndex]);
		else
			spawnShell(pidInit, (const char**) &argv[argvIndex]);
	}

	return 0;
}

void joinNamespace(pid_t pid, int type, const char* typeName)
{
	int fdNS;
	char pathNS[4096];
	
	snprintf(pathNS, sizeof(pathNS), "/proc/%d/ns/%s", pid, typeName);

	fdNS = open(pathNS, O_RDONLY);

	if (fdNS < 0)
	{
		fprintf(stderr, "Cannot open %s namespace file: %s\n", typeName, strerror(errno));
		exit(1);
	}

	// Calling setns() with a PID namespace doesn't move our process into it,
	// but our child process will be spawned inside the namespace
	if (setns(fdNS, type) != 0)
	{
		fprintf(stderr, "Cannot join %s namespace: %s\n", typeName, strerror(errno));
		exit(1);
	}
	close(fdNS);
}

static void pushShellspawnCommandData(int sockfd, shellspawn_cmd_type_t type, const void* data, size_t data_length)
{
	struct shellspawn_cmd* cmd;
	size_t length;

	length = sizeof(*cmd) + data_length;

	cmd = (struct shellspawn_cmd*) malloc(length);
	cmd->cmd = type;
	cmd->data_length = data_length;

	if (data != NULL)
		memcpy(cmd->data, data, data_length);

	if (write(sockfd, cmd, length) != length)
	{
		fprintf(stderr, "Error sending command to shellspawn: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	free(cmd);
}

static void pushShellspawnCommand(int sockfd, shellspawn_cmd_type_t type, const char* value)
{
	if (!value)
		pushShellspawnCommandData(sockfd, type, NULL, 0);
	else
		pushShellspawnCommandData(sockfd, type, value, strlen(value) + 1);
}

static void pushShellspawnCommandFDs(int sockfd, shellspawn_cmd_type_t type, const int fds[3])
{
	struct shellspawn_cmd cmd;
	char cmsgbuf[CMSG_SPACE(sizeof(int) * 3)];
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmptr;

	cmd.cmd = type;
	cmd.data_length = 0;

	iov.iov_base = &cmd;

	memset(&msg, 0, sizeof(msg));
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	iov.iov_base = &cmd;
	iov.iov_len = sizeof(cmd);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len = CMSG_LEN(sizeof(int) * 3);
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(cmptr), fds, sizeof(fds[0])*3);

	if (sendmsg(sockfd, &msg, 0) < 0)
	{
		fprintf(stderr, "Error sending command to shellspawn: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
}

static int _shSockfd = -1;
static struct termios orig_termios;
static int pty_master;
static void signalHandler(int signo)
{
	// printf("Received signal %d\n", signo);

	// Forward window size changes
	if (signo == SIGWINCH && pty_master != -1)
	{
		struct winsize win;

		ioctl(0, TIOCGWINSZ, &win);
		ioctl(pty_master, TIOCSWINSZ, &win);
	}
	
	// Foreground process loopkup in shellspawn doesn't work
	// if we're not running in TTY mode, so shellspawn falls back
	// to forwarding signals to the Bash subprocess.
	// 
	// Hence we translate SIGINT to SIGTERM for user convenience,
	// because Bash will not terminate on SIGINT.
	if (pty_master == -1 && signo == SIGINT)
		signo = SIGTERM;

	pushShellspawnCommandData(_shSockfd, SHELLSPAWN_SIGNAL, &signo, sizeof(signo));
}

static void shellLoop(int sockfd, int master)
{
	struct sigaction sa;
	struct pollfd pfds[3];
	const int fdcount = (master != -1) ? 3 : 1; // do we do pty proxying?

	// Vars for signal handler
	_shSockfd = sockfd;
	pty_master = master;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signalHandler;
	sigfillset(&sa.sa_mask);

	for (int i = 1; i < 32; i++)
		sigaction(i, &sa, NULL);

	pfds[2].fd = master;
	pfds[2].events = POLLIN;
	pfds[2].revents = 0;
	pfds[1].fd = STDIN_FILENO;
	pfds[1].events = POLLIN;
	pfds[1].revents = 0;
	pfds[0].fd = sockfd;
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;

	if (master != -1)
		fcntl(master, F_SETFL, O_NONBLOCK);
	//fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	fcntl(sockfd, F_SETFL, O_NONBLOCK);

	while (1)
	{
		char buf[4096];

		if (poll(pfds, fdcount, -1) < 0)
		{
			if (errno != EINTR)
			{
				perror("poll");
				break;
			}
		}

		if (pfds[2].revents & POLLIN)
		{
			int rd;
			do
			{
				rd = read(master, buf, sizeof(buf));
				if (rd > 0)
					write(STDOUT_FILENO, buf, rd);
			}
			while (rd == sizeof(buf));
		}

		if (pfds[1].revents & POLLIN)
		{
			int rd = 0;
			do
			{
				if (ioctl(STDIN_FILENO, FIONREAD, &rd) < 0) {
					perror("ioctl");
					exit(1);
				}
				if (rd > sizeof(buf)) {
					rd = sizeof(buf);
				}
				rd = read(STDIN_FILENO, buf, rd);
				if (rd > 0)
					write(master, buf, rd);
				else {
					perror("read");
					exit(1);
				}
			}
			while (rd == sizeof(buf));
		}

		if (pfds[0].revents & (POLLHUP | POLLIN))
		{
			struct shellspawn_result result;
			if (read(sockfd, &result, sizeof(result)) != sizeof(result))
				exit(1);
			if (result.kind == SHELLSPAWN_RESULT_EXIT)
				exit(result.value);
			if (result.kind == SHELLSPAWN_RESULT_ERROR)
			{
				fprintf(stderr, "shellspawn failed (errno=%d): %s\n", result.value,
					strerror(result.value));
				exit(1);
			}
		fprintf(stderr, "shellspawn returned an unknown result kind: %u\n", result.kind);
		exit(1);
		}
	}
}

static void restoreTermios(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

// Glibc openpty() fails for me on Debian, because grantpty() fails to chown() the pty node with error code EPERM.
// This is a more lenient version of openpty() that just works.
static int openpty_darling(int* amaster, int* aslave, char* name_unused, const struct termios* tos, const struct winsize* wsz)
{
	const char* slave_name;

	*amaster = posix_openpt(O_RDWR);
	if (*amaster == -1)
		return -1;

	grantpt(*amaster);
	if (unlockpt(*amaster) < 0)
		return -1;

	slave_name = ptsname(*amaster);
	*aslave = open(slave_name, O_RDWR | O_NOCTTY);
	if (*aslave == -1)
		return -1;

	if (tos != NULL)
		tcsetattr(*amaster, TCSANOW, tos);
	if (wsz != NULL)
		ioctl(*amaster, TIOCSWINSZ, wsz);

	return 0;
}

static void setupPtys(int fds[3], int* master)
{
	struct winsize win;
	struct termios termios;
	bool tty = true;

	if (tcgetattr(STDIN_FILENO, &termios) < 0)
		tty = false;

	if (openpty_darling(master, &fds[0], NULL, &termios, NULL) < 0)
	{
		perror("openpty");
		exit(1);
	}
	fds[2] = fds[1] = fds[0];

	if (tty)
	{
		orig_termios = termios;

		ioctl(0, TIOCGWINSZ, &win);

		termios.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
		termios.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
				INPCK | ISTRIP | IXON | PARMRK);
		termios.c_oflag &= ~OPOST;
		termios.c_cc[VMIN] = 1;
		termios.c_cc[VTIME] = 0;

		if (tcsetattr(STDIN_FILENO, TCSANOW, &termios) < 0)
		{
			perror("tcsetattr");
			exit(1);
		}
		ioctl(*master, TIOCSWINSZ, &win);

		atexit(restoreTermios);
	}
}

// Replace each quote character (') with the sequence '\''
// (copying the result to dest, if non-null)
// and return the length of the result.
static size_t escapeQuotes(char *dest, const char *src)
{
	size_t len = 0;

	for (; *src != 0; src++)
	{
		if (*src == '\'')
		{
			if (dest)
				memcpy(&dest[len], "'\\''", 4);
			len += 4;
		}
		else
		{
			if (dest)
				dest[len] = *src;
			len++;
		}
	}
	if (dest)
		dest[len] = 0;

	return len;
}

static bool rootlessInitIsRunning(pid_t pidInit)
{
	if (pidInit <= 0)
		return false;

	/* Reap our own completed darlingserver and reject an already-zombie
	 * server owned by another launcher.  kill(pid, 0) alone reports zombies
	 * as live and would keep a boot client in its readiness loop after a
	 * successful concurrent shutdown. */
	int child_status;
	pid_t waited = waitpid(pidInit, &child_status, WNOHANG);
	if (waited == pidInit)
		return false;
	if (waited < 0 && errno != ECHILD && errno != EINTR)
		return false;

	char path[64];
	char stat_line[4096];
	snprintf(path, sizeof(path), "/proc/%d/stat", pidInit);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno == EACCES || errno == EPERM;
	ssize_t length = read(fd, stat_line, sizeof(stat_line) - 1);
	int saved_errno = errno;
	close(fd);
	errno = saved_errno;
	if (length <= 0)
		return false;
	stat_line[length] = '\0';
	char* fields = strrchr(stat_line, ')');
	if (fields == NULL || fields[1] != ' ' || fields[2] == '\0')
		return false;
	if (fields[2] == 'Z' || fields[2] == 'X')
		return false;

	if (kill(pidInit, 0) == 0)
		return true;

	return errno == EPERM;
}

int connectToShellspawn(pid_t pidInit)
{
	struct sockaddr_un addr;
	struct timespec started;
	const long ready_timeout_ms = rootlessShellspawnReadyTimeoutMs();
	char error[512] = {0};
	int socketDirectoryFD = darling_runtime_mode_open_relative_directory(
		g_runtimePrefix, "var/run", false, error, sizeof(error));
	if (socketDirectoryFD < 0) {
		fprintf(stderr, "Cannot retain shellspawn socket directory: %s\n",
			error);
		exit(1);
	}

	// Connect to the shellspawn daemon in the container
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path),
			"/proc/self/fd/%d/shellspawn.sock",
			socketDirectoryFD) >= (int)sizeof(addr.sun_path)) {
		close(socketDirectoryFD);
		fprintf(stderr, "Retained shellspawn socket path is too long\n");
		exit(1);
	}
	if (clock_gettime(CLOCK_MONOTONIC, &started) != 0)
	{
		close(socketDirectoryFD);
		fprintf(stderr, "Unable to start rootless shellspawn readiness timer: %s\n", strerror(errno));
		exit(1);
	}

	for (;;)
	{
		struct stat socketStatus;
		if (fstatat(socketDirectoryFD, "shellspawn.sock", &socketStatus,
				AT_SYMLINK_NOFOLLOW) == 0) {
			if (S_ISLNK(socketStatus.st_mode) ||
				!S_ISSOCK(socketStatus.st_mode)) {
				close(socketDirectoryFD);
				fprintf(stderr,
					"Shellspawn endpoint is a symlink or non-socket\n");
				exit(1);
			}
		} else if (errno != ENOENT) {
			int saved_errno = errno;
			close(socketDirectoryFD);
			fprintf(stderr, "Cannot inspect shellspawn endpoint: %s\n",
				strerror(saved_errno));
			exit(1);
		}
		int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (sockfd == -1)
		{
			close(socketDirectoryFD);
			fprintf(stderr, "Error creating a unix domain socket: %s\n", strerror(errno));
			exit(1);
		}

		if (connect(sockfd, (struct sockaddr*) &addr, sizeof(addr)) == 0)
		{
			close(socketDirectoryFD);
			return sockfd;
		}

		int error = errno;
		close(sockfd);
		if (!rootlessModeEnabled() || (error != ENOENT && error != ECONNREFUSED))
		{
			close(socketDirectoryFD);
			fprintf(stderr, "Error connecting to shellspawn in the container (%s): %s\n", addr.sun_path, strerror(error));
			exit(1);
		}

		if (!rootlessInitIsRunning(pidInit))
		{
			close(socketDirectoryFD);
			fprintf(stderr, "Rootless init process %d exited before shellspawn became ready (%s)\n", pidInit, addr.sun_path);
			exit(1);
		}

		struct timespec now;
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		{
			close(socketDirectoryFD);
			fprintf(stderr, "Unable to read rootless shellspawn readiness timer: %s\n", strerror(errno));
			exit(1);
		}
		long elapsed_ms = (now.tv_sec - started.tv_sec) * 1000L
			+ (now.tv_nsec - started.tv_nsec) / 1000000L;
		if (elapsed_ms >= ready_timeout_ms)
		{
			close(socketDirectoryFD);
			fprintf(stderr, "Rootless shellspawn did not become ready within %ldms (%s)\n",
				ready_timeout_ms, addr.sun_path);
			exit(1);
		}
		poll(NULL, 0, 100);
	}
}

void setupShellspawnEnv(int sockfd)
{
	static const char* skip_vars[] = {
		"PATH",
		"TMPDIR",
		"HOME",
	};

	char buffer2[4096];

	// Push environment variables
	pushShellspawnCommand(sockfd, SHELLSPAWN_SETENV, 
		"PATH=/usr/bin:"
		"/bin:"
		"/usr/sbin:"
		"/sbin:"
		"/usr/local/bin");
	pushShellspawnCommand(sockfd, SHELLSPAWN_SETENV, "TMPDIR=/private/tmp");

	const char* login = NULL;
	struct passwd* pw = getpwuid(geteuid());

	if (pw != NULL)
		login = pw->pw_name;

	if (!login)
		login = getlogin();
	if (!login)
	{
		fprintf(stderr, "Cannot determine your user name\n");
		exit(1);
	}

	snprintf(buffer2, sizeof(buffer2), "HOME=/Users/%s", login);
	pushShellspawnCommand(sockfd, SHELLSPAWN_SETENV, buffer2);

	for (char** var_ptr = environ; *var_ptr != NULL; ++var_ptr) {
		const char* var = *var_ptr;
		const char* equal = strchr(var, '=');
		size_t name_len = (equal != NULL) ? (size_t)(equal - var) : strlen(var);
		bool skip_it = false;

		for (size_t i = 0; i < sizeof(skip_vars) / sizeof(*skip_vars); ++i) {
			if (strlen(skip_vars[i]) == name_len && strncmp(var, skip_vars[i], name_len) == 0) {
				skip_it = true;
				break;
			}
		}

		if (skip_it) {
			continue;
		}

		pushShellspawnCommand(sockfd, SHELLSPAWN_SETENV, var);
	}
}

void setupWorkingDir(int sockfd)
{
	char buffer2[4096];
	snprintf(buffer2, sizeof(buffer2), SYSTEM_ROOT "%s", g_workingDirectory);
	pushShellspawnCommand(sockfd, SHELLSPAWN_CHDIR, buffer2);
}

void setupIDs(int sockfd)
{
	int ids[2] = { g_originalUid, g_originalGid };
	pushShellspawnCommandData(sockfd, SHELLSPAWN_SETUIDGID, ids, sizeof(ids));
}

void setupFDs(int fds[3], int* master)
{
	*master = -1;

	if (isatty(STDIN_FILENO))
		setupPtys(fds, master);
	else
		fds[0] = dup(STDIN_FILENO); // dup() because we close() after spawning
	
	if (*master == -1 || !isatty(STDOUT_FILENO))
		fds[1] = STDOUT_FILENO;
	if (*master == -1 || !isatty(STDERR_FILENO))
		fds[2] = STDERR_FILENO;
}

void spawnGo(int sockfd, int fds[3], int master)
{
	pushShellspawnCommandFDs(sockfd, SHELLSPAWN_GO, fds);
	close(fds[0]);

	shellLoop(sockfd, master);
	
	if (master != -1)
		close(master);
	close(sockfd);
}

void spawnShell(pid_t pidInit, const char** argv)
{
	size_t total_len = 0;
	int count;
	int sockfd;
	char* buffer;
	int fds[3], master;

	if (argv != NULL)
	{
		for (count = 0; argv[count] != NULL; count++)
			total_len += escapeQuotes(NULL, argv[count]);

		buffer = malloc(total_len + count*3);

		char *to = buffer;
		for (int i = 0; argv[i] != NULL; i++)
		{
			if (to != buffer)
				to = stpcpy(to, " ");
			to = stpcpy(to, "'");
			to += escapeQuotes(to, argv[i]);
			to = stpcpy(to, "'");
		}
	}
	else
		buffer = NULL;

	sockfd = connectToShellspawn(pidInit);

	setupShellspawnEnv(sockfd);

	// Push shell arguments
	if (buffer != NULL)
	{
		pushShellspawnCommand(sockfd, SHELLSPAWN_ADDARG, "-c");
		pushShellspawnCommand(sockfd, SHELLSPAWN_ADDARG, buffer);

		free(buffer);
	}

	setupWorkingDir(sockfd);
	setupIDs(sockfd);
	setupFDs(fds, &master);
	spawnGo(sockfd, fds, master);
}

void spawnBinary(pid_t pidInit, const char* binary, const char** argv)
{
	int fds[3], master;
	int sockfd;

	sockfd = connectToShellspawn(pidInit);
	setupShellspawnEnv(sockfd);

	pushShellspawnCommand(sockfd, SHELLSPAWN_SETEXEC, binary);

	for (; *argv != NULL; ++argv)
		pushShellspawnCommand(sockfd, SHELLSPAWN_ADDARG, *argv);

	setupWorkingDir(sockfd);
	setupIDs(sockfd);
	setupFDs(fds, &master);
	spawnGo(sockfd, fds, master);
}

void showHelp(const char* argv0)
{
	fprintf(stderr, "This is Darling, translation layer for macOS software.\n\n");
	fprintf(stderr, "Copyright (C) 2012-2023 Lubos Dolezel\n\n");

	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s [--rootless] <program-path> [arguments...]\n", argv0);
	fprintf(stderr, "\t%s [--rootless] shell [arguments...]\n", argv0);
	fprintf(stderr, "\t%s [--rootless] exec <program-path> [arguments...]\n", argv0);
	fprintf(stderr, "\t%s [--rootless] shutdown\n", argv0);
	fprintf(stderr, "\t%s [--rootless] --confirm-prefix-recreate"
		" recreate-prefix\n", argv0);
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n"
		"--rootless - select rootless-eunion (exact spelling; requires an E-UNION-capable build)\n"
		"--confirm-prefix-recreate - explicit destructive confirmation;"
		" valid only with recreate-prefix\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Environment variables:\n"
		"DPREFIX - specifies the location of Darling prefix, defaults to ~/.darling\n"
		"DARLING_RUNTIME_MODE - canonical internal runtime mode; prefer --rootless\n"
		"DARLING_ROOTLESS, DARLING_NOOVERLAYFS, DARLING_EUNION - launcher-only compatibility inputs\n");
}

void showVersion(const char* argv0) {
	fprintf(stderr, "%s " GIT_BRANCH " @ " GIT_COMMIT_HASH "\n", argv0);
	fprintf(stderr, "Copyright (C) 2012-2023 Lubos Dolezel\n");
}

void missingSetuidRoot(void)
{
	char path[4096];
	int len;

	len = readlink("/proc/self/exe", path, sizeof(path)-1);
	if (len < 0)
		strcpy(path, "darling");
	else
		path[len] = '\0';

	fprintf(stderr, "Sorry, the `%s' binary is not setuid root, which is mandatory.\n", path);
	fprintf(stderr, "Darling needs this in order to create mount and PID namespaces and to perform mounts.\n");
}

pid_t spawnInitProcess(void)
{
	pid_t pid;
	int pipefd[2];
	char buffer[1];
	char error[512] = {0};
	struct rootless_shutdown_closure_capability shutdown_closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;

	if (darling_runtime_mode_verify_prefix_name(g_runtimePrefix,
			error, sizeof(error)) != 0 ||
		g_runtimePrefix->workdir_fd < 0 ||
		g_runtimePrefix->sidecar_fd < 0 ||
		g_runtimePrefix->lifecycle_lock_fd < 0) {
		fprintf(stderr,
			"Cannot hand the retained Darling prefix to darlingserver: %s\n",
			error[0] == '\0' ? "runtime workdir fd is missing" : error);
			exit(1);
	}
	if (rootlessModeEnabled() && rootless_shutdown_prepare_closure(
			g_runtimePrefix, &shutdown_closure,
			error, sizeof(error)) != 0) {
		fprintf(stderr,
			"Cannot prepare the rootless shutdown closure: %s\n", error);
		exit(1);
	}

	if (pipe(pipefd) == -1)
	{
		fprintf(stderr, "Cannot create a pipe for synchronization: %s\n", strerror(errno));
		if (rootlessModeEnabled())
			(void)rootless_shutdown_cleanup_empty_closure(
				&shutdown_closure, NULL, 0);
		exit(1);
	}

	if (!rootlessModeEnabled())
	{
		if (unshare(CLONE_NEWUTS | CLONE_NEWIPC) != 0)
		{
			fprintf(stderr, "Cannot unshare UTS and IPC namespaces to create darling-init: %s\n", strerror(errno));
			exit(1);
		}
	}

	pid = fork();

	if (pid < 0)
	{
		fprintf(stderr, "Cannot fork() to create darling-init: %s\n", strerror(errno));
		if (rootlessModeEnabled())
			(void)rootless_shutdown_cleanup_empty_closure(
				&shutdown_closure, NULL, 0);
		exit(1);
	}

	if (pid == 0)
	{
		// The child
		if (rootlessModeEnabled() && rootless_shutdown_enter_closure(
				&shutdown_closure, error, sizeof(error)) != 0) {
			fprintf(stderr,
				"Cannot enter the rootless shutdown closure: %s\n", error);
			_exit(1);
		}
		rootless_shutdown_release_closure(&shutdown_closure);

		char uid_str[21];
		char gid_str[21];
		char pipefd_str[21];
		char prefixfd_str[21];
		char parentfd_str[21];
		char workdirfd_str[21];
		char sidecarfd_str[21];
		char lockfd_str[21];

		snprintf(uid_str, sizeof(uid_str), "%d", g_originalUid);
		snprintf(gid_str, sizeof(gid_str), "%d", g_originalGid);
		snprintf(pipefd_str, sizeof(pipefd_str), "%d", pipefd[1]);
		snprintf(prefixfd_str, sizeof(prefixfd_str), "%d",
			g_runtimePrefix->directory_fd);
		snprintf(parentfd_str, sizeof(parentfd_str), "%d",
			g_runtimePrefix->parent_fd);
		snprintf(workdirfd_str, sizeof(workdirfd_str), "%d",
			g_runtimePrefix->workdir_fd);
		snprintf(sidecarfd_str, sizeof(sidecarfd_str), "%d",
			g_runtimePrefix->sidecar_fd);
		snprintf(lockfd_str, sizeof(lockfd_str), "%d",
			g_runtimePrefix->lifecycle_lock_fd);

		close(pipefd[0]);
		if (darling_runtime_mode_make_fd_inheritable(
				g_runtimePrefix->directory_fd, error, sizeof(error)) != 0 ||
			darling_runtime_mode_make_fd_inheritable(
				g_runtimePrefix->parent_fd, error, sizeof(error)) != 0 ||
			darling_runtime_mode_make_fd_inheritable(
				g_runtimePrefix->workdir_fd, error, sizeof(error)) != 0 ||
			darling_runtime_mode_make_fd_inheritable(
				g_runtimePrefix->sidecar_fd, error, sizeof(error)) != 0 ||
			darling_runtime_mode_make_lock_fd_inheritable(
				g_runtimePrefix->lifecycle_lock_fd,
				error, sizeof(error)) != 0) {
			fprintf(stderr,
				"Cannot preserve Darling prefix descriptors for darlingserver: %s\n",
				error);
			_exit(1);
		}

		execl(INSTALL_PREFIX "/bin/darlingserver", "darlingserver",
			prefixfd_str, parentfd_str, g_runtimePrefix->leaf,
			workdirfd_str, sidecarfd_str, lockfd_str,
			uid_str, gid_str, pipefd_str,
			g_fixPermissions ? "1" : "0", NULL);

		fprintf(stderr, "Failed to start darlingserver\n");
		exit(1);
	}

	// Wait for the child to drop UID/GIDs and unshare stuff
	close(pipefd[1]);
	ssize_t synchronization = read(pipefd[0], buffer, 1);
	close(pipefd[0]);
	if (rootlessModeEnabled() &&
		(synchronization != 1 || rootless_shutdown_closure_contains(
			&shutdown_closure, pid, error, sizeof(error)) != 0)) {
		fprintf(stderr,
			"Darlingserver did not retain its rootless shutdown closure: %s\n",
			error[0] == '\0' ? "startup handshake failed" : error);
		kill(pid, SIGKILL);
		(void)waitpid(pid, NULL, 0);
		(void)rootless_shutdown_cleanup_empty_closure(
			&shutdown_closure, NULL, 0);
		exit(1);
	}
	rootless_shutdown_release_closure(&shutdown_closure);

	/*
	snprintf(idmap, sizeof(idmap), "/proc/%d/uid_map", pid);

	file = fopen(idmap, "w");
	if (file != NULL)
	{
		fprintf(file, "0 %d 1\n", g_originalUid); // all users map to our user on the outside
		fclose(file);
	}
	else
	{
		fprintf(stderr, "Cannot set uid_map for the init process: %s\n", strerror(errno));
	}

	snprintf(idmap, sizeof(idmap), "/proc/%d/gid_map", pid);

	file = fopen(idmap, "w");
	if (file != NULL)
	{
		fprintf(file, "0 %d 1\n", g_originalGid); // all groups map to our group on the outside
		fclose(file);
	}
	else
	{
		fprintf(stderr, "Cannot set gid_map for the init process: %s\n", strerror(errno));
	}
	*/

	// Here's where we resume the child
	// if we enable user namespaces

	return pid;
}

void putInitPid(pid_t pidInit)
{
	char content[64];
	char error[512] = {0};
	int length = snprintf(content, sizeof(content), "%d\n", (int)pidInit);
	if (length < 0 || (size_t)length >= sizeof(content)) {
		fprintf(stderr, "Cannot format init PID\n");
		return;
	}

	if (!rootlessModeEnabled()) {
		seteuid(g_originalUid);
		setegid(g_originalGid);
	}

	int result = darling_runtime_mode_write_relative_atomic(
		g_runtimePrefix, ".init.pid", content, 0644,
		error, sizeof(error));

	if (!rootlessModeEnabled()) {
		seteuid(0);
		setegid(0);
	}

	if (result != 0) {
		fprintf(stderr, "Cannot write out PID of the init process: %s\n",
			error);
		return;
	}
}

char* defaultPrefixPath(void)
{
	const char defaultPath[] = "/.darling";
	const char* home = getenv("HOME");
	char* buf;

	if (!home)
	{
		fprintf(stderr, "Cannot detect your home directory!\n");
		return NULL;
	}

	buf = (char*) malloc(strlen(home) + sizeof(defaultPath));
	strcpy(buf, home);
	strcat(buf, defaultPath);

	return buf;
}

void setupWorkdir()
{
	char error[512] = {0};
	if (darling_runtime_mode_prepare_workdir(g_runtimePrefix,
			error, sizeof(error)) != 0) {
		fprintf(stderr, "Cannot prepare Darling runtime workdir: %s\n",
			error);
		exit(1);
	}
}

void setupPrefix()
{
	struct passwd* passwd_entry;
	char error[512] = {0};

	fprintf(stderr, "Setting up a new fd-anchored Darling prefix (%s)\n",
		g_runtimePrefix->leaf);

	if (!rootlessModeEnabled()) {
		seteuid(g_originalUid);
		setegid(g_originalGid);
	}

	passwd_entry = getpwuid(g_originalUid);
	if (!passwd_entry) {
		fprintf(stderr, "Failed to find Linux /etc/passwd entry for current user\n");
		exit(1);
	}

	if (darling_runtime_mode_setup_prefix(
			g_runtimePrefix,
			passwd_entry->pw_name,
			passwd_entry->pw_uid,
			passwd_entry->pw_gid,
			error,
			sizeof(error)
		) != 0) {
		fprintf(stderr, "Failed to initialize Darling prefix safely: %s\n",
			error);
		exit(1);
	}
	
	if (!rootlessModeEnabled()) {
		seteuid(0);
		setegid(0);
	}
}

pid_t getInitProcess()
{
	pid_t pid;
	long parsed_pid;
	char pidBuffer[64];
	char* pidEnd = NULL;
	char error[512] = {0};
	char procBuf[100];
	FILE *fp;
	char *exeBuf, *statusBuf;
	int uidMatch = 0, gidMatch = 0;

	int pidFD = darling_runtime_mode_open_relative_file(
		g_runtimePrefix, ".init.pid", O_RDONLY, 0,
		error, sizeof(error));
	if (pidFD < 0) {
		if (errno == ENOENT)
			return 0;
		fprintf(stderr, "Cannot safely read prefix init PID: %s\n", error);
		exit(1);
	}
	struct stat pidStatus;
	ssize_t pidLength;
	if (fstat(pidFD, &pidStatus) != 0 ||
		!S_ISREG(pidStatus.st_mode) ||
		pidStatus.st_size <= 0 ||
		pidStatus.st_size >= (off_t)sizeof(pidBuffer) ||
		(pidLength = read(pidFD, pidBuffer,
			(size_t)pidStatus.st_size)) != pidStatus.st_size ||
		close(pidFD) != 0) {
		int saved_errno = errno;
		close(pidFD);
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				".init.pid", 0, false, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove invalid prefix init PID: %s\n",
				error);
			exit(1);
		}
		errno = saved_errno;
		return 0;
	}
	pidBuffer[pidLength] = '\0';
	errno = 0;
	parsed_pid = strtol(pidBuffer, &pidEnd, 10);
	while (pidEnd != NULL && (*pidEnd == '\n' || *pidEnd == '\r'))
		pidEnd++;
	if (errno != 0 || pidEnd == pidBuffer || pidEnd == NULL ||
		*pidEnd != '\0' || parsed_pid <= 0 || (pid_t)parsed_pid != parsed_pid) {
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				".init.pid", 0, false, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove malformed prefix init PID: %s\n",
				error);
			exit(1);
		}
		return 0;
	}
	pid = (pid_t)parsed_pid;

	// Does the process exist?
	if (kill(pid, 0) == -1)
	{
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				".init.pid", 0, false, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove stale prefix init PID: %s\n",
				error);
			exit(1);
		}
		return 0;
	}

	// Is it actually an init process?
	snprintf(procBuf, sizeof(procBuf), "/proc/%d/comm", pid);
	fp = fopen(procBuf, "r");
	if (fp == NULL)
	{
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				".init.pid", 0, false, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove unverifiable prefix init PID: %s\n",
				error);
			exit(1);
		}
		return 0;
	}

	if (fscanf(fp, "%ms", &exeBuf) != 1)
	{
		fclose(fp);
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				".init.pid", 0, false, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove unreadable prefix init PID: %s\n",
				error);
			exit(1);
		}
		return 0;
	}
	fclose(fp);

	if (strcmp(exeBuf, "darlingserver") != 0)
	{
		if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
				".init.pid", 0, false, error, sizeof(error)) != 0) {
			fprintf(stderr, "Cannot remove foreign prefix init PID: %s\n",
				error);
			exit(1);
		}
		return 0;
	}
	free(exeBuf);

	// Is it owned by the current user?
	if (g_originalUid != 0)
	{
		snprintf(procBuf, sizeof(procBuf), "/proc/%d/status", pid);
		fp = fopen(procBuf, "r");
		if (fp == NULL)
		{
			if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
					".init.pid", 0, false,
					error, sizeof(error)) != 0) {
				fprintf(stderr,
					"Cannot remove unowned prefix init PID: %s\n",
					error);
				exit(1);
			}
			return 0;
		}

		while (1)
		{
			statusBuf = NULL;
			size_t len;
			if (getline(&statusBuf, &len, fp) == -1)
				break;
			int rid, eid, sid, fid;
			if (sscanf(statusBuf, "Uid: %d %d %d %d", &rid, &eid, &sid, &fid) == 4)
			{
				uidMatch = 1;
				uidMatch &= rid == g_originalUid;
				uidMatch &= eid == g_originalUid;
				uidMatch &= sid == g_originalUid;
				uidMatch &= fid == g_originalUid;
			}
			if (sscanf(statusBuf, "Gid: %d %d %d %d", &rid, &eid, &sid, &fid) == 4)
			{
				gidMatch = 1;
				gidMatch &= rid == g_originalGid;
				gidMatch &= eid == g_originalGid;
				gidMatch &= sid == g_originalGid;
				gidMatch &= fid == g_originalGid;
			}
			free(statusBuf);
		}
		fclose(fp);

		if (!uidMatch || !gidMatch)
		{
			if (darling_runtime_mode_unlink_relative(g_runtimePrefix,
					".init.pid", 0, false,
					error, sizeof(error)) != 0) {
				fprintf(stderr,
					"Cannot remove mismatched prefix init PID: %s\n",
					error);
				exit(1);
			}
			return 0;
		}
	}

	return pid;
}

void checkPrefixOwner()
{
	struct stat st;

	if (g_runtimePrefix->directory_fd < 0)
	{
		fprintf(stderr, "Darling prefix lifecycle fd is not open.\n");
		exit(1);
	}
	if (fstat(g_runtimePrefix->directory_fd, &st) == 0)
	{
		if (g_originalUid != 0 && st.st_uid != g_originalUid)
		{
			fprintf(stderr, "You do not own the prefix directory.\n");
			exit(1);
		}
	}
	else
	{
		fprintf(stderr, "Cannot inspect the retained prefix directory: %s\n",
			strerror(errno));
		exit(1);
	}
}
