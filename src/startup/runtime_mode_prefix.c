#define _GNU_SOURCE 1

#include "runtime_mode_prefix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void reset_handle(struct darling_runtime_prefix* handle)
{
	handle->directory_fd = -1;
	handle->parent_fd = -1;
	handle->workdir_fd = -1;
	handle->leaf[0] = '\0';
	handle->workdir_leaf[0] = '\0';
	handle->existed = false;
	handle->empty = false;
}

void darling_runtime_mode_close_prefix(
	struct darling_runtime_prefix* handle
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
	reset_handle(handle);
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
	struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
)
{
	if (prefix == NULL || *prefix == '\0' || handle == NULL)
		return prefix_error(error, error_size,
			"runtime prefix inspection input is missing: %s", "invalid");

	reset_handle(handle);
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
				handle->empty = true;
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
			handle->existed = true;
			if (inspect_empty(next, &handle->empty,
					error, error_size) != 0) {
				darling_runtime_mode_close_prefix(handle);
				return -1;
			}
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
	struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->parent_fd < 0 ||
		handle->leaf[0] == '\0')
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
	handle->empty = true;
	return 0;
}

int darling_runtime_mode_verify_prefix_name(
	const struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
)
{
	if (handle == NULL || handle->directory_fd < 0 ||
		handle->parent_fd < 0 || handle->leaf[0] == '\0')
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
	struct darling_runtime_prefix* handle,
	char* error,
	size_t error_size
)
{
	if (darling_runtime_mode_verify_prefix_name(handle,
			error, error_size) != 0)
		return -1;
	if (handle->workdir_fd >= 0)
		return 0;

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
		return 0;
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	if (close(fd) != 0)
		return prefix_error(error, error_size,
			"cannot close prefix initialization file: %s",
			strerror(errno));
	return 0;
}

int darling_runtime_mode_setup_prefix(
	struct darling_runtime_prefix* handle,
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
	handle->empty = false;
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
	const struct darling_runtime_prefix* handle,
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
