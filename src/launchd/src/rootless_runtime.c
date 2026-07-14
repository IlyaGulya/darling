/*
 * Copyright (c) 2026 Darling contributors.
 *
 * Rootless Darling runs without the privileged prefix setup path. Keep the
 * product-owned launchd prerequisites here so every launcher gets the same
 * clean-prefix behavior instead of reproducing it in an external runner.
 */

#include "rootless_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int ensure_directory(const char *path, mode_t mode)
{
	char current[4096];
	size_t length = strlen(path);

	if (length == 0 || length >= sizeof(current)) {
		return ENAMETOOLONG;
	}
	memcpy(current, path, length + 1);

	for (char *cursor = current + 1; *cursor != '\0'; ++cursor) {
		if (*cursor != '/') {
			continue;
		}
		*cursor = '\0';
		if (mkdir(current, mode) == -1 && errno != EEXIST) {
			return errno;
		}
		*cursor = '/';
	}
	if (mkdir(current, mode) == -1 && errno != EEXIST) {
		return errno;
	}

	struct stat status;
	if (stat(current, &status) == -1) {
		return errno;
	}
	if (!S_ISDIR(status.st_mode)) {
		return ENOTDIR;
	}
	if (chmod(current, mode) == -1) {
		return errno;
	}
	return 0;
}

int rootless_runtime_prepare(void)
{
	static const struct {
		const char *path;
		mode_t mode;
	} directories[] = {
		{ "/private/var/db/launchd.db/com.apple.launchd", 0755 },
		{ "/private/tmp", 01777 },
		{ "/private/var/tmp", 01777 },
		{ "/var/run", 0755 },
		{ "/var/tmp", 01777 },
		{ "/tmp", 01777 },
	};

	for (size_t index = 0; index < sizeof(directories) / sizeof(directories[0]); ++index) {
		int error = ensure_directory(directories[index].path, directories[index].mode);
		if (error != 0) {
			fprintf(stderr, "rootless runtime: cannot prepare %s: %s\n",
				directories[index].path, strerror(error));
			return error;
		}
	}
	return 0;
}
