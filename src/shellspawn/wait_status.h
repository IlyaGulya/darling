/*
 * Shared exit-status boundary for shellspawn and its host regression test.
 */
#ifndef DARLING_SHELLSPAWN_WAIT_STATUS_H
#define DARLING_SHELLSPAWN_WAIT_STATUS_H

#include <sys/wait.h>

static inline int shellspawn_exit_code_from_wait_status(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 1;
}

#endif
