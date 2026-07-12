#include "rootless_shutdown.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int session_for_pid(pid_t pid, pid_t* session)
{
	char path[64];
	char stat_line[4096];
	FILE* file;
	char* fields;
	char state;
	int parent;
	int group;
	int parsed_session;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	file = fopen(path, "r");
	if (file == NULL)
		return -errno;
	if (fgets(stat_line, sizeof(stat_line), file) == NULL) {
		int error = errno;
		fclose(file);
		return -error;
	}
	fclose(file);
	fields = strrchr(stat_line, ')');
	if (fields == NULL ||
		sscanf(fields + 1, " %c %d %d %d", &state, &parent, &group,
			&parsed_session) != 4)
		return -EINVAL;
	*session = parsed_session;
	return 0;
}

static int signal_session_members(pid_t session, int signal_number)
{
	DIR* proc = opendir("/proc");
	struct dirent* entry;
	int signalled = 0;

	if (proc == NULL)
		return -errno;
	while ((entry = readdir(proc)) != NULL) {
		char* end;
		long value;
		pid_t pid;
		pid_t candidate_session;

		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;
		value = strtol(entry->d_name, &end, 10);
		if (*end != '\0' || value <= 0)
			continue;
		pid = (pid_t)value;
		if (pid == getpid() || session_for_pid(pid, &candidate_session) != 0 ||
			candidate_session != session)
			continue;
		if (kill(pid, signal_number) == 0 || errno == ESRCH)
			signalled++;
	}
	closedir(proc);
	return signalled;
}

int shutdown_rootless_process_session(pid_t member)
{
	pid_t session;
	int result = session_for_pid(member, &session);

	if (result != 0)
		return result;
	result = signal_session_members(session, SIGTERM);
	if (result < 0)
		return result;
	usleep(100000);
	result = signal_session_members(session, SIGKILL);
	return result < 0 ? result : 0;
}
