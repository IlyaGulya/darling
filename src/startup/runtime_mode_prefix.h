#ifndef DARLING_RUNTIME_MODE_PREFIX_H
#define DARLING_RUNTIME_MODE_PREFIX_H

#include "runtime_mode.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

struct darling_runtime_prefix {
	int directory_fd;
	int parent_fd;
	int workdir_fd;
	char leaf[NAME_MAX + 1];
	char workdir_leaf[NAME_MAX + 1];
	bool existed;
	bool empty;
};

#define DARLING_RUNTIME_PREFIX_INITIALIZER \
	{ .directory_fd = -1, .parent_fd = -1, .workdir_fd = -1, \
		.leaf = {0}, .workdir_leaf = {0}, \
		.existed = false, .empty = false }

int darling_runtime_mode_open_prefix(
	const char* prefix,
	struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
);

void darling_runtime_mode_close_prefix(
	struct darling_runtime_prefix* handle
);

int darling_runtime_mode_setup_prefix(
	struct darling_runtime_prefix* handle,
	const char* user_name,
	uid_t user_id,
	gid_t group_id,
	char* error,
	size_t error_size
);

int darling_runtime_mode_initialize_prefix_marker(
	const struct darling_runtime_prefix* handle,
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_validate_prefix_marker(
	const struct darling_runtime_prefix* handle,
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_prefix_needs_initialization(
	const struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
);

int darling_runtime_mode_verify_prefix_name(
	const struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
);

int darling_runtime_mode_prepare_workdir(
	struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
);

int darling_runtime_mode_open_relative_directory(
	const struct darling_runtime_prefix* handle,
	const char* relative,
	bool create,
	char* error,
	size_t error_size
);

int darling_runtime_mode_open_relative_file(
	const struct darling_runtime_prefix* handle,
	const char* relative,
	int flags,
	mode_t mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_stat_relative(
	const struct darling_runtime_prefix* handle,
	const char* relative,
	struct stat* status,
	char* error,
	size_t error_size
);

int darling_runtime_mode_unlink_relative(
	const struct darling_runtime_prefix* handle,
	const char* relative,
	int flags,
	bool missing_ok,
	char* error,
	size_t error_size
);

int darling_runtime_mode_write_relative_atomic(
	const struct darling_runtime_prefix* handle,
	const char* relative,
	const char* content,
	mode_t mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_make_fd_inheritable(
	int fd,
	char* error,
	size_t error_size
);

#endif
