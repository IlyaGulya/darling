#ifndef DARLING_RUNTIME_CREDENTIALS_H
#define DARLING_RUNTIME_CREDENTIALS_H

#include <stddef.h>
#include <sys/types.h>

int darling_runtime_drop_rootless_credentials(
	uid_t uid,
	gid_t gid,
	char* error,
	size_t error_size
);

int darling_runtime_verify_rootless_credentials(
	uid_t uid,
	gid_t gid,
	char* error,
	size_t error_size
);

#endif
