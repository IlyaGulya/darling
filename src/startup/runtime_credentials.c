#define _GNU_SOURCE 1

#include "runtime_credentials.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int credentials_error(
	char* error,
	size_t error_size,
	const char* message
)
{
	if (error != NULL && error_size != 0)
		snprintf(error, error_size, "%s", message);
	return -1;
}

int darling_runtime_verify_rootless_credentials(
	uid_t uid,
	gid_t gid,
	char* error,
	size_t error_size
)
{
	if (uid == 0 || gid == 0)
		return credentials_error(error, error_size,
			"rootless mode requires non-root invoking user and group IDs");

	uid_t real_uid;
	uid_t effective_uid;
	uid_t saved_uid;
	gid_t real_gid;
	gid_t effective_gid;
	gid_t saved_gid;
	if (getresuid(&real_uid, &effective_uid, &saved_uid) != 0 ||
		getresgid(&real_gid, &effective_gid, &saved_gid) != 0) {
		if (error != NULL && error_size != 0)
			snprintf(error, error_size,
				"cannot verify rootless process credentials: %s",
				strerror(errno));
		return -1;
	}
	if (real_uid != uid || effective_uid != uid || saved_uid != uid ||
		real_gid != gid || effective_gid != gid || saved_gid != gid)
		return credentials_error(error, error_size,
			"rootless process retained unexpected real, effective, or saved credentials");
	return 0;
}

int darling_runtime_drop_rootless_credentials(
	uid_t uid,
	gid_t gid,
	char* error,
	size_t error_size
)
{
	if (uid == 0 || gid == 0)
		return credentials_error(error, error_size,
			"rootless mode requires non-root invoking user and group IDs");

	if (setresgid(gid, gid, gid) != 0) {
		if (error != NULL && error_size != 0)
			snprintf(error, error_size,
				"cannot permanently drop rootless group credentials: %s",
				strerror(errno));
		return -1;
	}
	if (setresuid(uid, uid, uid) != 0) {
		if (error != NULL && error_size != 0)
			snprintf(error, error_size,
				"cannot permanently drop rootless user credentials: %s",
				strerror(errno));
		return -1;
	}
	return darling_runtime_verify_rootless_credentials(
		uid, gid, error, error_size);
}
