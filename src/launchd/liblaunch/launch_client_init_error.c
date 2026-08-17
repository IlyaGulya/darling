#include "launch_client_init_error.h"

#include <errno.h>
#include <servers/bootstrap.h>

int
launch_client_getsocket_errno(int result, bool path_nonempty)
{
	if (result == BOOTSTRAP_SUCCESS && path_nonempty) {
		return 0;
	}
	if (result == BOOTSTRAP_NOT_PRIVILEGED) {
		return EPERM;
	}
	return ENOTCONN;
}

int
launch_client_preserve_init_errno(int current, int candidate)
{
	return current != 0 ? current : candidate;
}

bool
launch_client_init_allows_connect(int init_errno)
{
	return init_errno == 0;
}

int
launch_client_require_connection(
	pthread_once_t *once,
	void (*initialize)(void),
	const int *init_errno,
	bool (*is_connected)(void *),
	void *context)
{
	pthread_once(once, initialize);
	if (is_connected(context)) {
		return 0;
	}
	errno = *init_errno != 0 ? *init_errno : ENOTCONN;
	return -1;
}
