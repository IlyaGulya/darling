#define _GNU_SOURCE 1

#include "runtime_mode_prefix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef DARLING_RUNTIME_PREFIX_LIFECYCLE_TESTING
const char* darling_runtime_prefix_test_mountinfo_path;
#endif

#define DARLING_RUNTIME_PREFIX_PATH_MAX 4096

static unsigned long directory_sequence;

static int prefix_error(
	char* error,
	size_t error_size,
	const char* format,
	const char* detail
)
{
	if (error != NULL && error_size != 0)
		snprintf(error, error_size, format, detail);
	return -1;
}

static void reset_handle_to(
	darling_runtime_prefix handle,
	enum darling_runtime_prefix_anchor_state state
)
{
	handle->directory_fd = -1;
	handle->parent_fd = -1;
	handle->workdir_fd = -1;
	handle->sidecar_fd = -1;
	handle->lifecycle_lock_fd = -1;
	handle->leaf[0] = '\0';
	handle->workdir_leaf[0] = '\0';
	handle->sidecar_leaf[0] = '\0';
	handle->anchor_state = state;
}

static void reset_handle(darling_runtime_prefix handle)
{
	reset_handle_to(handle, DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED);
}

void darling_runtime_mode_close_prefix(
	darling_runtime_prefix handle
)
{
	if (handle == NULL)
		return;
	if (handle->directory_fd >= 0)
		close(handle->directory_fd);
	if (handle->parent_fd >= 0)
		close(handle->parent_fd);
	if (handle->workdir_fd >= 0)
		close(handle->workdir_fd);
	if (handle->sidecar_fd >= 0)
		close(handle->sidecar_fd);
	if (handle->lifecycle_lock_fd >= 0)
		close(handle->lifecycle_lock_fd);
	reset_handle(handle);
}

int darling_runtime_prefix_move(
	darling_runtime_prefix destination,
	darling_runtime_prefix source,
	char* error,
	size_t error_size
)
{
	if (destination == NULL || source == NULL || destination == source ||
		destination->anchor_state !=
			DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED ||
		destination->directory_fd >= 0 || destination->parent_fd >= 0 ||
		destination->workdir_fd >= 0 ||
		destination->sidecar_fd >= 0 ||
		destination->lifecycle_lock_fd >= 0 ||
		source->anchor_state ==
			DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED ||
		source->anchor_state == DARLING_RUNTIME_PREFIX_ANCHOR_MOVED)
		return prefix_error(error, error_size,
			"runtime prefix capability move is invalid: %s", "invalid");
	memcpy(destination, source, sizeof(*destination));
	reset_handle_to(source, DARLING_RUNTIME_PREFIX_ANCHOR_MOVED);
	return 0;
}

static int compare_opened_directory(
	int fd,
	const struct stat* before,
	const char* component,
	char* error,
	size_t error_size
)
{
	struct stat opened;
	if (fstat(fd, &opened) != 0 ||
		opened.st_dev != before->st_dev ||
		opened.st_ino != before->st_ino ||
		!S_ISDIR(opened.st_mode)) {
		int saved_errno = errno == 0 ? EAGAIN : errno;
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime prefix component changed during inspection: %s",
			component);
	}
	return 0;
}

static int create_and_open_directory(
	int parent_fd,
	const char* name,
	mode_t mode,
	char* error,
	size_t error_size
)
{
	char temporary[NAME_MAX + 1];
	int length = snprintf(temporary, sizeof(temporary),
		".darling-dir-%ld-%lu", (long)getpid(), ++directory_sequence);
	if (length < 0 || (size_t)length >= sizeof(temporary))
		return prefix_error(error, error_size,
			"runtime directory temporary name is too long: %s", name);
	if (mkdirat(parent_fd, temporary, mode) != 0)
		return prefix_error(error, error_size,
			"cannot create runtime directory temporary: %s",
			strerror(errno));
	struct stat created;
	if (fstatat(parent_fd, temporary, &created,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		!S_ISDIR(created.st_mode) ||
		S_ISLNK(created.st_mode)) {
		int saved_errno = errno == 0 ? EAGAIN : errno;
		unlinkat(parent_fd, temporary, AT_REMOVEDIR);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime directory temporary was replaced: %s", name);
	}
	int fd = openat(parent_fd, temporary,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0 ||
		compare_opened_directory(fd, &created, temporary,
			error, error_size) != 0) {
		int saved_errno = errno;
		if (fd >= 0)
			close(fd);
		unlinkat(parent_fd, temporary, AT_REMOVEDIR);
		errno = saved_errno;
		return -1;
	}
	if (renameat2(parent_fd, temporary, parent_fd, name,
			RENAME_NOREPLACE) != 0) {
		int saved_errno = errno;
		close(fd);
		unlinkat(parent_fd, temporary, AT_REMOVEDIR);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot publish runtime directory without replacement: %s",
			strerror(errno));
	}
	struct stat published;
	if (fstatat(parent_fd, name, &published, AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(published.st_mode) ||
		!S_ISDIR(published.st_mode) ||
		published.st_dev != created.st_dev ||
		published.st_ino != created.st_ino) {
		int saved_errno = errno == 0 ? EAGAIN : errno;
		close(fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime directory changed during publication: %s", name);
	}
	return fd;
}

static int inspect_empty(
	int fd,
	bool* empty,
	char* error,
	size_t error_size
)
{
	int duplicate = dup(fd);
	if (duplicate < 0)
		return prefix_error(error, error_size,
			"cannot duplicate runtime prefix directory: %s",
			strerror(errno));
	DIR* directory = fdopendir(duplicate);
	if (directory == NULL) {
		int saved_errno = errno;
		close(duplicate);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot enumerate runtime prefix: %s", strerror(errno));
	}

	bool observed_empty = true;
	errno = 0;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0) {
			observed_empty = false;
			break;
		}
	}
	int read_errno = errno;
	if (closedir(directory) != 0 && read_errno == 0)
		read_errno = errno;
	if (read_errno != 0) {
		errno = read_errno;
		return prefix_error(error, error_size,
			"cannot finish inspecting runtime prefix: %s", strerror(errno));
	}
	*empty = observed_empty;
	return 0;
}

int darling_runtime_mode_open_prefix(
	const char* prefix,
	darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	if (prefix == NULL || *prefix == '\0' || handle == NULL)
		return prefix_error(error, error_size,
			"runtime prefix inspection input is missing: %s", "invalid");

	if (handle->anchor_state !=
			DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED ||
		handle->directory_fd >= 0 || handle->parent_fd >= 0 ||
		handle->workdir_fd >= 0 || handle->leaf[0] != '\0' ||
		handle->sidecar_fd >= 0 || handle->workdir_leaf[0] != '\0' ||
		handle->lifecycle_lock_fd >= 0 ||
		handle->sidecar_leaf[0] != '\0')
		return prefix_error(error, error_size,
			"runtime prefix capability already owns an anchor: %s",
			"invalid");
	size_t length = strlen(prefix);
	if (length >= DARLING_RUNTIME_PREFIX_PATH_MAX)
		return prefix_error(error, error_size,
			"runtime prefix path is too long: %s", prefix);

	char copy[DARLING_RUNTIME_PREFIX_PATH_MAX];
	memcpy(copy, prefix, length + 1);

	char* components[512];
	size_t component_count = 0;
	char* save = NULL;
	for (char* component = strtok_r(copy, "/", &save);
			component != NULL;
			component = strtok_r(NULL, "/", &save)) {
		if (strcmp(component, ".") == 0)
			continue;
		if (strcmp(component, "..") == 0)
			return prefix_error(error, error_size,
				"runtime prefix must not contain '..': %s", prefix);
		if (strlen(component) > NAME_MAX ||
			component_count == sizeof(components) / sizeof(components[0]))
			return prefix_error(error, error_size,
				"runtime prefix has an invalid path component: %s", prefix);
		components[component_count++] = component;
	}

	int current = open(prefix[0] == '/' ? "/" : ".",
		O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (current < 0)
		return prefix_error(error, error_size,
			"cannot open runtime prefix root: %s", strerror(errno));

	if (component_count == 0) {
		close(current);
		return prefix_error(error, error_size,
			"runtime prefix must name a directory below its parent: %s",
			prefix);
	}

	for (size_t index = 0; index < component_count; ++index) {
		const bool leaf = index + 1 == component_count;
		struct stat before;
		if (fstatat(current, components[index], &before,
				AT_SYMLINK_NOFOLLOW) != 0) {
			int saved_errno = errno;
			if (saved_errno == ENOENT && leaf) {
				handle->parent_fd = current;
				memcpy(handle->leaf, components[index],
					strlen(components[index]) + 1);
				if (snprintf(handle->sidecar_leaf,
						sizeof(handle->sidecar_leaf), "%s%s",
						handle->leaf,
						DARLING_RUNTIME_PREFIX_SIDECAR_SUFFIX) >=
					(int)sizeof(handle->sidecar_leaf)) {
					darling_runtime_mode_close_prefix(handle);
					return prefix_error(error, error_size,
						"runtime prefix sidecar name is too long: %s",
						components[index]);
				}
				handle->anchor_state =
					DARLING_RUNTIME_PREFIX_ANCHOR_MISSING;
				return 0;
			}
			close(current);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot inspect runtime prefix component: %s",
				strerror(errno));
		}
		if (S_ISLNK(before.st_mode) || !S_ISDIR(before.st_mode)) {
			close(current);
			return prefix_error(error, error_size,
				"runtime prefix contains a symlink or non-directory component: %s",
				components[index]);
		}

		int next = openat(current, components[index],
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0) {
			int saved_errno = errno;
			close(current);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot open runtime prefix component without following links: %s",
				strerror(errno));
		}
		if (compare_opened_directory(next, &before, components[index],
				error, error_size) != 0) {
			close(next);
			close(current);
			return -1;
		}

		if (leaf) {
			handle->parent_fd = current;
			handle->directory_fd = next;
			memcpy(handle->leaf, components[index],
				strlen(components[index]) + 1);
			if (snprintf(handle->sidecar_leaf,
					sizeof(handle->sidecar_leaf), "%s%s",
					handle->leaf,
					DARLING_RUNTIME_PREFIX_SIDECAR_SUFFIX) >=
				(int)sizeof(handle->sidecar_leaf)) {
				darling_runtime_mode_close_prefix(handle);
				return prefix_error(error, error_size,
					"runtime prefix sidecar name is too long: %s",
					components[index]);
			}
			bool empty = false;
			if (inspect_empty(next, &empty, error, error_size) != 0) {
				darling_runtime_mode_close_prefix(handle);
				return -1;
			}
			handle->anchor_state = empty
				? DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY
				: DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED;
			return 0;
		}
		close(current);
		current = next;
	}

	close(current);
	return prefix_error(error, error_size,
		"runtime prefix has no terminal component: %s", prefix);
}

static int materialize_prefix(
	darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->parent_fd < 0 ||
		handle->leaf[0] == '\0' ||
		handle->anchor_state ==
			DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED ||
		handle->anchor_state == DARLING_RUNTIME_PREFIX_ANCHOR_MOVED)
		return prefix_error(error, error_size,
			"runtime prefix lifecycle handle is incomplete: %s", "invalid");

	if (handle->directory_fd >= 0) {
		struct stat named;
		struct stat opened;
		if (fstatat(handle->parent_fd, handle->leaf, &named,
				AT_SYMLINK_NOFOLLOW) != 0 ||
			fstat(handle->directory_fd, &opened) != 0 ||
			S_ISLNK(named.st_mode) ||
			!S_ISDIR(named.st_mode) ||
			named.st_dev != opened.st_dev ||
			named.st_ino != opened.st_ino)
			return prefix_error(error, error_size,
				"runtime prefix changed before initialization: %s",
				handle->leaf);
		return 0;
	}
	if (handle->anchor_state != DARLING_RUNTIME_PREFIX_ANCHOR_MISSING)
		return prefix_error(error, error_size,
			"runtime prefix capability has no materializable state: %s",
			"invalid");

	struct stat appeared;
	if (fstatat(handle->parent_fd, handle->leaf, &appeared,
			AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
		return prefix_error(error, error_size,
			"missing runtime prefix appeared before initialization: %s",
			handle->leaf);
	int fd = create_and_open_directory(handle->parent_fd, handle->leaf,
		0755, error, error_size);
	if (fd < 0)
		return -1;
	handle->directory_fd = fd;
	handle->anchor_state = DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY;
	return 0;
}

int darling_runtime_mode_verify_prefix_name(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0 ||
		handle->parent_fd < 0 || handle->leaf[0] == '\0' ||
		(handle->anchor_state != DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY &&
		 handle->anchor_state !=
			DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED))
		return prefix_error(error, error_size,
			"runtime prefix lifecycle handle is incomplete: %s", "invalid");

	struct stat named;
	struct stat opened;
	if (fstatat(handle->parent_fd, handle->leaf, &named,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		fstat(handle->directory_fd, &opened) != 0 ||
		S_ISLNK(named.st_mode) ||
		!S_ISDIR(named.st_mode) ||
		!S_ISDIR(opened.st_mode) ||
		named.st_dev != opened.st_dev ||
		named.st_ino != opened.st_ino)
		return prefix_error(error, error_size,
			"runtime prefix name no longer identifies the retained directory: %s",
			handle->leaf);
	return 0;
}

int darling_runtime_mode_prepare_workdir(
	darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	if (darling_runtime_mode_verify_prefix_name(handle,
			error, error_size) != 0)
		return -1;
	struct stat prefix_status;
	if (fstat(handle->directory_fd, &prefix_status) != 0)
		return prefix_error(error, error_size,
			"cannot identify runtime prefix for workdir: %s",
			strerror(errno));
	if (handle->workdir_fd >= 0)
		goto validate_workdir;

	int length = snprintf(handle->workdir_leaf, sizeof(handle->workdir_leaf),
		"%s.workdir", handle->leaf);
	if (length < 0 || (size_t)length >= sizeof(handle->workdir_leaf))
		return prefix_error(error, error_size,
			"runtime workdir name is too long: %s", handle->leaf);

	struct stat before;
	if (fstatat(handle->parent_fd, handle->workdir_leaf, &before,
			AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno != ENOENT)
			return prefix_error(error, error_size,
				"cannot inspect runtime workdir: %s", strerror(errno));
		int fd = create_and_open_directory(
			handle->parent_fd, handle->workdir_leaf, 0755,
			error, error_size);
		if (fd < 0)
			return -1;
		handle->workdir_fd = fd;
		goto validate_workdir;
	}
	if (S_ISLNK(before.st_mode) || !S_ISDIR(before.st_mode))
		return prefix_error(error, error_size,
			"runtime workdir is a symlink or non-directory: %s",
			handle->workdir_leaf);

	int fd = openat(handle->parent_fd, handle->workdir_leaf,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot retain runtime workdir: %s", strerror(errno));
	if (compare_opened_directory(fd, &before, handle->workdir_leaf,
			error, error_size) != 0) {
		close(fd);
		return -1;
	}
	handle->workdir_fd = fd;

validate_workdir:
	struct stat workdir_status;
	if (fstat(handle->workdir_fd, &workdir_status) != 0 ||
		!S_ISDIR(workdir_status.st_mode) ||
		(workdir_status.st_mode & 07777) != 0755 ||
		workdir_status.st_uid != prefix_status.st_uid ||
		workdir_status.st_gid != prefix_status.st_gid)
		return prefix_error(error, error_size,
			"runtime workdir has hostile ownership or mode: %s",
			handle->workdir_leaf);
	return 0;
}

static int open_relative_directory(
	int root_fd,
	const char* relative,
	bool create,
	char* error,
	size_t error_size
)
{
	if (root_fd < 0 || relative == NULL || *relative == '\0' ||
		relative[0] == '/')
		return prefix_error(error, error_size,
			"relative prefix directory is invalid: %s", "invalid");
	if (strlen(relative) >= DARLING_RUNTIME_PREFIX_PATH_MAX)
		return prefix_error(error, error_size,
			"relative prefix directory is too long: %s", relative);

	char copy[DARLING_RUNTIME_PREFIX_PATH_MAX];
	strcpy(copy, relative);
	int current = dup(root_fd);
	if (current < 0)
		return prefix_error(error, error_size,
			"cannot duplicate runtime prefix fd: %s", strerror(errno));

	char* save = NULL;
	for (char* component = strtok_r(copy, "/", &save);
			component != NULL;
			component = strtok_r(NULL, "/", &save)) {
		if (*component == '\0' || strcmp(component, ".") == 0 ||
			strcmp(component, "..") == 0) {
			close(current);
			return prefix_error(error, error_size,
				"relative prefix directory component is invalid: %s",
				component);
		}

		struct stat before;
		if (fstatat(current, component, &before,
				AT_SYMLINK_NOFOLLOW) != 0) {
			int saved_errno = errno;
			if (saved_errno != ENOENT || !create) {
				close(current);
				errno = saved_errno;
				return prefix_error(error, error_size,
					"cannot inspect relative prefix directory: %s",
					strerror(errno));
			}
			int created_fd = create_and_open_directory(
				current, component, 0755, error, error_size);
			if (created_fd < 0) {
				close(current);
				return -1;
			}
			close(current);
			current = created_fd;
			continue;
		}
		if (S_ISLNK(before.st_mode) || !S_ISDIR(before.st_mode)) {
			close(current);
			return prefix_error(error, error_size,
				"relative prefix path is a symlink or non-directory: %s",
				component);
		}
		int next = openat(current, component,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (next < 0) {
			int saved_errno = errno;
			close(current);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot open relative prefix directory: %s",
				strerror(errno));
		}
		if (compare_opened_directory(next, &before, component,
				error, error_size) != 0) {
			close(next);
			close(current);
			return -1;
		}
		close(current);
		current = next;
	}
	return current;
}

int darling_runtime_mode_open_relative_directory(
	const darling_runtime_prefix handle,
	const char* relative,
	bool create,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0)
		return prefix_error(error, error_size,
			"runtime prefix fd is not available: %s", "invalid");
	return open_relative_directory(handle->directory_fd, relative, create,
		error, error_size);
}

static int open_relative_parent(
	const darling_runtime_prefix handle,
	const char* relative,
	char* leaf,
	size_t leaf_size,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0 ||
		relative == NULL || *relative == '\0' ||
		relative[0] == '/' ||
		strlen(relative) >= DARLING_RUNTIME_PREFIX_PATH_MAX)
		return prefix_error(error, error_size,
			"relative prefix path is invalid: %s",
			relative == NULL ? "invalid" : relative);

	char copy[DARLING_RUNTIME_PREFIX_PATH_MAX];
	strcpy(copy, relative);
	char* separator = strrchr(copy, '/');
	const char* name = copy;
	int parent_fd;
	if (separator == NULL) {
		parent_fd = dup(handle->directory_fd);
	} else {
		*separator = '\0';
		name = separator + 1;
		if (copy[0] == '\0') {
			parent_fd = -1;
			errno = EINVAL;
		} else {
			parent_fd = open_relative_directory(handle->directory_fd,
				copy, false, error, error_size);
		}
	}
	if (parent_fd < 0) {
		if (separator == NULL)
			return prefix_error(error, error_size,
				"cannot duplicate runtime prefix fd: %s", strerror(errno));
		return -1;
	}
	if (*name == '\0' || strcmp(name, ".") == 0 ||
		strcmp(name, "..") == 0 || strchr(name, '/') != NULL ||
		strlen(name) >= leaf_size) {
		close(parent_fd);
		return prefix_error(error, error_size,
			"relative prefix leaf is invalid: %s", name);
	}
	strcpy(leaf, name);
	return parent_fd;
}

int darling_runtime_mode_open_relative_file(
	const darling_runtime_prefix handle,
	const char* relative,
	int flags,
	mode_t mode,
	char* error,
	size_t error_size
)
{
	char leaf[NAME_MAX + 1];
	int parent_fd = open_relative_parent(handle, relative,
		leaf, sizeof(leaf), error, error_size);
	if (parent_fd < 0)
		return -1;
	int fd = openat(parent_fd, leaf,
		flags | O_CLOEXEC | O_NOFOLLOW, mode);
	int saved_errno = errno;
	close(parent_fd);
	if (fd < 0) {
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot open relative prefix file: %s", strerror(errno));
	}
	return fd;
}

int darling_runtime_mode_stat_relative(
	const darling_runtime_prefix handle,
	const char* relative,
	struct stat* status,
	char* error,
	size_t error_size
)
{
	if (status == NULL)
		return prefix_error(error, error_size,
			"relative prefix status output is missing: %s", "invalid");
	char leaf[NAME_MAX + 1];
	int parent_fd = open_relative_parent(handle, relative,
		leaf, sizeof(leaf), error, error_size);
	if (parent_fd < 0)
		return -1;
	int result = fstatat(parent_fd, leaf, status, AT_SYMLINK_NOFOLLOW);
	int saved_errno = errno;
	close(parent_fd);
	if (result != 0) {
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot inspect relative prefix entry: %s", strerror(errno));
	}
	if (S_ISLNK(status->st_mode))
		return prefix_error(error, error_size,
			"relative prefix entry is a symlink: %s", leaf);
	return 0;
}

int darling_runtime_mode_unlink_relative(
	const darling_runtime_prefix handle,
	const char* relative,
	int flags,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	char leaf[NAME_MAX + 1];
	int parent_fd = open_relative_parent(handle, relative,
		leaf, sizeof(leaf), error, error_size);
	if (parent_fd < 0)
		return -1;
	struct stat status;
	if (fstatat(parent_fd, leaf, &status, AT_SYMLINK_NOFOLLOW) != 0) {
		int saved_errno = errno;
		close(parent_fd);
		if (missing_ok && saved_errno == ENOENT)
			return 0;
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot inspect relative prefix entry before removal: %s",
			strerror(errno));
	}
	if (S_ISLNK(status.st_mode)) {
		close(parent_fd);
		return prefix_error(error, error_size,
			"refusing to remove a symlink from the runtime prefix: %s",
			leaf);
	}
	if (unlinkat(parent_fd, leaf, flags) != 0) {
		int saved_errno = errno;
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot remove relative prefix entry: %s", strerror(errno));
	}
	close(parent_fd);
	return 0;
}

static int write_all(int fd, const char* content)
{
	size_t remaining = strlen(content);
	while (remaining != 0) {
		ssize_t written = write(fd, content, remaining);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		content += written;
		remaining -= (size_t)written;
	}
	return 0;
}

int darling_runtime_mode_write_relative_atomic(
	const darling_runtime_prefix handle,
	const char* relative,
	const char* content,
	mode_t mode,
	char* error,
	size_t error_size
)
{
	if (content == NULL)
		return prefix_error(error, error_size,
			"relative prefix content is missing: %s", "invalid");
	char leaf[NAME_MAX + 1];
	int parent_fd = open_relative_parent(handle, relative,
		leaf, sizeof(leaf), error, error_size);
	if (parent_fd < 0)
		return -1;

	struct stat existing;
	if (fstatat(parent_fd, leaf, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
		if (S_ISLNK(existing.st_mode) || !S_ISREG(existing.st_mode)) {
			close(parent_fd);
			return prefix_error(error, error_size,
				"refusing to replace a non-regular prefix file: %s", leaf);
		}
	} else if (errno != ENOENT) {
		int saved_errno = errno;
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot inspect relative prefix file before replacement: %s",
			strerror(errno));
	}

	char temporary[NAME_MAX + 1];
	int length = snprintf(temporary, sizeof(temporary),
		".%s.tmp.%ld", leaf, (long)getpid());
	if (length < 0 || (size_t)length >= sizeof(temporary)) {
		close(parent_fd);
		return prefix_error(error, error_size,
			"relative prefix temporary name is too long: %s", leaf);
	}
	int fd = openat(parent_fd, temporary,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
	if (fd < 0) {
		int saved_errno = errno;
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot create relative prefix temporary file: %s",
			strerror(errno));
	}
	if (write_all(fd, content) != 0 || fsync(fd) != 0) {
		int saved_errno = errno;
		close(fd);
		unlinkat(parent_fd, temporary, 0);
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot write relative prefix temporary file: %s",
			strerror(errno));
	}
	struct stat written;
	if (fstat(fd, &written) != 0 || !S_ISREG(written.st_mode)) {
		int saved_errno = errno == 0 ? EINVAL : errno;
		close(fd);
		unlinkat(parent_fd, temporary, 0);
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"relative prefix temporary file is invalid: %s",
			strerror(errno));
	}
	if (close(fd) != 0) {
		int saved_errno = errno;
		unlinkat(parent_fd, temporary, 0);
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot close relative prefix temporary file: %s",
			strerror(errno));
	}
	if (renameat(parent_fd, temporary, parent_fd, leaf) != 0) {
		int saved_errno = errno;
		unlinkat(parent_fd, temporary, 0);
		close(parent_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot publish relative prefix file atomically: %s",
			strerror(errno));
	}
	close(parent_fd);
	return 0;
}

int darling_runtime_mode_make_fd_inheritable(
	int fd,
	char* error,
	size_t error_size
)
{
	struct stat status;
	if (fd < 0 || fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode))
		return prefix_error(error, error_size,
			"cannot inherit invalid runtime directory fd: %s",
			fd < 0 ? "invalid" : strerror(errno));
	int flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0)
		return prefix_error(error, error_size,
			"cannot preserve runtime directory fd across exec: %s",
			strerror(errno));
	return 0;
}

static int fsync_directory(
	int fd,
	const char* detail,
	char* error,
	size_t error_size
);

static int create_regular_file(
	int directory_fd,
	const char* name,
	const char* content,
	char* error,
	size_t error_size
)
{
	int fd = openat(directory_fd, name,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot create prefix initialization file: %s",
			strerror(errno));
	if (write_all(fd, content) != 0) {
		int saved_errno = errno;
		close(fd);
		unlinkat(directory_fd, name, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot write prefix initialization file: %s",
			strerror(errno));
	}
	if (fsync(fd) != 0) {
		int saved_errno = errno;
		close(fd);
		unlinkat(directory_fd, name, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot persist prefix initialization file: %s",
			strerror(errno));
	}
	if (close(fd) != 0)
		return prefix_error(error, error_size,
			"cannot close prefix initialization file: %s",
			strerror(errno));
	return 0;
}

int darling_runtime_mode_setup_prefix(
	darling_runtime_prefix handle,
	const char* user_name,
	uid_t user_id,
	gid_t group_id,
	char* error,
	size_t error_size
)
{
	if (user_name == NULL || *user_name == '\0')
		return prefix_error(error, error_size,
			"runtime prefix user name is missing: %s", "invalid");
	if (materialize_prefix(handle, error, error_size) != 0)
		return -1;

	static const char* directories[] = {
		"Volumes",
		"Applications",
		"usr",
		"usr/local",
		"usr/local/share",
		"private",
		"private/var",
		"private/var/log",
		"private/var/db",
		"private/etc",
		"var",
		"var/run",
		"var/tmp",
		"var/log",
	};
	for (size_t index = 0;
			index < sizeof(directories) / sizeof(directories[0]);
			index++) {
		int fd = open_relative_directory(handle->directory_fd,
			directories[index], true, error, error_size);
		if (fd < 0)
			return -1;
		close(fd);
	}

	int etc_fd = open_relative_directory(handle->directory_fd,
		"private/etc", false, error, error_size);
	if (etc_fd < 0)
		return -1;
	char content[2048];
	int length = snprintf(content, sizeof(content),
		"root:*:0:0:System Administrator:/var/root:/bin/sh\n"
		"%s:*:%u:%u:Darling User:/Users/%s:/bin/bash\n",
		user_name, (unsigned)user_id, (unsigned)group_id, user_name);
	if (length < 0 || (size_t)length >= sizeof(content) ||
		create_regular_file(etc_fd, "passwd", content,
			error, error_size) != 0) {
		close(etc_fd);
		return length < 0 || (size_t)length >= sizeof(content)
			? prefix_error(error, error_size,
				"prefix passwd content is too long: %s", user_name)
			: -1;
	}

	length = snprintf(content, sizeof(content),
		"root:*:0:0::0:0:System Administrator:/var/root:/bin/sh\n"
		"%s:*:%u:%u::0:0:Darling User:/Users/%s:/bin/bash\n",
		user_name, (unsigned)user_id, (unsigned)group_id, user_name);
	if (length < 0 || (size_t)length >= sizeof(content) ||
		create_regular_file(etc_fd, "master.passwd", content,
			error, error_size) != 0) {
		close(etc_fd);
		return length < 0 || (size_t)length >= sizeof(content)
			? prefix_error(error, error_size,
				"prefix master.passwd content is too long: %s", user_name)
			: -1;
	}

	length = snprintf(content, sizeof(content),
		"wheel:*:0:root,%s\n"
		"%s:*:%u:%s\n",
		user_name, user_name, (unsigned)group_id, user_name);
	if (length < 0 || (size_t)length >= sizeof(content) ||
		create_regular_file(etc_fd, "group", content,
			error, error_size) != 0) {
		close(etc_fd);
		return length < 0 || (size_t)length >= sizeof(content)
			? prefix_error(error, error_size,
				"prefix group content is too long: %s", user_name)
			: -1;
	}
	close(etc_fd);
	for (size_t index =
			sizeof(directories) / sizeof(directories[0]);
			index > 0; --index) {
		int fd = open_relative_directory(handle->directory_fd,
			directories[index - 1], false, error, error_size);
		if (fd < 0)
			return -1;
		if (fsync_directory(fd, directories[index - 1],
				error, error_size) != 0) {
			int saved_errno = errno;
			close(fd);
			errno = saved_errno;
			return -1;
		}
		if (close(fd) != 0)
			return prefix_error(error, error_size,
				"cannot close persisted prefix directory: %s",
				strerror(errno));
	}
	if (fsync_directory(handle->directory_fd, "staged prefix root",
			error, error_size) != 0)
		return -1;
	handle->anchor_state = DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED;
	return 0;
}

static int marker_content(
	enum darling_runtime_mode mode,
	char* content,
	size_t content_size,
	char* error,
	size_t error_size
)
{
	const char* name = darling_runtime_mode_name(mode);
	if (name == NULL)
		return prefix_error(error, error_size,
			"invalid runtime mode for prefix marker: %s", "invalid");
	if (snprintf(content, content_size, "DARLING_RUNTIME_MODE_V1=%s\n",
			name) >= (int)content_size)
		return prefix_error(error, error_size,
			"runtime mode marker content is too long: %s", name);
	return 0;
}

static int require_open_handle(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0)
		return prefix_error(error, error_size,
			"runtime prefix lifecycle handle is not open: %s", "invalid");
	return 0;
}

int darling_runtime_mode_initialize_prefix_marker(
	const darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
)
{
	char content[128];
	if (require_open_handle(handle, error, error_size) != 0 ||
		marker_content(mode, content, sizeof(content), error, error_size) != 0)
		return -1;

	int fd = openat(handle->directory_fd, DARLING_RUNTIME_MODE_MARKER_NAME,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot create runtime mode marker: %s", strerror(errno));

	size_t length = strlen(content);
	ssize_t written = write(fd, content, length);
	int saved_errno = written < 0 ? errno : EIO;
	bool failed = written != (ssize_t)length;
	if (!failed && fsync(fd) != 0) {
		saved_errno = errno;
		failed = true;
	}
	if (close(fd) != 0 && !failed) {
		saved_errno = errno;
		failed = true;
	}
	if (failed) {
		unlinkat(handle->directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot write runtime mode marker: %s", strerror(errno));
	}
	return 0;
}

int darling_runtime_mode_validate_prefix_marker(
	const darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
)
{
	char expected[128];
	char observed[128];
	if (require_open_handle(handle, error, error_size) != 0 ||
		marker_content(mode, expected, sizeof(expected),
			error, error_size) != 0)
		return -1;

	int fd = openat(handle->directory_fd, DARLING_RUNTIME_MODE_MARKER_NAME,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return prefix_error(error, error_size,
			"runtime prefix has no valid mode marker; use a new prefix: %s",
			strerror(errno));

	struct stat status;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
		status.st_size < 0 || status.st_size >= (off_t)sizeof(observed)) {
		close(fd);
		return prefix_error(error, error_size,
			"runtime mode marker is not a bounded regular file: %s",
			DARLING_RUNTIME_MODE_MARKER_NAME);
	}

	ssize_t length = read(fd, observed, (size_t)status.st_size);
	int saved_errno = errno;
	if (close(fd) != 0 && length >= 0) {
		length = -1;
		saved_errno = errno;
	}
	if (length < 0 || length != status.st_size) {
		if (length >= 0)
			saved_errno = EIO;
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot read runtime mode marker: %s", strerror(errno));
	}
	observed[length] = '\0';
	if (strcmp(observed, expected) != 0)
		return prefix_error(error, error_size,
			"runtime prefix mode mismatch; use a new prefix: %s", observed);
	return 0;
}

int darling_runtime_mode_prefix_needs_initialization(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	if (require_open_handle(handle, error, error_size) != 0)
		return -1;
	int etc_fd = open_relative_directory(handle->directory_fd,
		"private/etc", false, NULL, 0);
	if (etc_fd < 0) {
		if (errno == ENOENT)
			return 1;
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix initialization: %s",
			strerror(errno));
	}

	static const char* required[] = {
		"passwd",
		"master.passwd",
		"group",
	};
	size_t present = 0;
	size_t missing = 0;
	for (size_t index = 0; index < sizeof(required) / sizeof(required[0]);
			index++) {
		struct stat status;
		if (fstatat(etc_fd, required[index], &status,
				AT_SYMLINK_NOFOLLOW) == 0) {
			if (!S_ISREG(status.st_mode) || S_ISLNK(status.st_mode)) {
				close(etc_fd);
				return prefix_error(error, error_size,
					"runtime prefix initialization file is invalid: %s",
					required[index]);
			}
			present++;
		} else if (errno == ENOENT) {
			missing++;
		} else {
			int saved_errno = errno;
			close(etc_fd);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot inspect runtime prefix initialization: %s",
				strerror(errno));
		}
	}
	close(etc_fd);
	if (missing == sizeof(required) / sizeof(required[0]))
		return 1;
	if (present == sizeof(required) / sizeof(required[0]))
		return 0;
	return prefix_error(error, error_size,
		"runtime prefix is partially initialized; use a new prefix: %s",
		"invalid");
}

#define DARLING_PREFIX_STATE_MAX 1024
#define DARLING_PREFIX_TRANSACTION_MAX 1024
#define DARLING_PREFIX_LIFECYCLE_NAME_MAX 96

enum lifecycle_transaction_operation {
	LIFECYCLE_TRANSACTION_INVALID,
	LIFECYCLE_TRANSACTION_CREATE,
	LIFECYCLE_TRANSACTION_RECREATE,
	LIFECYCLE_TRANSACTION_DELETE,
};

/*
 * Persistent transaction intent is deliberately independent from the stable
 * prefix snapshot. Recovery switches on both tags and rejects combinations
 * that are not reachable through the lifecycle state machine.
 */
enum lifecycle_transaction_phase {
	LIFECYCLE_PHASE_INVALID,
	LIFECYCLE_PHASE_PREPARED,
	LIFECYCLE_PHASE_REPLACEMENT_STAGED,
	LIFECYCLE_PHASE_SIDECAR_PUBLISHED,
	LIFECYCLE_PHASE_PREFIX_PUBLISHED,
	LIFECYCLE_PHASE_CLEANUP,
};

enum lifecycle_stable_kind {
	LIFECYCLE_STABLE_MISSING,
	LIFECYCLE_STABLE_EMPTY,
	LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED,
	LIFECYCLE_STABLE_CURRENT_V3,
};

struct lifecycle_stable_snapshot {
	enum lifecycle_stable_kind kind;
	union {
		enum darling_runtime_mode legacy_mode;
		struct darling_runtime_prefix_state current;
	} value;
};

enum lifecycle_recovery_status {
	LIFECYCLE_NOT_RECOVERED,
	LIFECYCLE_RECOVERED,
};

enum lifecycle_publish_mode {
	LIFECYCLE_PUBLISH_CREATE,
	LIFECYCLE_PUBLISH_REPLACE,
};

struct lifecycle_names {
	char lock[DARLING_PREFIX_LIFECYCLE_NAME_MAX];
	char transaction[DARLING_PREFIX_LIFECYCLE_NAME_MAX];
	char stage[DARLING_PREFIX_LIFECYCLE_NAME_MAX];
	char sidecar_stage[DARLING_PREFIX_LIFECYCLE_NAME_MAX];
};

struct lifecycle_transaction {
	enum lifecycle_transaction_operation operation;
	enum lifecycle_transaction_phase phase;
	char target[NAME_MAX + 1];
	char stage[DARLING_PREFIX_LIFECYCLE_NAME_MAX];
	enum darling_runtime_mode mode;
	uint64_t old_generation;
	uint64_t new_generation;
	dev_t old_device;
	ino_t old_inode;
	dev_t old_sidecar_device;
	ino_t old_sidecar_inode;
	dev_t new_sidecar_device;
	ino_t new_sidecar_inode;
	uid_t owner_uid;
	gid_t owner_gid;
};

static int lifecycle_checkpoint(
	const char* phase,
	char* error,
	size_t error_size
)
{
#ifdef DARLING_RUNTIME_PREFIX_LIFECYCLE_TESTING
	if (darling_runtime_prefix_test_checkpoint(phase) != 0) {
		if (errno == 0)
			errno = EIO;
		return prefix_error(error, error_size,
			"runtime prefix lifecycle checkpoint failed: %s", phase);
	}
#else
	(void)phase;
	(void)error;
	(void)error_size;
#endif
	return 0;
}

static int fsync_directory(
	int fd,
	const char* detail,
	char* error,
	size_t error_size
)
{
	if (fsync(fd) != 0)
		return prefix_error(error, error_size,
			"cannot persist runtime prefix directory: %s", detail);
	return 0;
}

static uint64_t lifecycle_name_hash(const char* name)
{
	uint64_t value = UINT64_C(1469598103934665603);
	while (*name != '\0') {
		value ^= (unsigned char)*name++;
		value *= UINT64_C(1099511628211);
	}
	return value;
}

static int format_lifecycle_names(
	const darling_runtime_prefix handle,
	struct lifecycle_names* names,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->parent_fd < 0 ||
		handle->leaf[0] == '\0' || names == NULL)
		return prefix_error(error, error_size,
			"runtime prefix lifecycle handle is incomplete: %s", "invalid");
	uint64_t hash = lifecycle_name_hash(handle->leaf);
	if (snprintf(names->lock, sizeof(names->lock),
			".darling-prefix-lock-v2-%016" PRIx64, hash) >=
			(int)sizeof(names->lock) ||
		snprintf(names->transaction, sizeof(names->transaction),
			".darling-prefix-transaction-v3-%016" PRIx64, hash) >=
			(int)sizeof(names->transaction) ||
		snprintf(names->stage, sizeof(names->stage),
			".darling-prefix-stage-v3-%016" PRIx64, hash) >=
			(int)sizeof(names->stage) ||
		snprintf(names->sidecar_stage, sizeof(names->sidecar_stage),
			".darling-prefix-sidecar-stage-v1-%016" PRIx64, hash) >=
			(int)sizeof(names->sidecar_stage))
		return prefix_error(error, error_size,
			"runtime prefix lifecycle metadata name is too long: %s",
			handle->leaf);
	return 0;
}

static int acquire_lifecycle_lock(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	uid_t owner_uid,
	gid_t owner_gid,
	char* error,
	size_t error_size
)
{
	bool created = false;
	int fd = handle->lifecycle_lock_fd;
	if (fd >= 0) {
		handle->lifecycle_lock_fd = -1;
	} else {
		fd = openat(handle->parent_fd, names->lock,
			O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd >= 0)
			created = true;
		else if (errno == EEXIST)
			fd = openat(handle->parent_fd, names->lock,
				O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	}
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot open runtime prefix lifecycle lock: %s",
			strerror(errno));
	struct stat status;
	if (fstat(fd, &status) != 0 ||
		!S_ISREG(status.st_mode) ||
		status.st_nlink != 1 ||
		(status.st_mode & 07777) != 0600 ||
		status.st_uid != owner_uid ||
		status.st_gid != owner_gid) {
		int saved_errno = errno == 0 ? EINVAL : errno;
		close(fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime prefix lifecycle lock metadata is hostile: %s",
			names->lock);
	}
	if (created && fsync(fd) != 0) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot persist runtime prefix lifecycle lock: %s",
			strerror(errno));
	}
	if (created &&
		fsync_directory(handle->parent_fd,
			"persistent lifecycle lock creation",
			error, error_size) != 0) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (lifecycle_checkpoint("lock-opened", error, error_size) != 0) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (flock(fd, LOCK_EX) != 0) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot acquire runtime prefix lifecycle lock: %s",
			strerror(errno));
	}
	struct stat named;
	struct stat locked;
	if (fstat(fd, &locked) != 0 ||
		fstatat(handle->parent_fd, names->lock, &named,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(named.st_mode) ||
		!S_ISREG(named.st_mode) ||
		named.st_nlink != 1 ||
		(named.st_mode & 07777) != 0600 ||
		named.st_uid != owner_uid ||
		named.st_gid != owner_gid ||
		named.st_dev != locked.st_dev ||
		named.st_ino != locked.st_ino) {
		int saved_errno = errno == 0 ? EAGAIN : errno;
		flock(fd, LOCK_UN);
		close(fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime prefix lifecycle lock changed while waiting: %s",
			names->lock);
	}
	if (lifecycle_checkpoint("lock-acquired", error, error_size) != 0) {
		int saved_errno = errno;
		flock(fd, LOCK_UN);
		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static int read_regular_file_at(
	int directory_fd,
	const char* name,
	char* content,
	size_t capacity,
	struct stat* status_out,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	struct stat named;
	if (fstatat(directory_fd, name, &named, AT_SYMLINK_NOFOLLOW) != 0) {
		if (missing_ok && errno == ENOENT)
			return 1;
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix metadata: %s", strerror(errno));
	}
	if (S_ISLNK(named.st_mode) ||
		!S_ISREG(named.st_mode) ||
		named.st_nlink != 1 ||
		(named.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
		named.st_size <= 0 ||
		named.st_size >= (off_t)capacity)
		return prefix_error(error, error_size,
			"runtime prefix metadata is not a bounded private regular file: %s",
			name);
	int fd = openat(directory_fd, name,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot open runtime prefix metadata: %s", strerror(errno));
	struct stat opened;
	if (fstat(fd, &opened) != 0 ||
		opened.st_dev != named.st_dev ||
		opened.st_ino != named.st_ino ||
		!S_ISREG(opened.st_mode)) {
		int saved_errno = errno == 0 ? EAGAIN : errno;
		close(fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime prefix metadata changed during inspection: %s", name);
	}
	size_t offset = 0;
	while (offset < (size_t)opened.st_size) {
		ssize_t length = read(fd, content + offset,
			(size_t)opened.st_size - offset);
		if (length < 0 && errno == EINTR)
			continue;
		if (length <= 0) {
			int saved_errno = length < 0 ? errno : EIO;
			close(fd);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot read runtime prefix metadata: %s",
				strerror(errno));
		}
		offset += (size_t)length;
	}
	if (close(fd) != 0)
		return prefix_error(error, error_size,
			"cannot close runtime prefix metadata: %s", strerror(errno));
	if (memchr(content, '\0', offset) != NULL)
		return prefix_error(error, error_size,
			"runtime prefix metadata contains a NUL byte: %s", name);
	content[offset] = '\0';
	if (status_out != NULL)
		*status_out = opened;
	return 0;
}

static int parse_uintmax_value(const char* value, uintmax_t* parsed)
{
	if (value == NULL || *value == '\0' || parsed == NULL)
		return -1;
	char* end = NULL;
	errno = 0;
	uintmax_t result = strtoumax(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0')
		return -1;
	*parsed = result;
	return 0;
}

static enum darling_runtime_mode lifecycle_mode_from_name(const char* name)
{
	static const enum darling_runtime_mode modes[] = {
		DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY,
		DARLING_RUNTIME_MODE_PRIVILEGED_COPY,
		DARLING_RUNTIME_MODE_PRIVILEGED_EUNION,
		DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
	};
	for (size_t index = 0;
			index < sizeof(modes) / sizeof(modes[0]); index++) {
		const char* candidate = darling_runtime_mode_name(modes[index]);
		if (candidate != NULL && strcmp(candidate, name) == 0)
			return modes[index];
	}
	return DARLING_RUNTIME_MODE_INVALID;
}

static int split_exact_lines(
	char* content,
	char** lines,
	size_t line_count
)
{
	size_t observed = 0;
	char* cursor = content;
	while (*cursor != '\0') {
		if (observed == line_count)
			return -1;
		lines[observed++] = cursor;
		char* newline = strchr(cursor, '\n');
		if (newline == NULL)
			return -1;
		*newline = '\0';
		cursor = newline + 1;
	}
	return observed == line_count ? 0 : -1;
}

static int format_prefix_state(
	const struct darling_runtime_prefix_state* state,
	char* content,
	size_t capacity,
	char* error,
	size_t error_size
)
{
	const char* mode = darling_runtime_mode_name(state->runtime_mode);
	if (mode == NULL ||
		state->schema_version != DARLING_RUNTIME_PREFIX_STATE_SCHEMA_VERSION ||
		state->generation == 0 ||
		state->sidecar_inode == 0 ||
		strcmp(state->provenance,
			DARLING_RUNTIME_PREFIX_PROVENANCE) != 0)
		return prefix_error(error, error_size,
			"runtime prefix state input is invalid: %s", "invalid");
	int length = snprintf(content, capacity,
		"DARLING_PREFIX_STATE_V3\n"
		"schema_version=%u\n"
		"runtime_mode=%s\n"
		"generation=%" PRIu64 "\n"
		"prefix_device=%" PRIuMAX "\n"
		"prefix_inode=%" PRIuMAX "\n"
		"sidecar_device=%" PRIuMAX "\n"
		"sidecar_inode=%" PRIuMAX "\n"
		"owner_uid=%" PRIuMAX "\n"
		"owner_gid=%" PRIuMAX "\n"
		"provenance=%s\n",
		state->schema_version,
		mode,
		state->generation,
		(uintmax_t)state->prefix_device,
		(uintmax_t)state->prefix_inode,
		(uintmax_t)state->sidecar_device,
		(uintmax_t)state->sidecar_inode,
		(uintmax_t)state->owner_uid,
		(uintmax_t)state->owner_gid,
		state->provenance);
	if (length < 0 || (size_t)length >= capacity)
		return prefix_error(error, error_size,
			"runtime prefix state is too large: %s", "invalid");
	return 0;
}

static int parse_prefix_state(
	char* content,
	struct darling_runtime_prefix_state* state,
	char* error,
	size_t error_size
)
{
	char* lines[11];
	if (split_exact_lines(content, lines,
			sizeof(lines) / sizeof(lines[0])) != 0 ||
		strcmp(lines[0], "DARLING_PREFIX_STATE_V3") != 0)
		return prefix_error(error, error_size,
			"runtime prefix state schema is malformed: %s", "invalid");
	static const char* keys[] = {
		"schema_version=",
		"runtime_mode=",
		"generation=",
		"prefix_device=",
		"prefix_inode=",
		"sidecar_device=",
		"sidecar_inode=",
		"owner_uid=",
		"owner_gid=",
		"provenance=",
	};
	for (size_t index = 0;
			index < sizeof(keys) / sizeof(keys[0]); index++) {
		if (strncmp(lines[index + 1], keys[index],
				strlen(keys[index])) != 0 ||
			lines[index + 1][strlen(keys[index])] == '\0')
			return prefix_error(error, error_size,
				"runtime prefix state field is malformed: %s",
				keys[index]);
	}
	uintmax_t schema;
	uintmax_t generation;
	uintmax_t device;
	uintmax_t inode;
	uintmax_t sidecar_device;
	uintmax_t sidecar_inode;
	uintmax_t uid;
	uintmax_t gid;
	if (parse_uintmax_value(lines[1] + strlen(keys[0]), &schema) != 0 ||
		parse_uintmax_value(lines[3] + strlen(keys[2]), &generation) != 0 ||
		parse_uintmax_value(lines[4] + strlen(keys[3]), &device) != 0 ||
		parse_uintmax_value(lines[5] + strlen(keys[4]), &inode) != 0 ||
		parse_uintmax_value(lines[6] + strlen(keys[5]),
			&sidecar_device) != 0 ||
		parse_uintmax_value(lines[7] + strlen(keys[6]),
			&sidecar_inode) != 0 ||
		parse_uintmax_value(lines[8] + strlen(keys[7]), &uid) != 0 ||
		parse_uintmax_value(lines[9] + strlen(keys[8]), &gid) != 0 ||
		schema > UINT_MAX || generation > UINT64_MAX ||
		uid > (uintmax_t)(uid_t)-1 || gid > (uintmax_t)(gid_t)-1)
		return prefix_error(error, error_size,
			"runtime prefix state numeric field is malformed: %s",
			"invalid");
	memset(state, 0, sizeof(*state));
	state->schema_version = (unsigned)schema;
	state->runtime_mode =
		lifecycle_mode_from_name(lines[2] + strlen(keys[1]));
	state->generation = (uint64_t)generation;
	state->prefix_device = (dev_t)device;
	state->prefix_inode = (ino_t)inode;
	state->sidecar_device = (dev_t)sidecar_device;
	state->sidecar_inode = (ino_t)sidecar_inode;
	state->owner_uid = (uid_t)uid;
	state->owner_gid = (gid_t)gid;
	const char* provenance = lines[10] + strlen(keys[9]);
	if (strlen(provenance) >= sizeof(state->provenance))
		return prefix_error(error, error_size,
			"runtime prefix state provenance is too long: %s",
			provenance);
	strcpy(state->provenance, provenance);
	return 0;
}

static int validate_prefix_state(
	const struct darling_runtime_prefix_state* state,
	int prefix_fd,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	char* error,
	size_t error_size
)
{
	if (state->schema_version >
		DARLING_RUNTIME_PREFIX_STATE_SCHEMA_VERSION)
		return prefix_error(error, error_size,
			"runtime prefix state uses a newer schema: %s",
			"recreation required");
	if (state->schema_version !=
			DARLING_RUNTIME_PREFIX_STATE_SCHEMA_VERSION ||
		state->runtime_mode != mode ||
		state->generation == 0 ||
		state->owner_uid != owner_uid ||
		state->owner_gid != owner_gid ||
		strcmp(state->provenance,
			DARLING_RUNTIME_PREFIX_PROVENANCE) != 0)
		return prefix_error(error, error_size,
			"runtime prefix state is incompatible: %s",
			"recreation required");
	struct stat prefix_status;
	if (fstat(prefix_fd, &prefix_status) != 0 ||
		!S_ISDIR(prefix_status.st_mode) ||
		prefix_status.st_uid != owner_uid ||
		prefix_status.st_gid != owner_gid ||
		state->prefix_device != prefix_status.st_dev ||
		state->prefix_inode != prefix_status.st_ino)
		return prefix_error(error, error_size,
			"runtime prefix state belongs to another prefix: %s",
			"recreation required");
	return 0;
}

static int read_prefix_state_fd(
	int prefix_fd,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_state* state,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	char content[DARLING_PREFIX_STATE_MAX];
	struct stat metadata_status;
	int result = read_regular_file_at(prefix_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME,
		content, sizeof(content), &metadata_status, missing_ok,
		error, error_size);
	if (result != 0)
		return result;
	if ((metadata_status.st_mode & 07777) != 0600 ||
		metadata_status.st_uid != owner_uid ||
		metadata_status.st_gid != owner_gid)
		return prefix_error(error, error_size,
			"runtime prefix state has hostile ownership or mode: %s",
			DARLING_RUNTIME_PREFIX_STATE_NAME);
	if (parse_prefix_state(content, state, error, error_size) != 0 ||
		validate_prefix_state(state, prefix_fd, mode,
			owner_uid, owner_gid, error, error_size) != 0)
		return -1;
	return 0;
}

static int validate_bound_sidecar(
	const darling_runtime_prefix handle,
	const struct darling_runtime_prefix_state* state,
	struct stat* named_out,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->parent_fd < 0 ||
		handle->sidecar_leaf[0] == '\0' || state == NULL)
		return prefix_error(error, error_size,
			"runtime prefix sidecar binding is incomplete: %s",
			"invalid");
	struct stat named;
	if (fstatat(handle->parent_fd, handle->sidecar_leaf, &named,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(named.st_mode) || !S_ISDIR(named.st_mode) ||
		(named.st_mode & 07777) != 0700 ||
		named.st_uid != state->owner_uid ||
		named.st_gid != state->owner_gid ||
		named.st_dev != state->sidecar_device ||
		named.st_ino != state->sidecar_inode)
		return prefix_error(error, error_size,
			"runtime prefix sidecar identity is missing or hostile: %s",
			handle->sidecar_leaf);
	if (named_out != NULL)
		*named_out = named;
	if (handle->sidecar_fd >= 0) {
		struct stat opened;
		if (fstat(handle->sidecar_fd, &opened) != 0 ||
			opened.st_dev != state->sidecar_device ||
			opened.st_ino != state->sidecar_inode ||
			!S_ISDIR(opened.st_mode)) {
			int saved_errno = errno == 0 ? EAGAIN : errno;
			errno = saved_errno;
			return prefix_error(error, error_size,
				"retained sidecar no longer matches prefix state: %s",
				handle->sidecar_leaf);
		}
	}
	return 0;
}

static int anchor_bound_sidecar(
	darling_runtime_prefix handle,
	const struct darling_runtime_prefix_state* state,
	char* error,
	size_t error_size
)
{
	struct stat named;
	if (validate_bound_sidecar(handle, state, &named,
			error, error_size) != 0)
		return -1;
	if (handle->sidecar_fd < 0) {
		int fd = openat(handle->parent_fd, handle->sidecar_leaf,
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0)
			return prefix_error(error, error_size,
				"cannot retain runtime prefix sidecar: %s",
				strerror(errno));
		if (compare_opened_directory(fd, &named,
				handle->sidecar_leaf, error, error_size) != 0) {
			close(fd);
			return -1;
		}
		handle->sidecar_fd = fd;
	}
	return 0;
}

int darling_runtime_prefix_read_state(
	const darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_state* state,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0 || state == NULL)
		return prefix_error(error, error_size,
			"runtime prefix state input is missing: %s", "invalid");
	if (darling_runtime_mode_verify_prefix_name(handle,
			error, error_size) != 0)
		return -1;
	if (read_prefix_state_fd(handle->directory_fd, mode,
			owner_uid, owner_gid, state, false,
			error, error_size) != 0)
		return -1;
	return validate_bound_sidecar(handle, state, NULL,
		error, error_size);
}

static int stage_new_prefix_state(
	int prefix_fd,
	const struct darling_runtime_prefix_state* state,
	char* error,
	size_t error_size
)
{
	char content[DARLING_PREFIX_STATE_MAX];
	if (format_prefix_state(state, content, sizeof(content),
			error, error_size) != 0)
		return -1;
	static const char temporary[] = ".darling-prefix-state-v3.tmp";
	struct stat appeared;
	if (fstatat(prefix_fd, temporary, &appeared,
			AT_SYMLINK_NOFOLLOW) == 0) {
		if (S_ISLNK(appeared.st_mode) ||
			!S_ISREG(appeared.st_mode) ||
			unlinkat(prefix_fd, temporary, 0) != 0)
			return prefix_error(error, error_size,
				"runtime prefix state temporary is hostile: %s",
				temporary);
	} else if (errno != ENOENT) {
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix state temporary: %s",
			strerror(errno));
	}
	int fd = openat(prefix_fd, temporary,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot create runtime prefix state temporary: %s",
			strerror(errno));
	if (write_all(fd, content) != 0 || fsync(fd) != 0) {
		int saved_errno = errno;
		close(fd);
		unlinkat(prefix_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot persist runtime prefix state temporary: %s",
			strerror(errno));
	}
	struct stat written;
	if (fstat(fd, &written) != 0 ||
		!S_ISREG(written.st_mode) ||
		written.st_nlink != 1) {
		int saved_errno = errno == 0 ? EINVAL : errno;
		close(fd);
		unlinkat(prefix_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"runtime prefix state temporary changed: %s", temporary);
	}
	if (close(fd) != 0) {
		int saved_errno = errno;
		unlinkat(prefix_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot close runtime prefix state temporary: %s",
			strerror(errno));
	}
	return lifecycle_checkpoint("state-staged", error, error_size);
}

static int publish_staged_prefix_state(
	int prefix_fd,
	enum lifecycle_publish_mode publish_mode,
	char* error,
	size_t error_size
)
{
	static const char temporary[] = ".darling-prefix-state-v3.tmp";
	int rename_result = publish_mode == LIFECYCLE_PUBLISH_REPLACE
		? renameat(prefix_fd, temporary, prefix_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME)
		: renameat2(prefix_fd, temporary, prefix_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME, RENAME_NOREPLACE);
	if (rename_result != 0) {
		int saved_errno = errno;
		unlinkat(prefix_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot publish runtime prefix state atomically: %s",
			strerror(errno));
	}
	if (fsync_directory(prefix_fd, "state publication",
			error, error_size) != 0)
		return -1;
	return lifecycle_checkpoint("state-published", error, error_size);
}

static int write_new_prefix_state(
	int prefix_fd,
	const struct darling_runtime_prefix_state* state,
	enum lifecycle_publish_mode publish_mode,
	char* error,
	size_t error_size
)
{
	if (stage_new_prefix_state(prefix_fd, state,
			error, error_size) != 0)
		return -1;
	return publish_staged_prefix_state(prefix_fd, publish_mode,
		error, error_size);
}

static const char* transaction_operation_name(
	enum lifecycle_transaction_operation operation
)
{
	switch (operation) {
		case LIFECYCLE_TRANSACTION_INVALID:
			return NULL;
		case LIFECYCLE_TRANSACTION_CREATE:
			return "create";
		case LIFECYCLE_TRANSACTION_RECREATE:
			return "recreate";
		case LIFECYCLE_TRANSACTION_DELETE:
			return "delete";
	}
	return NULL;
}

static enum lifecycle_transaction_operation transaction_operation_from_name(
	const char* name
)
{
	if (strcmp(name, "create") == 0)
		return LIFECYCLE_TRANSACTION_CREATE;
	if (strcmp(name, "recreate") == 0)
		return LIFECYCLE_TRANSACTION_RECREATE;
	if (strcmp(name, "delete") == 0)
		return LIFECYCLE_TRANSACTION_DELETE;
	return LIFECYCLE_TRANSACTION_INVALID;
}

static const char* transaction_phase_name(
	enum lifecycle_transaction_phase phase
)
{
	switch (phase) {
		case LIFECYCLE_PHASE_INVALID:
			return NULL;
		case LIFECYCLE_PHASE_PREPARED:
			return "prepared";
		case LIFECYCLE_PHASE_REPLACEMENT_STAGED:
			return "replacement-staged";
		case LIFECYCLE_PHASE_SIDECAR_PUBLISHED:
			return "sidecar-published";
		case LIFECYCLE_PHASE_PREFIX_PUBLISHED:
			return "prefix-published";
		case LIFECYCLE_PHASE_CLEANUP:
			return "cleanup";
	}
	return NULL;
}

static enum lifecycle_transaction_phase transaction_phase_from_name(
	const char* name
)
{
	if (strcmp(name, "prepared") == 0)
		return LIFECYCLE_PHASE_PREPARED;
	if (strcmp(name, "replacement-staged") == 0)
		return LIFECYCLE_PHASE_REPLACEMENT_STAGED;
	if (strcmp(name, "sidecar-published") == 0)
		return LIFECYCLE_PHASE_SIDECAR_PUBLISHED;
	if (strcmp(name, "prefix-published") == 0)
		return LIFECYCLE_PHASE_PREFIX_PUBLISHED;
	if (strcmp(name, "cleanup") == 0)
		return LIFECYCLE_PHASE_CLEANUP;
	return LIFECYCLE_PHASE_INVALID;
}

static int format_transaction(
	const struct lifecycle_transaction* transaction,
	char* content,
	size_t capacity,
	char* error,
	size_t error_size
)
{
	const char* operation =
		transaction_operation_name(transaction->operation);
	const char* phase = transaction_phase_name(transaction->phase);
	const char* mode = darling_runtime_mode_name(transaction->mode);
	if (operation == NULL || phase == NULL || mode == NULL ||
		transaction->target[0] == '\0' ||
		transaction->stage[0] == '\0')
		return prefix_error(error, error_size,
			"runtime prefix transaction input is invalid: %s", "invalid");
	int length = snprintf(content, capacity,
		"DARLING_PREFIX_TRANSACTION_V3\n"
		"operation=%s\n"
		"phase=%s\n"
		"target=%s\n"
		"stage=%s\n"
		"runtime_mode=%s\n"
		"old_generation=%" PRIu64 "\n"
		"new_generation=%" PRIu64 "\n"
		"old_device=%" PRIuMAX "\n"
		"old_inode=%" PRIuMAX "\n"
		"old_sidecar_device=%" PRIuMAX "\n"
		"old_sidecar_inode=%" PRIuMAX "\n"
		"new_sidecar_device=%" PRIuMAX "\n"
		"new_sidecar_inode=%" PRIuMAX "\n"
		"owner_uid=%" PRIuMAX "\n"
		"owner_gid=%" PRIuMAX "\n"
		"provenance=%s\n",
		operation,
		phase,
		transaction->target,
		transaction->stage,
		mode,
		transaction->old_generation,
		transaction->new_generation,
		(uintmax_t)transaction->old_device,
		(uintmax_t)transaction->old_inode,
		(uintmax_t)transaction->old_sidecar_device,
		(uintmax_t)transaction->old_sidecar_inode,
		(uintmax_t)transaction->new_sidecar_device,
		(uintmax_t)transaction->new_sidecar_inode,
		(uintmax_t)transaction->owner_uid,
		(uintmax_t)transaction->owner_gid,
		DARLING_RUNTIME_PREFIX_PROVENANCE);
	if (length < 0 || (size_t)length >= capacity)
		return prefix_error(error, error_size,
			"runtime prefix transaction is too large: %s", "invalid");
	return 0;
}

static int parse_transaction(
	char* content,
	struct lifecycle_transaction* transaction,
	char* error,
	size_t error_size
)
{
	char* lines[17];
	if (split_exact_lines(content, lines,
			sizeof(lines) / sizeof(lines[0])) != 0 ||
		strcmp(lines[0], "DARLING_PREFIX_TRANSACTION_V3") != 0)
		return prefix_error(error, error_size,
			"runtime prefix transaction schema is malformed: %s",
			"invalid");
	static const char* keys[] = {
		"operation=",
		"phase=",
		"target=",
		"stage=",
		"runtime_mode=",
		"old_generation=",
		"new_generation=",
		"old_device=",
		"old_inode=",
		"old_sidecar_device=",
		"old_sidecar_inode=",
		"new_sidecar_device=",
		"new_sidecar_inode=",
		"owner_uid=",
		"owner_gid=",
		"provenance=",
	};
	for (size_t index = 0;
			index < sizeof(keys) / sizeof(keys[0]); index++) {
		if (strncmp(lines[index + 1], keys[index],
				strlen(keys[index])) != 0 ||
			lines[index + 1][strlen(keys[index])] == '\0')
			return prefix_error(error, error_size,
				"runtime prefix transaction field is malformed: %s",
				keys[index]);
	}
	memset(transaction, 0, sizeof(*transaction));
	transaction->operation = transaction_operation_from_name(
		lines[1] + strlen(keys[0]));
	transaction->phase = transaction_phase_from_name(
		lines[2] + strlen(keys[1]));
	const char* target = lines[3] + strlen(keys[2]);
	const char* stage = lines[4] + strlen(keys[3]);
	if (transaction->operation == LIFECYCLE_TRANSACTION_INVALID ||
		transaction->phase == LIFECYCLE_PHASE_INVALID ||
		strlen(target) > NAME_MAX ||
		strlen(stage) >= sizeof(transaction->stage) ||
		strchr(target, '/') != NULL ||
		strchr(stage, '/') != NULL ||
		strcmp(target, ".") == 0 ||
		strcmp(target, "..") == 0)
		return prefix_error(error, error_size,
			"runtime prefix transaction identity is invalid: %s",
			"invalid");
	strcpy(transaction->target, target);
	strcpy(transaction->stage, stage);
	transaction->mode =
		lifecycle_mode_from_name(lines[5] + strlen(keys[4]));
	uintmax_t old_generation;
	uintmax_t new_generation;
	uintmax_t old_device;
	uintmax_t old_inode;
	uintmax_t old_sidecar_device;
	uintmax_t old_sidecar_inode;
	uintmax_t new_sidecar_device;
	uintmax_t new_sidecar_inode;
	uintmax_t uid;
	uintmax_t gid;
	if (parse_uintmax_value(lines[6] + strlen(keys[5]),
			&old_generation) != 0 ||
		parse_uintmax_value(lines[7] + strlen(keys[6]),
			&new_generation) != 0 ||
		parse_uintmax_value(lines[8] + strlen(keys[7]),
			&old_device) != 0 ||
		parse_uintmax_value(lines[9] + strlen(keys[8]),
			&old_inode) != 0 ||
		parse_uintmax_value(lines[10] + strlen(keys[9]),
			&old_sidecar_device) != 0 ||
		parse_uintmax_value(lines[11] + strlen(keys[10]),
			&old_sidecar_inode) != 0 ||
		parse_uintmax_value(lines[12] + strlen(keys[11]),
			&new_sidecar_device) != 0 ||
		parse_uintmax_value(lines[13] + strlen(keys[12]),
			&new_sidecar_inode) != 0 ||
		parse_uintmax_value(lines[14] + strlen(keys[13]), &uid) != 0 ||
		parse_uintmax_value(lines[15] + strlen(keys[14]), &gid) != 0 ||
		old_generation > UINT64_MAX ||
		new_generation > UINT64_MAX ||
		uid > (uintmax_t)(uid_t)-1 ||
		gid > (uintmax_t)(gid_t)-1 ||
		strcmp(lines[16] + strlen(keys[15]),
			DARLING_RUNTIME_PREFIX_PROVENANCE) != 0)
		return prefix_error(error, error_size,
			"runtime prefix transaction value is invalid: %s",
			"invalid");
	transaction->old_generation = (uint64_t)old_generation;
	transaction->new_generation = (uint64_t)new_generation;
	transaction->old_device = (dev_t)old_device;
	transaction->old_inode = (ino_t)old_inode;
	transaction->old_sidecar_device = (dev_t)old_sidecar_device;
	transaction->old_sidecar_inode = (ino_t)old_sidecar_inode;
	transaction->new_sidecar_device = (dev_t)new_sidecar_device;
	transaction->new_sidecar_inode = (ino_t)new_sidecar_inode;
	transaction->owner_uid = (uid_t)uid;
	transaction->owner_gid = (gid_t)gid;
	if (transaction->mode == DARLING_RUNTIME_MODE_INVALID)
		return prefix_error(error, error_size,
			"runtime prefix transaction mode is invalid: %s", "invalid");
	return 0;
}

static int write_transaction(
	const darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	const struct lifecycle_transaction* transaction,
	enum lifecycle_publish_mode publish_mode,
	char* error,
	size_t error_size
)
{
	char content[DARLING_PREFIX_TRANSACTION_MAX];
	if (format_transaction(transaction, content, sizeof(content),
			error, error_size) != 0)
		return -1;
	char temporary[DARLING_PREFIX_LIFECYCLE_NAME_MAX];
	if (snprintf(temporary, sizeof(temporary), "%s.tmp",
			names->transaction) >= (int)sizeof(temporary))
		return prefix_error(error, error_size,
			"runtime prefix transaction temporary is too long: %s",
			names->transaction);
	struct stat appeared;
	if (fstatat(handle->parent_fd, names->transaction, &appeared,
			AT_SYMLINK_NOFOLLOW) == 0) {
		if (publish_mode != LIFECYCLE_PUBLISH_REPLACE ||
			S_ISLNK(appeared.st_mode) ||
			!S_ISREG(appeared.st_mode) ||
			appeared.st_nlink != 1 ||
			(appeared.st_mode & (S_IWGRP | S_IWOTH)) != 0)
			return prefix_error(error, error_size,
				"runtime prefix transaction already exists or is hostile: %s",
				names->transaction);
	} else if (errno == ENOENT) {
		if (publish_mode != LIFECYCLE_PUBLISH_CREATE)
			return prefix_error(error, error_size,
				"runtime prefix transaction vanished before phase update: %s",
				names->transaction);
	} else {
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix transaction: %s",
			strerror(errno));
	}
	if (fstatat(handle->parent_fd, temporary, &appeared,
			AT_SYMLINK_NOFOLLOW) == 0) {
		if (S_ISLNK(appeared.st_mode) ||
			!S_ISREG(appeared.st_mode) ||
			unlinkat(handle->parent_fd, temporary, 0) != 0)
			return prefix_error(error, error_size,
				"runtime prefix transaction temporary is hostile: %s",
				temporary);
	} else if (errno != ENOENT) {
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix transaction temporary: %s",
			strerror(errno));
	}
	int fd = openat(handle->parent_fd, temporary,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot create runtime prefix transaction temporary: %s",
			strerror(errno));
	if (write_all(fd, content) != 0 || fsync(fd) != 0) {
		int saved_errno = errno;
		close(fd);
		unlinkat(handle->parent_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot persist runtime prefix transaction temporary: %s",
			strerror(errno));
	}
	if (close(fd) != 0) {
		int saved_errno = errno;
		unlinkat(handle->parent_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot close runtime prefix transaction temporary: %s",
			strerror(errno));
	}
	int rename_result = publish_mode == LIFECYCLE_PUBLISH_CREATE
		? renameat2(handle->parent_fd, temporary,
			handle->parent_fd, names->transaction,
			RENAME_NOREPLACE)
		: renameat(handle->parent_fd, temporary,
			handle->parent_fd, names->transaction);
	if (rename_result != 0) {
		int saved_errno = errno;
		unlinkat(handle->parent_fd, temporary, 0);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot publish runtime prefix transaction: %s",
			strerror(errno));
	}
	return fsync_directory(handle->parent_fd,
		"transaction publication", error, error_size);
}

static int advance_transaction_phase(
	const darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	struct lifecycle_transaction* transaction,
	enum lifecycle_transaction_phase expected,
	enum lifecycle_transaction_phase next,
	const char* checkpoint,
	char* error,
	size_t error_size
)
{
	if (transaction->phase != expected ||
		next == LIFECYCLE_PHASE_INVALID)
		return prefix_error(error, error_size,
			"runtime prefix transaction phase transition is invalid: %s",
			checkpoint);
	struct lifecycle_transaction updated = *transaction;
	updated.phase = next;
	if (write_transaction(handle, names, &updated,
			LIFECYCLE_PUBLISH_REPLACE, error, error_size) != 0)
		return -1;
	*transaction = updated;
	return lifecycle_checkpoint(checkpoint, error, error_size);
}

static int read_transaction(
	const darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	struct lifecycle_transaction* transaction,
	char* error,
	size_t error_size
)
{
	char content[DARLING_PREFIX_TRANSACTION_MAX];
	struct stat status;
	int result = read_regular_file_at(handle->parent_fd,
		names->transaction, content, sizeof(content), &status, true,
		error, error_size);
	if (result != 0)
		return result;
	if (parse_transaction(content, transaction,
			error, error_size) != 0)
		return -1;
	if ((status.st_mode & 07777) != 0600 ||
		status.st_uid != transaction->owner_uid ||
		status.st_gid != transaction->owner_gid)
		return prefix_error(error, error_size,
			"runtime prefix transaction has an unexpected owner: %s",
			names->transaction);
	return 0;
}

static int remove_tree_contents(
	int directory_fd,
	char* error,
	size_t error_size
)
{
	int duplicate = dup(directory_fd);
	if (duplicate < 0)
		return prefix_error(error, error_size,
			"cannot duplicate lifecycle cleanup directory: %s",
			strerror(errno));
	DIR* directory = fdopendir(duplicate);
	if (directory == NULL) {
		int saved_errno = errno;
		close(duplicate);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot enumerate lifecycle cleanup directory: %s",
			strerror(errno));
	}
	struct dirent* entry;
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
			strcmp(entry->d_name, "..") == 0)
			continue;
		struct stat status;
		if (fstatat(directory_fd, entry->d_name, &status,
				AT_SYMLINK_NOFOLLOW) != 0) {
			int saved_errno = errno;
			closedir(directory);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot inspect lifecycle cleanup entry: %s",
				strerror(errno));
		}
		if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode)) {
			int child = openat(directory_fd, entry->d_name,
				O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (child < 0 ||
				compare_opened_directory(child, &status, entry->d_name,
					error, error_size) != 0) {
				int saved_errno = errno;
				if (child >= 0)
					close(child);
				closedir(directory);
				errno = saved_errno;
				return -1;
			}
			if (remove_tree_contents(child, error, error_size) != 0) {
				close(child);
				closedir(directory);
				return -1;
			}
			close(child);
			if (unlinkat(directory_fd, entry->d_name,
					AT_REMOVEDIR) != 0) {
				int saved_errno = errno;
				closedir(directory);
				errno = saved_errno;
				return prefix_error(error, error_size,
					"cannot remove lifecycle cleanup directory: %s",
					strerror(errno));
			}
		} else {
			if (unlinkat(directory_fd, entry->d_name, 0) != 0) {
				int saved_errno = errno;
				closedir(directory);
				errno = saved_errno;
				return prefix_error(error, error_size,
					"cannot remove lifecycle cleanup entry: %s",
					strerror(errno));
			}
		}
	}
	int read_errno = errno;
	if (closedir(directory) != 0 && read_errno == 0)
		read_errno = errno;
	if (read_errno != 0) {
		errno = read_errno;
		return prefix_error(error, error_size,
			"cannot finish lifecycle cleanup enumeration: %s",
			strerror(errno));
	}
	return fsync_directory(directory_fd, "recursive cleanup",
		error, error_size);
}

static int open_named_directory(
	int parent_fd,
	const char* name,
	bool missing_ok,
	struct stat* status_out,
	char* error,
	size_t error_size
)
{
	struct stat before;
	if (fstatat(parent_fd, name, &before,
			AT_SYMLINK_NOFOLLOW) != 0) {
		if (missing_ok && errno == ENOENT)
			return -2;
		return prefix_error(error, error_size,
			"cannot inspect lifecycle directory: %s", strerror(errno));
	}
	if (S_ISLNK(before.st_mode) || !S_ISDIR(before.st_mode))
		return prefix_error(error, error_size,
			"lifecycle directory is a symlink or non-directory: %s",
			name);
	int fd = openat(parent_fd, name,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return prefix_error(error, error_size,
			"cannot open lifecycle directory: %s", strerror(errno));
	if (compare_opened_directory(fd, &before, name,
			error, error_size) != 0) {
		close(fd);
		return -1;
	}
	if (status_out != NULL)
		*status_out = before;
	return fd;
}

static int remove_named_tree(
	int parent_fd,
	const char* name,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	int fd = open_named_directory(parent_fd, name, missing_ok,
		NULL, error, error_size);
	if (fd == -2)
		return 0;
	if (fd < 0)
		return -1;
	if (remove_tree_contents(fd, error, error_size) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) {
		if (missing_ok && errno == ENOENT)
			return 0;
		return prefix_error(error, error_size,
			"cannot remove lifecycle directory root: %s",
			strerror(errno));
	}
	return fsync_directory(parent_fd, "lifecycle root cleanup",
		error, error_size);
}

static int validate_legacy_marker(
	int prefix_fd,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	char* error,
	size_t error_size
)
{
	char observed[128];
	char expected[128];
	struct stat status;
	if (marker_content(mode, expected, sizeof(expected),
			error, error_size) != 0)
		return -1;
	int result = read_regular_file_at(prefix_fd,
		DARLING_RUNTIME_MODE_MARKER_NAME,
		observed, sizeof(observed), &status, true,
		error, error_size);
	if (result != 0)
		return result;
	if (status.st_uid != 0 && status.st_uid != owner_uid)
		return prefix_error(error, error_size,
			"legacy runtime marker has an unexpected owner: %s",
			DARLING_RUNTIME_MODE_MARKER_NAME);
	if (strcmp(observed, expected) != 0)
		return prefix_error(error, error_size,
			"legacy runtime prefix mode is incompatible: %s",
			"recreation required");
	return 0;
}

static int unlink_private_regular_at(
	int directory_fd,
	const char* name,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	struct stat status;
	if (fstatat(directory_fd, name, &status,
			AT_SYMLINK_NOFOLLOW) != 0) {
		if (missing_ok && errno == ENOENT)
			return 0;
		return prefix_error(error, error_size,
			"cannot inspect lifecycle metadata before cleanup: %s",
			strerror(errno));
	}
	if (S_ISLNK(status.st_mode) || !S_ISREG(status.st_mode) ||
		status.st_nlink != 1)
		return prefix_error(error, error_size,
			"lifecycle cleanup metadata is hostile: %s", name);
	if (unlinkat(directory_fd, name, 0) != 0)
		return prefix_error(error, error_size,
			"cannot remove lifecycle metadata: %s", strerror(errno));
	return 0;
}

static int prefix_inode_matches(
	const darling_runtime_prefix handle,
	dev_t device,
	ino_t inode
)
{
	if (handle->directory_fd < 0)
		return 0;
	struct stat status;
	return fstat(handle->directory_fd, &status) == 0 &&
		status.st_dev == device && status.st_ino == inode;
}

static int cleanup_transaction_metadata(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	bool cleanup_legacy,
	char* error,
	size_t error_size
)
{
	if (handle->directory_fd >= 0) {
		if (unlink_private_regular_at(handle->directory_fd,
				".darling-prefix-state-v3.tmp", true,
				error, error_size) != 0)
			return -1;
		if (cleanup_legacy &&
			unlink_private_regular_at(handle->directory_fd,
				DARLING_RUNTIME_MODE_MARKER_NAME, true,
				error, error_size) != 0)
			return -1;
		if (fsync_directory(handle->directory_fd,
				"transaction target cleanup", error, error_size) != 0)
			return -1;
	}
	if (unlink_private_regular_at(handle->parent_fd,
			names->transaction, false, error, error_size) != 0)
		return -1;
	return fsync_directory(handle->parent_fd,
		"transaction cleanup", error, error_size);
}

static int validate_transaction_identity(
	const darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	const struct lifecycle_transaction* transaction,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	char* error,
	size_t error_size
)
{
	if (strcmp(transaction->target, handle->leaf) != 0 ||
		strcmp(transaction->stage, names->stage) != 0 ||
		transaction->mode != mode ||
		transaction->owner_uid != owner_uid ||
		transaction->owner_gid != owner_gid ||
		transaction->new_generation == 0 ||
		transaction->new_generation <= transaction->old_generation ||
		(transaction->operation != LIFECYCLE_TRANSACTION_DELETE &&
			transaction->phase >= LIFECYCLE_PHASE_REPLACEMENT_STAGED &&
			transaction->new_sidecar_inode == 0))
		return prefix_error(error, error_size,
			"runtime prefix transaction belongs to another lifecycle: %s",
			names->transaction);
	return 0;
}

static int remove_prefix_workdir(
	const darling_runtime_prefix handle,
	bool missing_ok,
	char* error,
	size_t error_size
);

static int inspect_stable_snapshot(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct lifecycle_stable_snapshot* snapshot,
	char* error,
	size_t error_size
)
{
	memset(snapshot, 0, sizeof(*snapshot));
	switch (handle->anchor_state) {
		case DARLING_RUNTIME_PREFIX_ANCHOR_MISSING:
			snapshot->kind = LIFECYCLE_STABLE_MISSING;
			return 0;
		case DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY:
			if (darling_runtime_mode_verify_prefix_name(handle,
					error, error_size) != 0)
				return -1;
			snapshot->kind = LIFECYCLE_STABLE_EMPTY;
			return 0;
		case DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED:
			break;
		case DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED:
		case DARLING_RUNTIME_PREFIX_ANCHOR_MOVED:
			return prefix_error(error, error_size,
				"runtime prefix capability cannot be inspected: %s",
				"invalid typestate");
	}
	if (darling_runtime_mode_verify_prefix_name(handle,
			error, error_size) != 0)
		return -1;
	int state_result = read_prefix_state_fd(handle->directory_fd,
		mode, owner_uid, owner_gid, &snapshot->value.current,
		true, error, error_size);
	if (state_result < 0)
		return -1;
	if (state_result == 0) {
		snapshot->kind = LIFECYCLE_STABLE_CURRENT_V3;
		return 0;
	}
	struct stat legacy_state;
	if (fstatat(handle->directory_fd,
			DARLING_RUNTIME_PREFIX_LEGACY_STATE_NAME,
			&legacy_state, AT_SYMLINK_NOFOLLOW) == 0) {
		snapshot->kind = LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED;
		return 0;
	}
	if (errno != ENOENT)
		return prefix_error(error, error_size,
			"cannot inspect legacy runtime prefix state: %s",
			strerror(errno));
	int legacy_result = validate_legacy_marker(handle->directory_fd,
		mode, owner_uid, error, error_size);
	if (legacy_result < 0)
		return -1;
	if (legacy_result == 0) {
		snapshot->kind = LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED;
		snapshot->value.legacy_mode = mode;
		return 0;
	}
	/*
	 * A populated prefix without the v3 state marker is an unversioned
	 * legacy format. It is never interpreted or upgraded in place.
	 */
	snapshot->kind = LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED;
	return 0;
}

enum lifecycle_stable_relation {
	LIFECYCLE_RELATION_MISSING,
	LIFECYCLE_RELATION_OLD_EMPTY,
	LIFECYCLE_RELATION_OLD_LEGACY,
	LIFECYCLE_RELATION_OLD_CURRENT,
	LIFECYCLE_RELATION_NEW_CURRENT,
	LIFECYCLE_RELATION_UNEXPECTED,
};

static enum lifecycle_stable_relation stable_relation(
	const darling_runtime_prefix handle,
	const struct lifecycle_stable_snapshot* snapshot,
	const struct lifecycle_transaction* transaction
)
{
	switch (snapshot->kind) {
		case LIFECYCLE_STABLE_MISSING:
			return LIFECYCLE_RELATION_MISSING;
		case LIFECYCLE_STABLE_EMPTY:
			return prefix_inode_matches(handle,
				transaction->old_device, transaction->old_inode)
				? LIFECYCLE_RELATION_OLD_EMPTY
				: LIFECYCLE_RELATION_UNEXPECTED;
		case LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED:
			return prefix_inode_matches(handle,
				transaction->old_device, transaction->old_inode)
				? LIFECYCLE_RELATION_OLD_LEGACY
				: LIFECYCLE_RELATION_UNEXPECTED;
		case LIFECYCLE_STABLE_CURRENT_V3:
			if (snapshot->value.current.generation ==
				transaction->new_generation)
				return LIFECYCLE_RELATION_NEW_CURRENT;
			if (snapshot->value.current.generation ==
					transaction->old_generation &&
				prefix_inode_matches(handle,
					transaction->old_device,
					transaction->old_inode))
				return LIFECYCLE_RELATION_OLD_CURRENT;
			return LIFECYCLE_RELATION_UNEXPECTED;
	}
	return LIFECYCLE_RELATION_UNEXPECTED;
}

enum lifecycle_recovery_disposition {
	LIFECYCLE_RECOVERY_INVALID,
	LIFECYCLE_RECOVERY_ROLLBACK,
	LIFECYCLE_RECOVERY_FINISH,
};

static enum lifecycle_recovery_disposition recovery_disposition(
	enum lifecycle_transaction_operation operation,
	enum lifecycle_transaction_phase phase,
	enum lifecycle_stable_relation relation
)
{
	switch (operation) {
		case LIFECYCLE_TRANSACTION_CREATE:
			switch (phase) {
				case LIFECYCLE_PHASE_PREPARED:
				case LIFECYCLE_PHASE_REPLACEMENT_STAGED:
					if (relation == LIFECYCLE_RELATION_MISSING)
						return LIFECYCLE_RECOVERY_ROLLBACK;
					if (relation == LIFECYCLE_RELATION_NEW_CURRENT)
						return LIFECYCLE_RECOVERY_FINISH;
					return LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_SIDECAR_PUBLISHED:
					if (relation == LIFECYCLE_RELATION_MISSING)
						return LIFECYCLE_RECOVERY_ROLLBACK;
					if (relation == LIFECYCLE_RELATION_NEW_CURRENT)
						return LIFECYCLE_RECOVERY_FINISH;
					return LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_PREFIX_PUBLISHED:
				case LIFECYCLE_PHASE_CLEANUP:
					return relation == LIFECYCLE_RELATION_NEW_CURRENT
						? LIFECYCLE_RECOVERY_FINISH
						: LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_INVALID:
					return LIFECYCLE_RECOVERY_INVALID;
			}
			break;
		case LIFECYCLE_TRANSACTION_RECREATE:
			switch (phase) {
				case LIFECYCLE_PHASE_PREPARED:
				case LIFECYCLE_PHASE_REPLACEMENT_STAGED:
					if (relation == LIFECYCLE_RELATION_OLD_EMPTY ||
						relation == LIFECYCLE_RELATION_OLD_LEGACY ||
						relation == LIFECYCLE_RELATION_OLD_CURRENT)
						return LIFECYCLE_RECOVERY_ROLLBACK;
					if (relation == LIFECYCLE_RELATION_NEW_CURRENT)
						return LIFECYCLE_RECOVERY_FINISH;
					return LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_SIDECAR_PUBLISHED:
					if (relation == LIFECYCLE_RELATION_OLD_EMPTY ||
						relation == LIFECYCLE_RELATION_OLD_LEGACY ||
						relation == LIFECYCLE_RELATION_OLD_CURRENT)
						return LIFECYCLE_RECOVERY_ROLLBACK;
					if (relation == LIFECYCLE_RELATION_NEW_CURRENT)
						return LIFECYCLE_RECOVERY_FINISH;
					return LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_PREFIX_PUBLISHED:
				case LIFECYCLE_PHASE_CLEANUP:
					return relation == LIFECYCLE_RELATION_NEW_CURRENT
						? LIFECYCLE_RECOVERY_FINISH
						: LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_INVALID:
					return LIFECYCLE_RECOVERY_INVALID;
			}
			break;
		case LIFECYCLE_TRANSACTION_DELETE:
			switch (phase) {
				case LIFECYCLE_PHASE_PREPARED:
					if (relation == LIFECYCLE_RELATION_OLD_CURRENT)
						return LIFECYCLE_RECOVERY_ROLLBACK;
					if (relation == LIFECYCLE_RELATION_MISSING)
						return LIFECYCLE_RECOVERY_FINISH;
					return LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_SIDECAR_PUBLISHED:
					if (relation == LIFECYCLE_RELATION_OLD_CURRENT)
						return LIFECYCLE_RECOVERY_ROLLBACK;
					if (relation == LIFECYCLE_RELATION_MISSING)
						return LIFECYCLE_RECOVERY_FINISH;
					return LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_PREFIX_PUBLISHED:
				case LIFECYCLE_PHASE_CLEANUP:
					return relation == LIFECYCLE_RELATION_MISSING
						? LIFECYCLE_RECOVERY_FINISH
						: LIFECYCLE_RECOVERY_INVALID;
				case LIFECYCLE_PHASE_INVALID:
				case LIFECYCLE_PHASE_REPLACEMENT_STAGED:
					return LIFECYCLE_RECOVERY_INVALID;
			}
			break;
		case LIFECYCLE_TRANSACTION_INVALID:
			return LIFECYCLE_RECOVERY_INVALID;
	}
	return LIFECYCLE_RECOVERY_INVALID;
}

#ifdef DARLING_RUNTIME_PREFIX_LIFECYCLE_TESTING
int darling_runtime_prefix_test_recovery_matrix(
	enum darling_runtime_prefix_test_operation operation,
	enum darling_runtime_prefix_test_phase phase,
	enum darling_runtime_prefix_test_stable_relation stable
)
{
	if (operation < DARLING_RUNTIME_PREFIX_TEST_CREATE ||
		operation > DARLING_RUNTIME_PREFIX_TEST_DELETE ||
		phase < DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
		phase > DARLING_RUNTIME_PREFIX_TEST_CLEANUP ||
		stable < DARLING_RUNTIME_PREFIX_TEST_MISSING ||
		stable > DARLING_RUNTIME_PREFIX_TEST_UNEXPECTED)
		return DARLING_RUNTIME_PREFIX_TEST_INVALID;
	enum lifecycle_recovery_disposition result =
		recovery_disposition(
			(enum lifecycle_transaction_operation)operation,
			(enum lifecycle_transaction_phase)phase,
			(enum lifecycle_stable_relation)stable);
	switch (result) {
		case LIFECYCLE_RECOVERY_INVALID:
			return DARLING_RUNTIME_PREFIX_TEST_INVALID;
		case LIFECYCLE_RECOVERY_ROLLBACK:
			return DARLING_RUNTIME_PREFIX_TEST_ROLLBACK;
		case LIFECYCLE_RECOVERY_FINISH:
			return DARLING_RUNTIME_PREFIX_TEST_FINISH;
	}
	return DARLING_RUNTIME_PREFIX_TEST_INVALID;
}
#endif

static int named_directory_identity(
	int parent_fd,
	const char* name,
	dev_t expected_device,
	ino_t expected_inode,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	struct stat status;
	if (fstatat(parent_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
		if (missing_ok && errno == ENOENT)
			return 1;
		return prefix_error(error, error_size,
			"cannot inspect lifecycle sidecar identity: %s",
			strerror(errno));
	}
	if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode) ||
		status.st_dev != expected_device ||
		status.st_ino != expected_inode)
		return prefix_error(error, error_size,
			"lifecycle sidecar identity is inconsistent: %s", name);
	return 0;
}

static int recover_sidecar_rollback(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	const struct lifecycle_transaction* transaction,
	char* error,
	size_t error_size
)
{
	struct stat final_status;
	int final_result = fstatat(handle->parent_fd, handle->sidecar_leaf,
		&final_status, AT_SYMLINK_NOFOLLOW);
	if (final_result != 0 && errno != ENOENT)
		return prefix_error(error, error_size,
			"cannot inspect sidecar rollback boundary: %s",
			strerror(errno));
	const bool final_missing = final_result != 0;
	const bool new_published = !final_missing &&
		S_ISDIR(final_status.st_mode) &&
		!S_ISLNK(final_status.st_mode) &&
		final_status.st_dev == transaction->new_sidecar_device &&
		final_status.st_ino == transaction->new_sidecar_inode;
	if (!new_published) {
		if ((!final_missing &&
			 (transaction->old_sidecar_inode == 0 ||
			  final_status.st_dev != transaction->old_sidecar_device ||
			  final_status.st_ino != transaction->old_sidecar_inode)) ||
			(final_missing && transaction->old_sidecar_inode != 0))
			return prefix_error(error, error_size,
				"runtime sidecar changed before rollback: %s",
				handle->sidecar_leaf);
		return remove_named_tree(handle->parent_fd,
			names->sidecar_stage, true, error, error_size);
	}
	if (named_directory_identity(handle->parent_fd,
			handle->sidecar_leaf,
			transaction->new_sidecar_device,
			transaction->new_sidecar_inode,
			false, error, error_size) != 0)
		return -1;
	if (transaction->old_sidecar_inode != 0) {
		if (named_directory_identity(handle->parent_fd,
				names->sidecar_stage,
				transaction->old_sidecar_device,
				transaction->old_sidecar_inode,
				false, error, error_size) != 0 ||
			renameat2(handle->parent_fd, names->sidecar_stage,
				handle->parent_fd, handle->sidecar_leaf,
				RENAME_EXCHANGE) != 0)
			return prefix_error(error, error_size,
				"cannot roll back runtime prefix sidecar publication: %s",
				strerror(errno));
		return remove_named_tree(handle->parent_fd,
			names->sidecar_stage, false, error, error_size);
	}
	return remove_named_tree(handle->parent_fd,
		handle->sidecar_leaf, false, error, error_size);
}

static int recover_sidecar_finish(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	const struct lifecycle_transaction* transaction,
	char* error,
	size_t error_size
)
{
	if (named_directory_identity(handle->parent_fd,
			handle->sidecar_leaf,
			transaction->new_sidecar_device,
			transaction->new_sidecar_inode,
			false, error, error_size) != 0)
		return -1;
	return remove_named_tree(handle->parent_fd,
		names->sidecar_stage, true, error, error_size);
}

static int recover_delete_sidecar(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	const struct lifecycle_transaction* transaction,
	enum lifecycle_recovery_disposition disposition,
	char* error,
	size_t error_size
)
{
	int stage_result = named_directory_identity(handle->parent_fd,
		names->sidecar_stage,
		transaction->old_sidecar_device,
		transaction->old_sidecar_inode,
		true, error, error_size);
	if (stage_result < 0)
		return -1;
	if (stage_result == 1) {
		if (transaction->phase >= LIFECYCLE_PHASE_SIDECAR_PUBLISHED)
			return prefix_error(error, error_size,
				"runtime sidecar deletion stage is missing: %s",
				names->sidecar_stage);
		return 0;
	}
	if (disposition == LIFECYCLE_RECOVERY_ROLLBACK) {
		struct stat appeared;
		if (fstatat(handle->parent_fd, handle->sidecar_leaf,
				&appeared, AT_SYMLINK_NOFOLLOW) == 0 ||
			errno != ENOENT ||
			renameat2(handle->parent_fd, names->sidecar_stage,
				handle->parent_fd, handle->sidecar_leaf,
				RENAME_NOREPLACE) != 0)
			return prefix_error(error, error_size,
				"cannot roll back runtime sidecar deletion: %s",
				strerror(errno));
		return fsync_directory(handle->parent_fd,
			"sidecar deletion rollback", error, error_size);
	}
	return remove_named_tree(handle->parent_fd,
		names->sidecar_stage, false, error, error_size);
}

static int recover_interrupted_transaction(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	enum lifecycle_recovery_status* recovered,
	char* error,
	size_t error_size
)
{
	*recovered = LIFECYCLE_NOT_RECOVERED;
	struct lifecycle_transaction transaction;
	int transaction_result = read_transaction(handle, names,
		&transaction, error, error_size);
	if (transaction_result == 1) {
		struct stat unexpected;
		if (fstatat(handle->parent_fd, names->stage, &unexpected,
				AT_SYMLINK_NOFOLLOW) == 0 ||
			fstatat(handle->parent_fd, names->sidecar_stage, &unexpected,
				AT_SYMLINK_NOFOLLOW) == 0)
			return prefix_error(error, error_size,
				"orphan runtime prefix or sidecar stage has no transaction: %s",
				names->stage);
		if (errno != ENOENT)
			return prefix_error(error, error_size,
				"cannot inspect runtime prefix stage: %s",
				strerror(errno));
		return 0;
	}
	if (transaction_result != 0 ||
		validate_transaction_identity(handle, names, &transaction,
			mode, owner_uid, owner_gid, error, error_size) != 0)
		return -1;

	struct lifecycle_stable_snapshot stable;
	if (inspect_stable_snapshot(handle, mode, owner_uid, owner_gid,
			&stable, error, error_size) != 0)
		return -1;
	enum lifecycle_stable_relation relation =
		stable_relation(handle, &stable, &transaction);
	enum lifecycle_recovery_disposition disposition =
		recovery_disposition(transaction.operation,
			transaction.phase, relation);
	if (disposition == LIFECYCLE_RECOVERY_INVALID)
		return prefix_error(error, error_size,
			"runtime prefix stable state and journal phase are incompatible: %s",
			names->transaction);

	if (disposition == LIFECYCLE_RECOVERY_ROLLBACK) {
		if (((transaction.operation == LIFECYCLE_TRANSACTION_CREATE ||
			  transaction.operation == LIFECYCLE_TRANSACTION_RECREATE) &&
			 recover_sidecar_rollback(handle, names, &transaction,
				error, error_size) != 0) ||
			(transaction.operation == LIFECYCLE_TRANSACTION_DELETE &&
			 recover_delete_sidecar(handle, names, &transaction,
				disposition, error, error_size) != 0) ||
			remove_named_tree(handle->parent_fd, names->stage,
				true, error, error_size) != 0 ||
			(handle->directory_fd >= 0 &&
			 unlink_private_regular_at(handle->directory_fd,
				".darling-prefix-state-v3.tmp", true,
				error, error_size) != 0) ||
			unlink_private_regular_at(handle->parent_fd,
				names->transaction, false,
				error, error_size) != 0 ||
			fsync_directory(handle->parent_fd,
				"transaction rollback", error, error_size) != 0)
			return -1;
	} else {
		enum lifecycle_transaction_operation operation =
			transaction.operation;
		if (handle->workdir_fd >= 0) {
			close(handle->workdir_fd);
			handle->workdir_fd = -1;
		}
		if (remove_named_tree(handle->parent_fd, names->stage,
				true, error, error_size) != 0)
			return -1;
		if ((operation == LIFECYCLE_TRANSACTION_CREATE ||
			 operation == LIFECYCLE_TRANSACTION_RECREATE) &&
			recover_sidecar_finish(handle, names, &transaction,
				error, error_size) != 0)
			return -1;
		if (operation == LIFECYCLE_TRANSACTION_DELETE &&
			recover_delete_sidecar(handle, names, &transaction,
				disposition, error, error_size) != 0)
			return -1;
		if ((operation == LIFECYCLE_TRANSACTION_RECREATE ||
			 operation == LIFECYCLE_TRANSACTION_DELETE) &&
			remove_prefix_workdir(handle, true,
				error, error_size) != 0)
			return -1;
		if (handle->directory_fd >= 0) {
			if (unlink_private_regular_at(handle->directory_fd,
					".darling-prefix-state-v3.tmp", true,
					error, error_size) != 0 ||
				fsync_directory(handle->directory_fd,
					"transaction finish", error, error_size) != 0)
				return -1;
		}
		if (unlink_private_regular_at(handle->parent_fd,
				names->transaction, false,
				error, error_size) != 0 ||
			fsync_directory(handle->parent_fd,
				"transaction finish", error, error_size) != 0)
			return -1;
	}
	*recovered = LIFECYCLE_RECOVERED;
	return 0;
}

static int make_prefix_state(
	int prefix_fd,
	int sidecar_fd,
	enum darling_runtime_mode mode,
	uint64_t generation,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_state* state,
	char* error,
	size_t error_size
)
{
	struct stat status;
	struct stat sidecar_status;
	if (prefix_fd < 0 || fstat(prefix_fd, &status) != 0 ||
		!S_ISDIR(status.st_mode) || sidecar_fd < 0 ||
		fstat(sidecar_fd, &sidecar_status) != 0 ||
		!S_ISDIR(sidecar_status.st_mode) || generation == 0)
		return prefix_error(error, error_size,
			"cannot identify runtime prefix state directory: %s",
			strerror(errno));
	memset(state, 0, sizeof(*state));
	state->schema_version = DARLING_RUNTIME_PREFIX_STATE_SCHEMA_VERSION;
	state->runtime_mode = mode;
	state->generation = generation;
	state->prefix_device = status.st_dev;
	state->prefix_inode = status.st_ino;
	state->sidecar_device = sidecar_status.st_dev;
	state->sidecar_inode = sidecar_status.st_ino;
	state->owner_uid = owner_uid;
	state->owner_gid = owner_gid;
	strcpy(state->provenance, DARLING_RUNTIME_PREFIX_PROVENANCE);
	return 0;
}

static int validate_complete_prefix_contents(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	int initialized = darling_runtime_mode_prefix_needs_initialization(
		handle, error, error_size);
	if (initialized < 0)
		return -1;
	if (initialized != 0)
		return prefix_error(error, error_size,
			"runtime prefix state exists without complete contents: %s",
			"recreation required");
	return 0;
}

static int initialize_staged_prefix(
	const darling_runtime_prefix target,
	const struct lifecycle_names* names,
	enum darling_runtime_mode mode,
	const char* user_name,
	uid_t owner_uid,
	gid_t owner_gid,
	uint64_t generation,
	darling_runtime_prefix staged_out,
	struct darling_runtime_prefix_state* state_out,
	char* error,
	size_t error_size
)
{
	struct stat unexpected;
	if (fstatat(target->parent_fd, names->stage, &unexpected,
			AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT ||
		fstatat(target->parent_fd, names->sidecar_stage, &unexpected,
			AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
		return prefix_error(error, error_size,
			"runtime prefix or sidecar stage already exists or is hostile: %s",
			names->stage);
	int sidecar_fd = create_and_open_directory(target->parent_fd,
		names->sidecar_stage, 0700, error, error_size);
	if (sidecar_fd < 0)
		return -1;
	if (fchown(sidecar_fd, owner_uid, owner_gid) != 0 ||
		fchmod(sidecar_fd, 0700) != 0 ||
		fsync_directory(sidecar_fd, "staged empty sidecar",
			error, error_size) != 0) {
		int saved_errno = errno;
		close(sidecar_fd);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot finalize staged runtime prefix sidecar: %s",
			strerror(errno));
	}
	int stage_fd = create_and_open_directory(target->parent_fd,
		names->stage, 0700, error, error_size);
	if (stage_fd < 0) {
		close(sidecar_fd);
		return -1;
	}
	darling_runtime_prefix staged =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	staged->parent_fd = dup(target->parent_fd);
	staged->directory_fd = stage_fd;
	staged->sidecar_fd = sidecar_fd;
	staged->anchor_state = DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY;
	strcpy(staged->leaf, names->stage);
	strcpy(staged->sidecar_leaf, names->sidecar_stage);
	if (staged->parent_fd < 0 ||
		darling_runtime_mode_setup_prefix(staged, user_name,
			owner_uid, owner_gid, error, error_size) != 0 ||
		(fchmod(stage_fd, 0755) != 0 &&
		 prefix_error(error, error_size,
			"cannot finalize staged prefix mode: %s",
			strerror(errno)) != 0) ||
		make_prefix_state(stage_fd, sidecar_fd, mode, generation,
			owner_uid, owner_gid, state_out,
			error, error_size) != 0 ||
		write_new_prefix_state(stage_fd, state_out,
			LIFECYCLE_PUBLISH_CREATE,
			error, error_size) != 0 ||
		validate_complete_prefix_contents(staged,
			error, error_size) != 0 ||
		read_prefix_state_fd(stage_fd, mode, owner_uid, owner_gid,
			state_out, false, error, error_size) != 0 ||
		fsync_directory(stage_fd, "staged prefix",
			error, error_size) != 0) {
		int saved_errno = errno;
		darling_runtime_mode_close_prefix(staged);
		errno = saved_errno;
		return -1;
	}
	int move_result = darling_runtime_prefix_move(staged_out, staged,
		error, error_size);
	if (move_result != 0)
		darling_runtime_mode_close_prefix(staged);
	return move_result;
}

static int replace_handle_directory_fd(
	darling_runtime_prefix handle,
	int new_fd,
	char* error,
	size_t error_size
)
{
	if (handle->directory_fd >= 0)
		close(handle->directory_fd);
	handle->directory_fd = new_fd;
	handle->anchor_state = DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED;
	return darling_runtime_mode_verify_prefix_name(
		handle, error, error_size);
}

static int publish_staged_prefix(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	darling_runtime_prefix staged,
	enum lifecycle_transaction_operation operation,
	char* error,
	size_t error_size
)
{
	if (operation != LIFECYCLE_TRANSACTION_CREATE &&
		operation != LIFECYCLE_TRANSACTION_RECREATE)
		return prefix_error(error, error_size,
			"runtime prefix publication operation is invalid: %s",
			"invalid");
	if (staged == NULL ||
		staged->anchor_state != DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED ||
		staged->directory_fd < 0 ||
		strcmp(staged->leaf, names->stage) != 0)
		return prefix_error(error, error_size,
			"staged runtime prefix capability is invalid: %s",
			names->stage);
	int result = operation == LIFECYCLE_TRANSACTION_RECREATE
		? renameat2(handle->parent_fd, names->stage,
			handle->parent_fd, handle->leaf, RENAME_EXCHANGE)
		: renameat2(handle->parent_fd, names->stage,
			handle->parent_fd, handle->leaf, RENAME_NOREPLACE);
	if (result != 0)
		return prefix_error(error, error_size,
			"cannot publish staged runtime prefix atomically: %s",
			strerror(errno));
	int stage_fd = staged->directory_fd;
	staged->directory_fd = -1;
	int replace_result = replace_handle_directory_fd(handle, stage_fd,
		error, error_size);
	darling_runtime_mode_close_prefix(staged);
	if (replace_result != 0 ||
		fsync_directory(handle->parent_fd,
			"prefix publication", error, error_size) != 0)
		return -1;
	return lifecycle_checkpoint("prefix-published",
		error, error_size);
}

static int publish_staged_sidecar(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	darling_runtime_prefix staged,
	enum lifecycle_transaction_operation operation,
	char* error,
	size_t error_size
)
{
	if ((operation != LIFECYCLE_TRANSACTION_CREATE &&
			operation != LIFECYCLE_TRANSACTION_RECREATE) ||
		staged == NULL || staged->sidecar_fd < 0 ||
		strcmp(staged->sidecar_leaf, names->sidecar_stage) != 0 ||
		handle->sidecar_leaf[0] == '\0')
		return prefix_error(error, error_size,
			"staged runtime prefix sidecar capability is invalid: %s",
			names->sidecar_stage);
	const bool replacing = handle->sidecar_fd >= 0;
	int rename_result = replacing
		? renameat2(handle->parent_fd, names->sidecar_stage,
			handle->parent_fd, handle->sidecar_leaf, RENAME_EXCHANGE)
		: renameat2(handle->parent_fd, names->sidecar_stage,
			handle->parent_fd, handle->sidecar_leaf, RENAME_NOREPLACE);
	if (rename_result != 0)
		return prefix_error(error, error_size,
			"cannot publish staged runtime prefix sidecar atomically: %s",
			strerror(errno));
	int old_sidecar_fd = handle->sidecar_fd;
	handle->sidecar_fd = staged->sidecar_fd;
	staged->sidecar_fd = old_sidecar_fd;
	if (staged->sidecar_fd < 0)
		staged->sidecar_leaf[0] = '\0';
	if (fsync_directory(handle->parent_fd,
			"prefix sidecar publication", error, error_size) != 0)
		return -1;
	return lifecycle_checkpoint("sidecar-published",
		error, error_size);
}

static int format_workdir_leaf(
	const darling_runtime_prefix handle,
	char* leaf,
	size_t capacity,
	char* error,
	size_t error_size
)
{
	int length = snprintf(leaf, capacity, "%s.workdir", handle->leaf);
	if (length < 0 || (size_t)length >= capacity)
		return prefix_error(error, error_size,
			"runtime prefix workdir name is too long: %s",
			handle->leaf);
	return 0;
}

static int remove_prefix_workdir(
	const darling_runtime_prefix handle,
	bool missing_ok,
	char* error,
	size_t error_size
)
{
	char leaf[NAME_MAX + 1];
	if (format_workdir_leaf(handle, leaf, sizeof(leaf),
			error, error_size) != 0)
		return -1;
	return remove_named_tree(handle->parent_fd, leaf,
		missing_ok, error, error_size);
}

static int ensure_prefix_not_running(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	int fd = darling_runtime_mode_open_relative_file(handle,
		".init.pid", O_RDONLY, 0, NULL, 0);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0;
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix process state: %s",
			strerror(errno));
	}
	char content[64];
	ssize_t length = read(fd, content, sizeof(content) - 1);
	int saved_errno = errno;
	close(fd);
	if (length <= 0 || length >= (ssize_t)sizeof(content) - 1) {
		errno = length < 0 ? saved_errno : EINVAL;
		return prefix_error(error, error_size,
			"runtime prefix PID metadata is malformed: %s",
			".init.pid");
	}
	content[length] = '\0';
	char* end = NULL;
	errno = 0;
	long pid = strtol(content, &end, 10);
	if (errno != 0 || end == content ||
		(*end != '\n' && *end != '\0') ||
		(*end == '\n' && end[1] != '\0') ||
		pid <= 0 || pid > INT_MAX)
		return prefix_error(error, error_size,
			"runtime prefix PID metadata is malformed: %s",
			".init.pid");
	if (kill((pid_t)pid, 0) == 0 || errno == EPERM)
		return prefix_error(error, error_size,
			"runtime prefix is still in use by a live process: %s",
			".init.pid");
	if (errno != ESRCH)
		return prefix_error(error, error_size,
			"cannot validate runtime prefix process state: %s",
			strerror(errno));
	return 0;
}

static int decode_mountinfo_path(char* path)
{
	char* input = path;
	char* output = path;
	while (*input != '\0') {
		if (*input == '\\' &&
			input[1] >= '0' && input[1] <= '7' &&
			input[2] >= '0' && input[2] <= '7' &&
			input[3] >= '0' && input[3] <= '7') {
			unsigned int value =
				(unsigned int)(input[1] - '0') * 64 +
				(unsigned int)(input[2] - '0') * 8 +
				(unsigned int)(input[3] - '0');
			if (value == 0)
				return -1;
			*output++ = (char)value;
			input += 4;
			continue;
		}
		*output++ = *input++;
	}
	*output = '\0';
	return 0;
}

static int ensure_prefix_not_mounted(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
)
{
	char descriptor_path[64];
	char prefix_path[DARLING_RUNTIME_PREFIX_PATH_MAX];
	int descriptor_length = snprintf(descriptor_path,
		sizeof(descriptor_path), "/proc/self/fd/%d",
		handle->directory_fd);
	if (descriptor_length < 0 ||
		(size_t)descriptor_length >= sizeof(descriptor_path))
		return prefix_error(error, error_size,
			"cannot construct retained prefix descriptor path: %s",
			"invalid");
	ssize_t prefix_length = readlink(descriptor_path,
		prefix_path, sizeof(prefix_path) - 1);
	if (prefix_length <= 0 ||
		prefix_length >= (ssize_t)sizeof(prefix_path) - 1)
		return prefix_error(error, error_size,
			"cannot identify retained prefix for mount check: %s",
			prefix_length < 0 ? strerror(errno) : "path too long");
	prefix_path[prefix_length] = '\0';
	if (strstr(prefix_path, " (deleted)") != NULL)
		return prefix_error(error, error_size,
			"retained prefix was detached before mount check: %s",
			"invalid");

	const char* mountinfo_path = "/proc/self/mountinfo";
#ifdef DARLING_RUNTIME_PREFIX_LIFECYCLE_TESTING
	if (darling_runtime_prefix_test_mountinfo_path != NULL)
		mountinfo_path = darling_runtime_prefix_test_mountinfo_path;
#endif
	FILE* mountinfo = fopen(mountinfo_path, "re");
	if (mountinfo == NULL)
		return prefix_error(error, error_size,
			"cannot inspect runtime prefix mounts: %s",
			strerror(errno));
	char* line = NULL;
	size_t capacity = 0;
	int result = 0;
	while (getline(&line, &capacity, mountinfo) >= 0) {
		char* save = NULL;
		char* field = strtok_r(line, " ", &save);
		for (int index = 1; field != NULL && index < 5; ++index)
			field = strtok_r(NULL, " ", &save);
		if (field == NULL || decode_mountinfo_path(field) != 0) {
			result = prefix_error(error, error_size,
				"runtime mountinfo is malformed: %s", "invalid");
			break;
		}
		size_t mount_length = strlen(field);
		if ((mount_length == (size_t)prefix_length &&
			 memcmp(field, prefix_path, mount_length) == 0) ||
			(mount_length > (size_t)prefix_length &&
			 memcmp(field, prefix_path, (size_t)prefix_length) == 0 &&
			 field[prefix_length] == '/')) {
			result = prefix_error(error, error_size,
				"runtime prefix has a live mount and cannot be"
				" recreated: %s", field);
			break;
		}
	}
	int saved_errno = errno;
	free(line);
	if (fclose(mountinfo) != 0 && result == 0)
		result = prefix_error(error, error_size,
			"cannot finish runtime prefix mount inspection: %s",
			strerror(errno));
	errno = saved_errno;
	return result;
}

static void recover_preserving_failure(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	int saved_errno,
	const char* saved_error,
	char* error,
	size_t error_size
)
{
	char cleanup_error[512] = {0};
	enum lifecycle_recovery_status recovered =
		LIFECYCLE_NOT_RECOVERED;
	(void)recover_interrupted_transaction(handle, names, mode,
		owner_uid, owner_gid, &recovered,
		cleanup_error, sizeof(cleanup_error));
	errno = saved_errno;
	if (error != NULL && error_size != 0) {
		snprintf(error, error_size, "%s",
			saved_error == NULL ? "runtime prefix lifecycle failed" :
			saved_error);
	}
}

static int begin_transaction(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	enum lifecycle_transaction_operation operation,
	enum darling_runtime_mode mode,
	uint64_t old_generation,
	uint64_t new_generation,
	uid_t owner_uid,
	gid_t owner_gid,
	struct lifecycle_transaction* transaction,
	char* error,
	size_t error_size
)
{
	memset(transaction, 0, sizeof(*transaction));
	transaction->operation = operation;
	transaction->phase = LIFECYCLE_PHASE_PREPARED;
	strcpy(transaction->target, handle->leaf);
	strcpy(transaction->stage, names->stage);
	transaction->mode = mode;
	transaction->old_generation = old_generation;
	transaction->new_generation = new_generation;
	transaction->owner_uid = owner_uid;
	transaction->owner_gid = owner_gid;
	if (handle->directory_fd >= 0) {
		struct stat old_status;
		if (fstat(handle->directory_fd, &old_status) != 0)
			return prefix_error(error, error_size,
				"cannot identify current runtime prefix: %s",
				strerror(errno));
		transaction->old_device = old_status.st_dev;
		transaction->old_inode = old_status.st_ino;
	}
	if (handle->sidecar_fd >= 0) {
		struct stat old_sidecar;
		if (fstat(handle->sidecar_fd, &old_sidecar) != 0 ||
			!S_ISDIR(old_sidecar.st_mode))
			return prefix_error(error, error_size,
				"cannot identify current runtime prefix sidecar: %s",
				strerror(errno));
		transaction->old_sidecar_device = old_sidecar.st_dev;
		transaction->old_sidecar_inode = old_sidecar.st_ino;
	}
	if (write_transaction(handle, names, transaction,
			LIFECYCLE_PUBLISH_CREATE,
			error, error_size) != 0)
		return -1;
	return lifecycle_checkpoint("transaction-prepared",
		error, error_size);
}

static int create_or_recreate_prefix(
	darling_runtime_prefix handle,
	const struct lifecycle_names* names,
	enum darling_runtime_mode mode,
	const char* user_name,
	uid_t owner_uid,
	gid_t owner_gid,
	uint64_t old_generation,
	enum darling_runtime_prefix_lifecycle_action action,
	enum lifecycle_recovery_status recovered,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
)
{
	enum lifecycle_transaction_operation operation =
		handle->directory_fd >= 0
			? LIFECYCLE_TRANSACTION_RECREATE
			: LIFECYCLE_TRANSACTION_CREATE;
	struct lifecycle_transaction transaction;
	if (begin_transaction(handle, names,
			operation,
			mode, old_generation, old_generation + 1,
			owner_uid, owner_gid, &transaction,
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot begin prefix transaction" : error);
		recover_preserving_failure(handle, names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		return -1;
	}

	darling_runtime_prefix staged =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	struct darling_runtime_prefix_state new_state;
	if (initialize_staged_prefix(handle, names, mode, user_name,
			owner_uid, owner_gid, transaction.new_generation,
			staged, &new_state, error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot stage runtime prefix" : error);
		darling_runtime_mode_close_prefix(staged);
		recover_preserving_failure(handle, names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		return -1;
	}
	struct stat staged_sidecar_status;
	if (fstat(staged->sidecar_fd, &staged_sidecar_status) != 0) {
		int saved_errno = errno;
		darling_runtime_mode_close_prefix(staged);
		errno = saved_errno;
		return prefix_error(error, error_size,
			"cannot identify staged runtime sidecar: %s",
			strerror(errno));
	}
	transaction.new_sidecar_device = staged_sidecar_status.st_dev;
	transaction.new_sidecar_inode = staged_sidecar_status.st_ino;
	if (
		advance_transaction_phase(handle, names, &transaction,
			LIFECYCLE_PHASE_PREPARED,
			LIFECYCLE_PHASE_REPLACEMENT_STAGED,
			"replacement-staged",
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot stage runtime prefix" : error);
		darling_runtime_mode_close_prefix(staged);
		recover_preserving_failure(handle, names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		return -1;
	}
	if (publish_staged_sidecar(handle, names, staged, operation,
			error, error_size) != 0 ||
		advance_transaction_phase(handle, names, &transaction,
			LIFECYCLE_PHASE_REPLACEMENT_STAGED,
			LIFECYCLE_PHASE_SIDECAR_PUBLISHED,
			"journal-sidecar-published",
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot publish runtime sidecar" : error);
		darling_runtime_mode_close_prefix(staged);
		recover_preserving_failure(handle, names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		return -1;
	}
	if (publish_staged_prefix(handle, names, staged, operation,
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot publish runtime prefix" : error);
		/* Close only the still-owned portion of the staged capability. */
		darling_runtime_mode_close_prefix(staged);
		recover_preserving_failure(handle, names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		return -1;
	}
	if (advance_transaction_phase(handle, names, &transaction,
			LIFECYCLE_PHASE_SIDECAR_PUBLISHED,
			LIFECYCLE_PHASE_PREFIX_PUBLISHED,
			"journal-prefix-published",
			error, error_size) != 0 ||
		advance_transaction_phase(handle, names, &transaction,
			LIFECYCLE_PHASE_PREFIX_PUBLISHED,
			LIFECYCLE_PHASE_CLEANUP,
			"cleanup-start",
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot start prefix cleanup" : error);
		recover_preserving_failure(handle, names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		return -1;
	}
	if (operation == LIFECYCLE_TRANSACTION_RECREATE) {
		if (handle->workdir_fd >= 0) {
			close(handle->workdir_fd);
			handle->workdir_fd = -1;
		}
		if (remove_named_tree(handle->parent_fd, names->stage,
				false, error, error_size) != 0 ||
			remove_named_tree(handle->parent_fd,
				names->sidecar_stage, true,
				error, error_size) != 0 ||
			remove_prefix_workdir(handle, true,
				error, error_size) != 0) {
			int saved_errno = errno;
			char saved_error[512];
			snprintf(saved_error, sizeof(saved_error), "%s",
				error == NULL ? "cannot clean replaced prefix" : error);
			recover_preserving_failure(handle, names, mode,
				owner_uid, owner_gid, saved_errno, saved_error,
				error, error_size);
			return -1;
		}
	}
	if (operation == LIFECYCLE_TRANSACTION_CREATE &&
		remove_named_tree(handle->parent_fd,
			names->sidecar_stage, true,
			error, error_size) != 0)
		return -1;
	if (cleanup_transaction_metadata(handle, names, false,
			error, error_size) != 0 ||
		lifecycle_checkpoint("cleanup-complete",
			error, error_size) != 0)
		return -1;
	if (result != NULL) {
		memset(result, 0, sizeof(*result));
		result->action = action;
		result->recovery =
			recovered == LIFECYCLE_RECOVERED
				? DARLING_RUNTIME_PREFIX_RECOVERED_TRANSACTION
				: DARLING_RUNTIME_PREFIX_NO_RECOVERY;
		result->state = new_state;
	}
	return 0;
}

static int preflight_existing_prefix_compatibility(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct lifecycle_stable_snapshot* stable,
	char* error,
	size_t error_size
)
{
	if (inspect_stable_snapshot(handle, mode, owner_uid, owner_gid,
			stable, error, error_size) != 0)
		return -1;
	switch (stable->kind) {
		case LIFECYCLE_STABLE_CURRENT_V3:
			return validate_complete_prefix_contents(handle,
				error, error_size);
		case LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED:
			return 1;
		case LIFECYCLE_STABLE_MISSING:
		case LIFECYCLE_STABLE_EMPTY:
			return 0;
	}
	return prefix_error(error, error_size,
		"runtime prefix stable state is invalid: %s", "invalid");
}

int darling_runtime_prefix_prepare(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	const char* user_name,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->parent_fd < 0 ||
		handle->leaf[0] == '\0' ||
		user_name == NULL || *user_name == '\0' ||
		darling_runtime_mode_name(mode) == NULL)
		return prefix_error(error, error_size,
			"runtime prefix prepare input is invalid: %s", "invalid");
	struct lifecycle_stable_snapshot preflight;
	if (handle->anchor_state == DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED) {
		int preflight_result =
			preflight_existing_prefix_compatibility(handle, mode,
				owner_uid, owner_gid, &preflight,
				error, error_size);
		if (preflight_result < 0)
			return -1;
		if (preflight_result > 0) {
			if (result != NULL) {
				memset(result, 0, sizeof(*result));
				result->verdict =
					DARLING_RUNTIME_PREFIX_VERDICT_RECREATE_REQUIRED;
			}
			prefix_error(error, error_size,
				"legacy or unversioned prefix is not bootable;"
				" explicit recreation is required: %s",
				"PREFIX_RECREATE_REQUIRED");
			return DARLING_RUNTIME_PREFIX_RECREATE_REQUIRED;
		}
	}
	struct lifecycle_names names;
	if (format_lifecycle_names(handle, &names,
			error, error_size) != 0)
		return -1;
	int lock_fd = acquire_lifecycle_lock(handle, &names,
		owner_uid, owner_gid,
		error, error_size);
	if (lock_fd < 0)
		return -1;
	enum lifecycle_recovery_status recovered =
		LIFECYCLE_NOT_RECOVERED;
	if (recover_interrupted_transaction(handle, &names, mode,
			owner_uid, owner_gid, &recovered,
			error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	struct lifecycle_stable_snapshot stable;
	if (inspect_stable_snapshot(handle, mode, owner_uid, owner_gid,
			&stable, error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	int result_code;
	switch (stable.kind) {
		case LIFECYCLE_STABLE_MISSING:
		case LIFECYCLE_STABLE_EMPTY:
			result_code = create_or_recreate_prefix(handle, &names,
				mode, user_name, owner_uid, owner_gid, 0,
				DARLING_RUNTIME_PREFIX_CREATED, recovered, result,
				error, error_size);
			break;
		case LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED:
			if (result != NULL) {
				memset(result, 0, sizeof(*result));
				result->verdict =
					DARLING_RUNTIME_PREFIX_VERDICT_RECREATE_REQUIRED;
			}
			prefix_error(error, error_size,
				"legacy or unversioned prefix is not bootable;"
				" explicit recreation is required: %s",
				"PREFIX_RECREATE_REQUIRED");
			result_code = DARLING_RUNTIME_PREFIX_RECREATE_REQUIRED;
			break;
		case LIFECYCLE_STABLE_CURRENT_V3: {
			if (anchor_bound_sidecar(handle, &stable.value.current,
					error, error_size) != 0) {
				result_code = -1;
				break;
			}
			struct stat legacy;
			if (fstatat(handle->directory_fd,
					DARLING_RUNTIME_MODE_MARKER_NAME, &legacy,
					AT_SYMLINK_NOFOLLOW) == 0) {
				result_code = prefix_error(error, error_size,
					"current runtime prefix has unexpected legacy metadata: %s",
					DARLING_RUNTIME_MODE_MARKER_NAME);
				break;
			}
			if (errno != ENOENT ||
				validate_complete_prefix_contents(handle,
					error, error_size) != 0) {
				result_code = -1;
				break;
			}
			if (result != NULL) {
				memset(result, 0, sizeof(*result));
				result->action =
					recovered == LIFECYCLE_RECOVERED
						? DARLING_RUNTIME_PREFIX_REPAIRED
						: DARLING_RUNTIME_PREFIX_REUSED;
				result->recovery =
					recovered == LIFECYCLE_RECOVERED
						? DARLING_RUNTIME_PREFIX_RECOVERED_TRANSACTION
						: DARLING_RUNTIME_PREFIX_NO_RECOVERY;
				result->state = stable.value.current;
			}
			result_code = 0;
			break;
		}
	}
	if (result_code == 0) {
		if (flock(lock_fd, LOCK_SH) != 0) {
			int saved_errno = errno;
			close(lock_fd);
			errno = saved_errno;
			return prefix_error(error, error_size,
				"cannot retain runtime prefix lifecycle lease: %s",
				strerror(errno));
		}
		handle->lifecycle_lock_fd = lock_fd;
		return 0;
	}
	close(lock_fd);
	return result_code;
}

int darling_runtime_prefix_recreate(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	const char* user_name,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0 ||
		user_name == NULL || *user_name == '\0')
		return prefix_error(error, error_size,
			"runtime prefix recreate input is invalid: %s", "invalid");
	struct lifecycle_stable_snapshot preflight;
	int preflight_result = preflight_existing_prefix_compatibility(
		handle, mode, owner_uid, owner_gid, &preflight,
		error, error_size);
	if (preflight_result < 0)
		return -1;
	struct lifecycle_names names;
	if (format_lifecycle_names(handle, &names,
			error, error_size) != 0)
		return -1;
	int lock_fd = acquire_lifecycle_lock(handle, &names,
		owner_uid, owner_gid,
		error, error_size);
	if (lock_fd < 0)
		return -1;
	enum lifecycle_recovery_status recovered =
		LIFECYCLE_NOT_RECOVERED;
	if (recover_interrupted_transaction(handle, &names, mode,
			owner_uid, owner_gid, &recovered,
			error, error_size) != 0 ||
		darling_runtime_mode_verify_prefix_name(handle,
			error, error_size) != 0 ||
		ensure_prefix_not_running(handle, error, error_size) != 0 ||
		ensure_prefix_not_mounted(handle, error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	struct lifecycle_stable_snapshot stable;
	if (inspect_stable_snapshot(handle, mode, owner_uid, owner_gid,
			&stable, error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	uint64_t old_generation = 0;
	if (stable.kind == LIFECYCLE_STABLE_CURRENT_V3)
	{
		if (anchor_bound_sidecar(handle, &stable.value.current,
				error, error_size) != 0) {
			close(lock_fd);
			return -1;
		}
		old_generation = stable.value.current.generation;
	}
	else if (stable.kind != LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED) {
		close(lock_fd);
		return prefix_error(error, error_size,
			"runtime prefix cannot be explicitly recreated from state: %s",
			"invalid");
	}
	if (stable.kind == LIFECYCLE_STABLE_LEGACY_OR_UNVERSIONED) {
		struct stat unexpected_sidecar;
		if (fstatat(handle->parent_fd, handle->sidecar_leaf,
				&unexpected_sidecar, AT_SYMLINK_NOFOLLOW) == 0 ||
			errno != ENOENT) {
			close(lock_fd);
			return prefix_error(error, error_size,
				"legacy prefix has unexpected sidecar state: %s",
				handle->sidecar_leaf);
		}
	}
	int result_code = create_or_recreate_prefix(handle, &names,
		mode, user_name, owner_uid, owner_gid, old_generation,
		DARLING_RUNTIME_PREFIX_RECREATED, recovered, result,
		error, error_size);
	close(lock_fd);
	return result_code;
}

int darling_runtime_prefix_delete(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->parent_fd < 0 ||
		handle->leaf[0] == '\0')
		return prefix_error(error, error_size,
			"runtime prefix delete input is invalid: %s", "invalid");
	if (handle->directory_fd >= 0) {
		struct lifecycle_stable_snapshot preflight;
		int preflight_result =
			preflight_existing_prefix_compatibility(handle, mode,
				owner_uid, owner_gid, &preflight,
				error, error_size);
		if (preflight_result != 0) {
			if (preflight_result > 0)
				prefix_error(error, error_size,
					"legacy prefix deletion is permitted only"
					" through explicit recreate: %s",
					"PREFIX_RECREATE_REQUIRED");
			return -1;
		}
	}
	struct lifecycle_names names;
	if (format_lifecycle_names(handle, &names,
			error, error_size) != 0)
		return -1;
	int lock_fd = acquire_lifecycle_lock(handle, &names,
		owner_uid, owner_gid,
		error, error_size);
	if (lock_fd < 0)
		return -1;
	enum lifecycle_recovery_status recovered =
		LIFECYCLE_NOT_RECOVERED;
	if (recover_interrupted_transaction(handle, &names, mode,
			owner_uid, owner_gid, &recovered,
			error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	if (handle->directory_fd < 0) {
		if (result != NULL) {
			memset(result, 0, sizeof(*result));
			result->action = DARLING_RUNTIME_PREFIX_DELETED;
			result->recovery =
				recovered == LIFECYCLE_RECOVERED
					? DARLING_RUNTIME_PREFIX_RECOVERED_TRANSACTION
					: DARLING_RUNTIME_PREFIX_NO_RECOVERY;
		}
		close(lock_fd);
		return 0;
	}
	if (darling_runtime_mode_verify_prefix_name(handle,
			error, error_size) != 0 ||
		ensure_prefix_not_running(handle, error, error_size) != 0 ||
		ensure_prefix_not_mounted(handle, error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	struct darling_runtime_prefix_state state;
	if (read_prefix_state_fd(handle->directory_fd, mode,
			owner_uid, owner_gid, &state, false,
			error, error_size) != 0 ||
		anchor_bound_sidecar(handle, &state,
			error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	struct lifecycle_transaction transaction;
	if (begin_transaction(handle, &names,
			LIFECYCLE_TRANSACTION_DELETE, mode,
			state.generation, state.generation + 1,
			owner_uid, owner_gid, &transaction,
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot begin prefix delete" : error);
		recover_preserving_failure(handle, &names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		close(lock_fd);
		return -1;
	}
	struct stat appeared;
	if (fstatat(handle->parent_fd, names.sidecar_stage, &appeared,
			AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT ||
		renameat2(handle->parent_fd, handle->sidecar_leaf,
			handle->parent_fd, names.sidecar_stage,
			RENAME_NOREPLACE) != 0 ||
		fsync_directory(handle->parent_fd, "sidecar deletion",
			error, error_size) != 0 ||
		advance_transaction_phase(handle, &names, &transaction,
			LIFECYCLE_PHASE_PREPARED,
			LIFECYCLE_PHASE_SIDECAR_PUBLISHED,
			"journal-sidecar-published",
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error),
			"cannot atomically detach runtime sidecar for deletion: %s",
			strerror(saved_errno));
		recover_preserving_failure(handle, &names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		close(lock_fd);
		return -1;
	}
	close(handle->sidecar_fd);
	handle->sidecar_fd = -1;
	if (fstatat(handle->parent_fd, names.stage, &appeared,
			AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT ||
		renameat2(handle->parent_fd, handle->leaf,
			handle->parent_fd, names.stage,
			RENAME_NOREPLACE) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error),
			"cannot atomically detach runtime prefix for deletion: %s",
			strerror(saved_errno));
		recover_preserving_failure(handle, &names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		close(lock_fd);
		return -1;
	}
	close(handle->directory_fd);
	handle->directory_fd = -1;
	if (handle->workdir_fd >= 0) {
		close(handle->workdir_fd);
		handle->workdir_fd = -1;
	}
	handle->anchor_state = DARLING_RUNTIME_PREFIX_ANCHOR_MISSING;
	if (fsync_directory(handle->parent_fd, "prefix deletion",
			error, error_size) != 0 ||
		lifecycle_checkpoint("prefix-published",
			error, error_size) != 0 ||
		advance_transaction_phase(handle, &names, &transaction,
			LIFECYCLE_PHASE_SIDECAR_PUBLISHED,
			LIFECYCLE_PHASE_PREFIX_PUBLISHED,
			"journal-prefix-published",
			error, error_size) != 0 ||
		advance_transaction_phase(handle, &names, &transaction,
			LIFECYCLE_PHASE_PREFIX_PUBLISHED,
			LIFECYCLE_PHASE_CLEANUP,
			"cleanup-start",
			error, error_size) != 0 ||
		remove_named_tree(handle->parent_fd, names.stage,
			false, error, error_size) != 0 ||
		remove_named_tree(handle->parent_fd, names.sidecar_stage,
			false, error, error_size) != 0 ||
		remove_prefix_workdir(handle, true,
			error, error_size) != 0 ||
		unlink_private_regular_at(handle->parent_fd,
			names.transaction, false,
			error, error_size) != 0 ||
		fsync_directory(handle->parent_fd,
			"delete transaction cleanup", error, error_size) != 0 ||
		lifecycle_checkpoint("cleanup-complete",
			error, error_size) != 0) {
		int saved_errno = errno;
		char saved_error[512];
		snprintf(saved_error, sizeof(saved_error), "%s",
			error == NULL ? "cannot finish prefix deletion" : error);
		recover_preserving_failure(handle, &names, mode,
			owner_uid, owner_gid, saved_errno, saved_error,
			error, error_size);
		close(lock_fd);
		return -1;
	}
	if (lifecycle_checkpoint("delete-cleanup-locked",
			error, error_size) != 0) {
		close(lock_fd);
		return -1;
	}
	if (result != NULL) {
		memset(result, 0, sizeof(*result));
		result->action = DARLING_RUNTIME_PREFIX_DELETED;
		result->recovery =
			recovered == LIFECYCLE_RECOVERED
				? DARLING_RUNTIME_PREFIX_RECOVERED_TRANSACTION
				: DARLING_RUNTIME_PREFIX_NO_RECOVERY;
		result->state = state;
	}
	close(lock_fd);
	return 0;
}
