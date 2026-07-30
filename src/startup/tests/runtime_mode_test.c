#define _GNU_SOURCE 1

#include "../runtime_credentials.h"
#include "../runtime_mode.h"
#include "../runtime_mode_prefix.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum checkpoint_injection {
	CHECKPOINT_NONE,
	CHECKPOINT_FAILURE,
	CHECKPOINT_SIGINT,
	CHECKPOINT_PAUSE,
};

static enum checkpoint_injection checkpoint_injection;
static const char* checkpoint_phase;
static int checkpoint_ready_fd = -1;
static int checkpoint_release_fd = -1;

static const char* fsync_failure_suffix;
static int fsync_failure_observed;
static bool fsync_trace_enabled;
static size_t fsync_trace_count;
static char fsync_trace[256][4096];

int fsync(int fd)
{
	char descriptor[64];
	char path[4096];
	int length = snprintf(descriptor, sizeof(descriptor),
		"/proc/self/fd/%d", fd);
	ssize_t path_length = length > 0 &&
			(size_t)length < sizeof(descriptor)
		? readlink(descriptor, path, sizeof(path) - 1)
		: -1;
	if (path_length > 0) {
		path[path_length] = '\0';
		if (fsync_trace_enabled &&
			fsync_trace_count <
				sizeof(fsync_trace) / sizeof(fsync_trace[0])) {
			snprintf(fsync_trace[fsync_trace_count],
				sizeof(fsync_trace[fsync_trace_count]),
				"%s", path);
			fsync_trace_count++;
		}
		if (fsync_failure_suffix != NULL) {
			size_t path_size = (size_t)path_length;
			size_t suffix_size = strlen(fsync_failure_suffix);
			if (path_size >= suffix_size &&
				strcmp(path + path_size - suffix_size,
					fsync_failure_suffix) == 0) {
				fsync_failure_observed = 1;
				fsync_failure_suffix = NULL;
				errno = EIO;
				return -1;
			}
		}
	}
	return (int)syscall(SYS_fsync, fd);
}

#ifdef DARLING_RUNTIME_PREFIX_LIFECYCLE_TESTING
extern const char* darling_runtime_prefix_test_mountinfo_path;

int darling_runtime_prefix_test_checkpoint(const char* phase)
{
	if (checkpoint_injection == CHECKPOINT_NONE ||
		checkpoint_phase == NULL ||
		strcmp(checkpoint_phase, phase) != 0)
		return 0;
	enum checkpoint_injection injection = checkpoint_injection;
	checkpoint_injection = CHECKPOINT_NONE;
	if (injection == CHECKPOINT_PAUSE) {
		char byte = 'R';
		if (checkpoint_ready_fd < 0 ||
			checkpoint_release_fd < 0 ||
			write(checkpoint_ready_fd, &byte, 1) != 1 ||
			read(checkpoint_release_fd, &byte, 1) != 1) {
			errno = EIO;
			return -1;
		}
		return 0;
	}
	if (injection == CHECKPOINT_SIGINT) {
		raise(SIGINT);
		_exit(127);
	}
	errno = EIO;
	return -1;
}
#endif

static void require(int condition, const char* message)
{
	if (!condition) {
		fprintf(stderr, "FAIL %s\n", message);
		exit(1);
	}
}

static void write_exact_file(const char* path, const char* content)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	require(fd >= 0, "create fixture file");
	size_t length = strlen(content);
	require(write(fd, content, length) == (ssize_t)length,
		"write fixture file");
	require(close(fd) == 0, "close fixture file");
}

static void require_exact_file(const char* path, const char* content)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	require(fd >= 0, "open fixture file for immutable comparison");
	char observed[128] = {0};
	ssize_t length = read(fd, observed, sizeof(observed));
	require(close(fd) == 0, "close immutable comparison file");
	require(length == (ssize_t)strlen(content) &&
		memcmp(observed, content, (size_t)length) == 0,
		"rejected prefix path mutated its target");
}

static int remove_fixture_entry(
	const char* path,
	const struct stat* status,
	int type,
	struct FTW* state
)
{
	(void)status;
	(void)state;
	return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void remove_fixture_tree(const char* path)
{
	require(nftw(path, remove_fixture_entry, 32, FTW_DEPTH | FTW_PHYS) == 0,
		"remove completed prefix fixture");
}

static int leaked_temporary;

static int find_temporary_entry(
	const char* path,
	const struct stat* status,
	int type,
	struct FTW* state
)
{
	(void)status;
	(void)type;
	const char* name = path + state->base;
	if (strncmp(name, ".darling-dir-", strlen(".darling-dir-")) == 0 ||
		strstr(name, ".tmp.") != NULL ||
		strstr(name, ".darling-prefix-stage-v3-") != NULL ||
		strstr(name, ".darling-prefix-sidecar-stage-v1-") != NULL ||
		strstr(name, ".darling-prefix-transaction-v3-") != NULL ||
		strcmp(name, ".darling-prefix-state-v3.tmp") == 0)
		leaked_temporary = 1;
	return 0;
}

static void require_no_temporary_entries(const char* path)
{
	leaked_temporary = 0;
	require(nftw(path, find_temporary_entry, 32, FTW_PHYS) == 0,
		"scan prefix fixture for lifecycle temporary entries");
	require(!leaked_temporary,
		"prefix lifecycle left a temporary directory or file");
}

static void resolve_ok(
	bool cli,
	const char* canonical,
	const char* rootless,
	const char* nooverlay,
	const char* eunion,
	enum darling_runtime_mode expected
)
{
	enum darling_runtime_mode mode = DARLING_RUNTIME_MODE_INVALID;
	char error[256] = {0};
	require(darling_runtime_mode_resolve(cli, canonical, rootless, nooverlay,
			eunion, DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY, true,
			&mode, error, sizeof(error)) == 0, error);
	require(mode == expected, "resolved runtime mode differs from expectation");
}

static void resolve_fails(
	bool cli,
	const char* canonical,
	const char* rootless,
	const char* nooverlay,
	const char* eunion,
	bool capable
)
{
	enum darling_runtime_mode mode = DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY;
	char error[256] = {0};
	require(darling_runtime_mode_resolve(cli, canonical, rootless, nooverlay,
			eunion, DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY, capable,
			&mode, error, sizeof(error)) != 0,
		"invalid runtime mode selection unexpectedly succeeded");
	require(mode == DARLING_RUNTIME_MODE_INVALID,
		"failed runtime mode selection retained a usable mode");
	require(error[0] != '\0', "failed runtime mode selection omitted its reason");
}

static void test_resolution(void)
{
	resolve_ok(false, NULL, NULL, NULL, NULL,
		DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY);
	resolve_ok(false, "privileged-overlay", NULL, NULL, NULL,
		DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY);
	resolve_ok(false, "privileged-copy", NULL, NULL, NULL,
		DARLING_RUNTIME_MODE_PRIVILEGED_COPY);
	resolve_ok(false, "privileged-eunion", NULL, NULL, NULL,
		DARLING_RUNTIME_MODE_PRIVILEGED_EUNION);
	resolve_ok(false, "rootless-eunion", NULL, NULL, NULL,
		DARLING_RUNTIME_MODE_ROOTLESS_EUNION);

	resolve_ok(false, NULL, "0", "1", "0",
		DARLING_RUNTIME_MODE_PRIVILEGED_COPY);
	resolve_ok(false, NULL, "0", "1", "1",
		DARLING_RUNTIME_MODE_PRIVILEGED_EUNION);
	resolve_ok(false, NULL, "1", "1", "1",
		DARLING_RUNTIME_MODE_ROOTLESS_EUNION);
	resolve_ok(true, NULL, NULL, NULL, NULL,
		DARLING_RUNTIME_MODE_ROOTLESS_EUNION);
	resolve_ok(true, "rootless-eunion", "1", "1", "1",
		DARLING_RUNTIME_MODE_ROOTLESS_EUNION);

	resolve_fails(false, "unknown", NULL, NULL, NULL, true);
	resolve_fails(false, NULL, "true", NULL, NULL, true);
	resolve_fails(false, NULL, "1", NULL, NULL, true);
	resolve_fails(false, NULL, "1", "1", "0", true);
	resolve_fails(false, NULL, "0", "0", "1", true);
	resolve_fails(false, "privileged-copy", "0", "0", "0", true);
	resolve_fails(true, "privileged-overlay", NULL, NULL, NULL, true);
	resolve_fails(true, NULL, "0", "1", "0", true);
	resolve_fails(false, "rootless-eunion", NULL, NULL, NULL, false);
	resolve_fails(false, "privileged-eunion", NULL, NULL, NULL, false);
}

static void test_process_boundary(void)
{
	char* argv[] = {"darling", "--rootless", "shell", NULL};
	struct darling_runtime_cli cli;
	char error[256] = {0};
	require(darling_runtime_mode_parse_cli(3, argv, &cli,
			error, sizeof(error)) == 0, error);
	require(cli.rootless && cli.command_index == 2 &&
			!cli.show_help && !cli.show_version,
		"exact launcher CLI was not parsed once");

	char* abbreviated[] = {"darling", "--root", "shell", NULL};
	require(darling_runtime_mode_parse_cli(3, abbreviated, &cli,
			error, sizeof(error)) != 0,
		"abbreviated --root was accepted as --rootless");
	require(strstr(error, "must be exact") != NULL,
		"abbreviated option rejection omitted the exact-option reason");

	char* terminated[] = {"darling", "--rootless", "--", "--root", NULL};
	require(darling_runtime_mode_parse_cli(4, terminated, &cli,
			error, sizeof(error)) == 0 &&
			cli.rootless && cli.command_index == 3,
		"option terminator did not preserve the command verbatim");

	char* help[] = {"darling", "--help", NULL};
	require(darling_runtime_mode_parse_cli(2, help, &cli,
			error, sizeof(error)) == 0 &&
			cli.show_help && cli.command_index == -1,
		"exact --help parsing failed");
	char* recreate[] = {
		"darling", "--rootless", "--confirm-prefix-recreate",
		"recreate-prefix", NULL,
	};
	require(darling_runtime_mode_parse_cli(4, recreate, &cli,
			error, sizeof(error)) == 0 &&
			cli.rootless && cli.confirm_prefix_recreate &&
			cli.command_index == 3,
		"exact recreate confirmation was not parsed");
	char* duplicate_recreate[] = {
		"darling", "--confirm-prefix-recreate",
		"--confirm-prefix-recreate", "recreate-prefix", NULL,
	};
	require(darling_runtime_mode_parse_cli(
			4, duplicate_recreate, &cli,
			error, sizeof(error)) != 0,
		"duplicate recreate confirmation was accepted");

	require(darling_runtime_mode_parse_cli(3, argv, &cli,
			error, sizeof(error)) == 0, error);
	setenv("DARLING_ROOTLESS", "1", 1);
	setenv("DARLING_NOOVERLAYFS", "1", 1);
	setenv("DARLING_EUNION", "1", 1);
	unsetenv(DARLING_RUNTIME_MODE_ENV);

	enum darling_runtime_mode mode;
	require(darling_runtime_mode_select_process(&cli, true,
			&mode, error, sizeof(error)) == 0, error);
	require(mode == DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
		"CLI did not select rootless-eunion");
	require(darling_runtime_mode_publish(mode, error, sizeof(error)) == 0,
		error);
	require(strcmp(getenv(DARLING_RUNTIME_MODE_ENV), "rootless-eunion") == 0,
		"canonical runtime mode was not published");
	require(getenv("DARLING_ROOTLESS") == NULL &&
		getenv("DARLING_NOOVERLAYFS") == NULL &&
		getenv("DARLING_EUNION") == NULL,
		"legacy flags crossed the launcher compatibility boundary");
	require(darling_runtime_mode_require_canonical_process(true,
			&mode, error, sizeof(error)) == 0, error);

	setenv("DARLING_ROOTLESS", "1", 1);
	require(darling_runtime_mode_require_canonical_process(true,
			&mode, error, sizeof(error)) != 0,
		"downstream accepted a repeated legacy mode decision");
	unsetenv("DARLING_ROOTLESS");

	unsetenv(DARLING_RUNTIME_MODE_ENV);
	require(darling_runtime_mode_select_process(&cli, false,
			&mode, error, sizeof(error)) != 0,
		"rootless CLI ignored a build without E-UNION capability");
}

static void test_rootless_credentials(void)
{
	char error[256] = {0};
	require(darling_runtime_verify_rootless_credentials(
			getuid(), getgid(), error, sizeof(error)) == 0,
		error);
	require(darling_runtime_drop_rootless_credentials(
			getuid(), getgid(), error, sizeof(error)) == 0,
		error);
	require(darling_runtime_verify_rootless_credentials(
			getuid(), getgid(), error, sizeof(error)) == 0,
		error);
	require(darling_runtime_verify_rootless_credentials(
			0, getgid(), error, sizeof(error)) != 0,
		"root user identity was accepted for rootless mode");
	require(darling_runtime_verify_rootless_credentials(
			getuid(), 0, error, sizeof(error)) != 0,
		"root group identity was accepted for rootless mode");
}

static void open_prefix_fixture(
	const char* path,
	darling_runtime_prefix handle
)
{
	char error[512] = {0};
	require(handle->anchor_state ==
			DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED &&
			handle->directory_fd < 0 && handle->parent_fd < 0 &&
			handle->workdir_fd < 0,
		"prefix fixture capability was not initialized");
	require(darling_runtime_mode_open_prefix(path, handle,
			error, sizeof(error)) == 0, error);
}

static void prepare_prefix_fixture(
	const char* path,
	darling_runtime_prefix handle,
	struct darling_runtime_prefix_lifecycle_result* result
)
{
	char error[512] = {0};
	open_prefix_fixture(path, handle);
	require(darling_runtime_prefix_prepare(handle,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), result,
			error, sizeof(error)) == 0, error);
}

static void create_legacy_prefix_fixture(const char* path)
{
	char error[512] = {0};
	darling_runtime_prefix handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	open_prefix_fixture(path, handle);
	require(darling_runtime_mode_setup_prefix(handle, "tester",
			getuid(), getgid(), error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_initialize_prefix_marker(handle,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	darling_runtime_mode_close_prefix(handle);
}

static size_t read_fd_file(
	int directory_fd,
	const char* name,
	char* content,
	size_t capacity
)
{
	int fd = openat(directory_fd, name,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	require(fd >= 0, "open fixture metadata");
	ssize_t length = read(fd, content, capacity);
	require(length >= 0 && (size_t)length < capacity,
		"read bounded fixture metadata");
	require(close(fd) == 0, "close fixture metadata");
	content[length] = '\0';
	return (size_t)length;
}

static void replace_fd_file(
	int directory_fd,
	const char* name,
	const char* content
)
{
	require(unlinkat(directory_fd, name, 0) == 0,
		"remove fixture metadata");
	int fd = openat(directory_fd, name,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	require(fd >= 0, "create replacement fixture metadata");
	size_t length = strlen(content);
	require(write(fd, content, length) == (ssize_t)length,
		"write replacement fixture metadata");
	require(fsync(fd) == 0, "persist replacement fixture metadata");
	require(close(fd) == 0, "close replacement fixture metadata");
}

static void delete_prefix_fixture(
	darling_runtime_prefix handle
)
{
	char error[512] = {0};
	struct darling_runtime_prefix_lifecycle_result result;
	require(darling_runtime_prefix_delete(handle,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			getuid(), getgid(), &result,
			error, sizeof(error)) == 0, error);
	require(result.action == DARLING_RUNTIME_PREFIX_DELETED,
		"delete lifecycle action mismatch");
	struct stat sidecar;
	require(fstatat(handle->parent_fd, handle->sidecar_leaf,
			&sidecar, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT,
		"delete lifecycle retained final sidecar");
	darling_runtime_mode_close_prefix(handle);
}

static void require_state(
	const darling_runtime_prefix handle,
	uint64_t generation
)
{
	char error[512] = {0};
	struct darling_runtime_prefix_state state;
	require(darling_runtime_prefix_read_state(handle,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			getuid(), getgid(), &state,
			error, sizeof(error)) == 0, error);
	require(state.schema_version ==
			DARLING_RUNTIME_PREFIX_STATE_SCHEMA_VERSION,
		"prefix state schema mismatch");
	require(state.runtime_mode ==
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
		"prefix state mode mismatch");
	require(state.generation == generation,
		"prefix state generation mismatch");
	require(state.owner_uid == getuid() && state.owner_gid == getgid(),
		"prefix state owner mismatch");
	struct stat sidecar_status;
	require(handle->sidecar_fd >= 0 &&
		fstat(handle->sidecar_fd, &sidecar_status) == 0 &&
		S_ISDIR(sidecar_status.st_mode) &&
		(sidecar_status.st_mode & 07777) == 0700 &&
		state.sidecar_device == sidecar_status.st_dev &&
		state.sidecar_inode == sidecar_status.st_ino,
		"prefix state does not bind retained private sidecar");
	require(strcmp(state.provenance,
			DARLING_RUNTIME_PREFIX_PROVENANCE) == 0,
		"prefix state provenance mismatch");
}

static void test_legacy_recreate_policy(
	const char* directory, const char* name)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	create_legacy_prefix_fixture(path);
	char sentinel[4096];
	snprintf(sentinel, sizeof(sentinel), "%s/private/legacy-sentinel",
		path);
	int sentinel_fd = open(sentinel,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(sentinel_fd >= 0 &&
		write(sentinel_fd, "legacy\n", 7) == 7 &&
		fsync(sentinel_fd) == 0 && close(sentinel_fd) == 0,
		"create legacy no-mutation sentinel");
	struct stat before;
	require(lstat(sentinel, &before) == 0,
		"snapshot legacy sentinel");

	darling_runtime_prefix recovered =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	open_prefix_fixture(path, recovered);
	struct darling_runtime_prefix_lifecycle_result result;
	require(darling_runtime_prefix_prepare(recovered,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) ==
			DARLING_RUNTIME_PREFIX_RECREATE_REQUIRED,
		"legacy prefix did not return typed recreate verdict");
	require(result.verdict ==
			DARLING_RUNTIME_PREFIX_VERDICT_RECREATE_REQUIRED &&
			strstr(error, "PREFIX_RECREATE_REQUIRED") != NULL,
		"legacy recreate verdict omitted typed diagnostics");
	struct stat after;
	require(lstat(sentinel, &after) == 0 &&
			before.st_dev == after.st_dev &&
			before.st_ino == after.st_ino &&
			before.st_size == after.st_size,
		"ordinary prepare mutated legacy prefix");

	char init_pid[4096];
	snprintf(init_pid, sizeof(init_pid), "%s/.init.pid", path);
	char pid_content[64];
	snprintf(pid_content, sizeof(pid_content), "%ld\n", (long)getpid());
	write_exact_file(init_pid, pid_content);
	memset(error, 0, sizeof(error));
	require(darling_runtime_prefix_recreate(recovered,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0 &&
			strstr(error, "live process") != NULL,
		"explicit recreation ignored a live prefix process");
	require_exact_file(sentinel, "legacy\n");
	require(unlink(init_pid) == 0,
		"remove live-process recreation fixture");

	char mountinfo_path[4096];
	snprintf(mountinfo_path, sizeof(mountinfo_path),
		"%s/mountinfo-fixture", directory);
	FILE* mountinfo = fopen(mountinfo_path, "we");
	require(mountinfo != NULL,
		"create mountinfo recreation fixture");
	require(fprintf(mountinfo,
			"36 25 0:32 / %s rw,relatime - tmpfs tmpfs rw\n",
			path) > 0 && fflush(mountinfo) == 0 &&
			fsync(fileno(mountinfo)) == 0 && fclose(mountinfo) == 0,
		"write mountinfo recreation fixture");
	darling_runtime_prefix_test_mountinfo_path = mountinfo_path;
	memset(error, 0, sizeof(error));
	require(darling_runtime_prefix_recreate(recovered,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0 &&
			strstr(error, "live mount") != NULL,
		"explicit recreation ignored a live prefix mount");
	darling_runtime_prefix_test_mountinfo_path = NULL;
	require_exact_file(sentinel, "legacy\n");
	require(unlink(mountinfo_path) == 0,
		"remove mountinfo recreation fixture");

	require(darling_runtime_prefix_recreate(recovered,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) == 0,
		error);
	require(result.action == DARLING_RUNTIME_PREFIX_RECREATED,
		"explicit legacy recreation action mismatch");
	require_state(recovered, 1);
	require(access(sentinel, F_OK) != 0 && errno == ENOENT,
		"explicit recreation retained legacy payload");
	struct stat legacy;
	require(fstatat(recovered->directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME, &legacy,
			AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT,
		"legacy marker survived explicit sidecar-v1 recreation");
	require_no_temporary_entries(directory);
	delete_prefix_fixture(recovered);
}

enum mutation_fixture_operation {
	MUTATION_FIXTURE_CREATE,
	MUTATION_FIXTURE_RECREATE,
	MUTATION_FIXTURE_DELETE,
};

static void run_mutation_interruption(
	const char* directory,
	const char* name,
	enum mutation_fixture_operation operation,
	enum checkpoint_injection injection,
	const char* phase
)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	if (operation != MUTATION_FIXTURE_CREATE) {
		darling_runtime_prefix initial =
			DARLING_RUNTIME_PREFIX_INITIALIZER;
		struct darling_runtime_prefix_lifecycle_result initial_result;
		prepare_prefix_fixture(path, initial, &initial_result);
		darling_runtime_mode_close_prefix(initial);
	}

	pid_t child = fork();
	require(child >= 0, "fork mutation interruption fixture");
	if (child == 0) {
		darling_runtime_prefix handle =
			DARLING_RUNTIME_PREFIX_INITIALIZER;
		char error[512] = {0};
		if (darling_runtime_mode_open_prefix(path, handle,
				error, sizeof(error)) != 0)
			_exit(90);
		checkpoint_injection = injection;
		checkpoint_phase = phase;
		struct darling_runtime_prefix_lifecycle_result result;
		int outcome;
		switch (operation) {
			case MUTATION_FIXTURE_CREATE:
				outcome = darling_runtime_prefix_prepare(handle,
					DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
					"tester", getuid(), getgid(), &result,
					error, sizeof(error));
				break;
			case MUTATION_FIXTURE_RECREATE:
				outcome = darling_runtime_prefix_recreate(handle,
					DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
					"tester", getuid(), getgid(), &result,
					error, sizeof(error));
				break;
			case MUTATION_FIXTURE_DELETE:
				outcome = darling_runtime_prefix_delete(handle,
					DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
					getuid(), getgid(), &result,
					error, sizeof(error));
				break;
			default:
				_exit(92);
		}
		darling_runtime_mode_close_prefix(handle);
		_exit(outcome == 0 ? 91 : 0);
	}
	int status;
	require(waitpid(child, &status, 0) == child,
		"wait for mutation interruption fixture");
	if (injection == CHECKPOINT_FAILURE)
		require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			"injected mutation failure did not propagate");
	else
		require((WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) ||
				(WIFEXITED(status) && WEXITSTATUS(status) == 127),
			"injected mutation SIGINT did not interrupt");

	checkpoint_injection = CHECKPOINT_NONE;
	checkpoint_phase = NULL;
	darling_runtime_prefix recovered =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	require(darling_runtime_mode_open_prefix(path, recovered,
			error, sizeof(error)) == 0, error);
	struct darling_runtime_prefix_lifecycle_result result;
	if (operation == MUTATION_FIXTURE_DELETE) {
		require(darling_runtime_prefix_delete(recovered,
				DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
				getuid(), getgid(), &result,
				error, sizeof(error)) == 0, error);
		require(result.action == DARLING_RUNTIME_PREFIX_DELETED &&
				recovered->anchor_state ==
					DARLING_RUNTIME_PREFIX_ANCHOR_MISSING,
			"interrupted delete did not recover to missing");
		darling_runtime_mode_close_prefix(recovered);
	} else {
		require(darling_runtime_prefix_prepare(recovered,
				DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
				"tester", getuid(), getgid(), &result,
				error, sizeof(error)) == 0, error);
		require(result.state.generation >= 1 &&
				result.state.generation <=
					(operation == MUTATION_FIXTURE_RECREATE ? 2 : 1),
			"interrupted create/recreate reached an invalid generation");
		require_state(recovered, result.state.generation);
		delete_prefix_fixture(recovered);
	}
	require_no_temporary_entries(directory);
}

static void run_staged_durability_failure(
	const char* directory,
	const char* name,
	const char* failed_suffix
)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	darling_runtime_prefix handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	open_prefix_fixture(path, handle);
	fsync_failure_suffix = failed_suffix;
	fsync_failure_observed = 0;
	struct darling_runtime_prefix_lifecycle_result result;
	int outcome = darling_runtime_prefix_prepare(handle,
		DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
		"tester", getuid(), getgid(), &result,
		error, sizeof(error));
	fsync_failure_suffix = NULL;
	require(fsync_failure_observed,
		"staged durability fixture did not reach the selected fsync");
	require(outcome != 0,
		"staged durability fsync failure did not fail the transaction");
	darling_runtime_mode_close_prefix(handle);

	darling_runtime_prefix recovered =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	prepare_prefix_fixture(path, recovered, &result);
	require_state(recovered, 1);
	delete_prefix_fixture(recovered);
	require_no_temporary_entries(directory);
}

static size_t fsync_suffix_after(const char* suffix, size_t after)
{
	size_t suffix_size = strlen(suffix);
	for (size_t index = after; index < fsync_trace_count; ++index) {
		size_t path_size = strlen(fsync_trace[index]);
		if (path_size >= suffix_size &&
			strcmp(fsync_trace[index] + path_size - suffix_size,
				suffix) == 0)
			return index;
	}
	return SIZE_MAX;
}

static void run_staged_durability_order(const char* directory)
{
	char path[2048];
	snprintf(path, sizeof(path), "%s/durability-order", directory);
	darling_runtime_prefix handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	struct darling_runtime_prefix_lifecycle_result result;
	fsync_trace_count = 0;
	fsync_trace_enabled = true;
	prepare_prefix_fixture(path, handle, &result);
	fsync_trace_enabled = false;

	size_t passwd = fsync_suffix_after("/private/etc/passwd", 0);
	size_t master = fsync_suffix_after("/private/etc/master.passwd", 0);
	size_t group = fsync_suffix_after("/private/etc/group", 0);
	size_t last_file = passwd;
	if (master > last_file)
		last_file = master;
	if (group > last_file)
		last_file = group;
	size_t etc = fsync_suffix_after("/private/etc", last_file + 1);
	size_t private_dir = etc == SIZE_MAX
		? SIZE_MAX
		: fsync_suffix_after("/private", etc + 1);
	require(passwd != SIZE_MAX && master != SIZE_MAX &&
			group != SIZE_MAX && etc != SIZE_MAX &&
			private_dir != SIZE_MAX,
		"staged tree did not fsync files and directories bottom-up");

	bool root_after_private = false;
	for (size_t index = private_dir + 1;
			index < fsync_trace_count; ++index) {
		const char* base = strrchr(fsync_trace[index], '/');
		if (base != NULL &&
			strncmp(base + 1, ".darling-prefix-stage-v3-",
				strlen(".darling-prefix-stage-v3-")) == 0) {
			root_after_private = true;
			break;
		}
	}
	require(root_after_private,
		"staged root was not fsynced after nested directories");
	delete_prefix_fixture(handle);
	require_no_temporary_entries(directory);
}

static uint64_t test_lifecycle_name_hash(const char* name)
{
	uint64_t value = UINT64_C(1469598103934665603);
	while (*name != '\0') {
		value ^= (unsigned char)*name++;
		value *= UINT64_C(1099511628211);
	}
	return value;
}

static void write_pipe_byte(int fd, const char* message)
{
	char byte = 'R';
	require(write(fd, &byte, 1) == 1, message);
}

static void read_pipe_byte(int fd, const char* message)
{
	char byte;
	require(read(fd, &byte, 1) == 1, message);
}

static int poll_pipe_byte(int fd, int timeout_ms)
{
	struct pollfd descriptor = {
		.fd = fd,
		.events = POLLIN,
	};
	int result;
	do {
		result = poll(&descriptor, 1, timeout_ms);
	} while (result < 0 && errno == EINTR);
	require(result >= 0, "poll lifecycle checkpoint pipe");
	return result == 1 && (descriptor.revents & POLLIN) != 0;
}

static pid_t fork_paused_lifecycle(
	const char* path,
	const char* phase,
	int ready_fd,
	int release_fd,
	bool delete_prefix
)
{
	pid_t child = fork();
	require(child >= 0, "fork paused lifecycle process");
	if (child != 0)
		return child;
	checkpoint_injection = CHECKPOINT_PAUSE;
	checkpoint_phase = phase;
	checkpoint_ready_fd = ready_fd;
	checkpoint_release_fd = release_fd;
	darling_runtime_prefix handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	if (darling_runtime_mode_open_prefix(path, handle,
			error, sizeof(error)) != 0)
		_exit(90);
	struct darling_runtime_prefix_lifecycle_result result;
	int outcome = delete_prefix
		? darling_runtime_prefix_delete(handle,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			getuid(), getgid(), &result, error, sizeof(error))
		: darling_runtime_prefix_prepare(handle,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error));
	darling_runtime_mode_close_prefix(handle);
	_exit(outcome == 0 ? 0 : 91);
}

static void wait_success(pid_t child, const char* message)
{
	int status;
	require(waitpid(child, &status, 0) == child &&
			WIFEXITED(status) && WEXITSTATUS(status) == 0,
		message);
}

static void wait_failure(pid_t child, const char* message)
{
	int status;
	require(waitpid(child, &status, 0) == child &&
			WIFEXITED(status) && WEXITSTATUS(status) != 0,
		message);
}

static void test_persistent_lifecycle_lock_serializes_three_processes(
	const char* directory
)
{
	const char* leaf = "lock-race-prefix";
	char path[2048];
	char lock_path[2048];
	snprintf(path, sizeof(path), "%s/%s", directory, leaf);
	snprintf(lock_path, sizeof(lock_path),
		"%s/.darling-prefix-lock-v2-%016" PRIx64,
		directory, test_lifecycle_name_hash(leaf));
	darling_runtime_prefix initial =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	struct darling_runtime_prefix_lifecycle_result result;
	prepare_prefix_fixture(path, initial, &result);
	darling_runtime_mode_close_prefix(initial);

	int waiter_ready[2];
	int waiter_release[2];
	int deleter_ready[2];
	int deleter_release[2];
	int creator_ready[2];
	int creator_release[2];
	require(pipe(waiter_ready) == 0 && pipe(waiter_release) == 0 &&
			pipe(deleter_ready) == 0 && pipe(deleter_release) == 0 &&
			pipe(creator_ready) == 0 && pipe(creator_release) == 0,
		"create three-process lifecycle pipes");

	pid_t waiter = fork_paused_lifecycle(path, "lock-opened",
		waiter_ready[1], waiter_release[0], false);
	read_pipe_byte(waiter_ready[0],
		"waiter did not open the persistent lock inode");

	pid_t deleter = fork_paused_lifecycle(path, "delete-cleanup-locked",
		deleter_ready[1], deleter_release[0], true);
	read_pipe_byte(deleter_ready[0],
		"deleter did not reach locked cleanup");
	struct stat named_lock;
	bool lock_remained_named =
		lstat(lock_path, &named_lock) == 0 &&
		S_ISREG(named_lock.st_mode);

	pid_t creator = fork_paused_lifecycle(path, "lock-acquired",
		creator_ready[1], creator_release[0], false);
	bool creator_split_lock = poll_pipe_byte(creator_ready[0], 250);

	write_pipe_byte(deleter_release[1], "release deleter checkpoint");
	wait_success(deleter, "deleter process failed");
	if (!creator_split_lock)
		require(poll_pipe_byte(creator_ready[0], 5000),
			"creator did not acquire persistent lock after delete released it");
	write_pipe_byte(creator_release[1], "release creator checkpoint");
	wait_success(creator, "creator process failed");
	write_pipe_byte(waiter_release[1], "release waiter checkpoint");
	wait_failure(waiter,
		"stale waiter did not fail closed after prefix replacement");

	for (int fd = 0; fd < 2; ++fd) {
		close(waiter_ready[fd]);
		close(waiter_release[fd]);
		close(deleter_ready[fd]);
		close(deleter_release[fd]);
		close(creator_ready[fd]);
		close(creator_release[fd]);
	}
	require(lock_remained_named,
		"delete unlinked the persistent lifecycle lock while held");
	require(!creator_split_lock,
		"third process acquired a replacement lifecycle lock concurrently");

	darling_runtime_prefix cleanup =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	open_prefix_fixture(path, cleanup);
	delete_prefix_fixture(cleanup);
	require(lstat(lock_path, &named_lock) == 0 &&
			S_ISREG(named_lock.st_mode) &&
			named_lock.st_nlink == 1,
		"persistent lifecycle lock was not retained after final delete");
}

static void test_lifecycle_lock_race(void)
{
	char root[] = "/tmp/darling-prefix-lock-race-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp lifecycle lock fixture");
	test_persistent_lifecycle_lock_serializes_three_processes(directory);
	remove_fixture_tree(directory);
}

static void test_staged_tree_durability(void)
{
	char root[] = "/tmp/darling-prefix-durability-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp lifecycle durability fixture");
	run_staged_durability_failure(directory, "file-fsync",
		"/private/etc/passwd");
	run_staged_durability_failure(directory, "directory-fsync",
		"/private/etc");
	run_staged_durability_order(directory);
	remove_fixture_tree(directory);
}

static void test_mutation_interruption_coverage(void)
{
	static const struct {
		const char* label;
		const char* phase;
	} cases[] = {
		{"transaction", "transaction-prepared"},
		{"replacement", "replacement-staged"},
		{"sidecar", "sidecar-published"},
		{"prefix", "prefix-published"},
		{"cleanup", "cleanup-start"},
	};
	static const enum checkpoint_injection injections[] = {
		CHECKPOINT_FAILURE,
		CHECKPOINT_SIGINT,
	};
	char root[] = "/tmp/darling-prefix-interruption-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp lifecycle interruption fixture");
	unsigned sequence = 0;
	for (size_t operation = MUTATION_FIXTURE_CREATE;
			operation <= MUTATION_FIXTURE_RECREATE;
			operation++) {
		for (size_t injection = 0;
				injection < sizeof(injections) / sizeof(injections[0]);
				injection++) {
			for (size_t phase = 0;
					phase < sizeof(cases) / sizeof(cases[0]);
					phase++) {
				char name[128];
				snprintf(name, sizeof(name),
					"operation-%zu-%s-%u",
					operation, cases[phase].label, sequence++);
				run_mutation_interruption(directory, name,
					(enum mutation_fixture_operation)operation,
					injections[injection], cases[phase].phase);
			}
		}
	}
	static const struct {
		const char* label;
		const char* phase;
	} delete_cases[] = {
		{"transaction", "transaction-prepared"},
		{"sidecar", "journal-sidecar-published"},
		{"prefix", "prefix-published"},
		{"cleanup", "cleanup-start"},
	};
	for (size_t injection = 0;
			injection < sizeof(injections) / sizeof(injections[0]);
			injection++) {
		for (size_t phase = 0;
				phase < sizeof(delete_cases) / sizeof(delete_cases[0]);
				phase++) {
			char name[128];
			snprintf(name, sizeof(name), "delete-%s-%u",
				delete_cases[phase].label, sequence++);
			run_mutation_interruption(directory, name,
				MUTATION_FIXTURE_DELETE, injections[injection],
				delete_cases[phase].phase);
		}
	}
	require(sequence == 28,
		"real create/recreate/delete interruption matrix was incomplete");
	remove_fixture_tree(directory);
}

static void test_owned_capability_rejects_reopen(void)
{
	char root[] = "/tmp/darling-prefix-owned-handle-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp owned capability fixture");
	char path[2048];
	snprintf(path, sizeof(path), "%s/prefix", directory);
	require(mkdir(path, 0700) == 0, "create owned capability prefix");
	darling_runtime_prefix handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	open_prefix_fixture(path, handle);
	int directory_fd = handle->directory_fd;
	int parent_fd = handle->parent_fd;
	char error[512] = {0};
	require(darling_runtime_mode_open_prefix(path, handle,
			error, sizeof(error)) != 0,
		"open_prefix accepted an already-owned capability");
	require(handle->directory_fd == directory_fd &&
			handle->parent_fd == parent_fd &&
			fcntl(directory_fd, F_GETFD) >= 0 &&
			fcntl(parent_fd, F_GETFD) >= 0,
		"rejected capability reopen leaked or replaced owned descriptors");
	darling_runtime_mode_close_prefix(handle);
	require(rmdir(path) == 0, "remove owned capability prefix");
	require(rmdir(directory) == 0, "remove owned capability fixture");
}

static enum darling_runtime_prefix_test_recovery expected_recovery(
	enum darling_runtime_prefix_test_operation operation,
	enum darling_runtime_prefix_test_phase phase,
	enum darling_runtime_prefix_test_stable_relation stable
)
{
	switch (operation) {
		case DARLING_RUNTIME_PREFIX_TEST_CREATE:
			if ((phase == DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_REPLACEMENT_STAGED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_SIDECAR_PUBLISHED) &&
				stable == DARLING_RUNTIME_PREFIX_TEST_MISSING)
				return DARLING_RUNTIME_PREFIX_TEST_ROLLBACK;
			if ((phase == DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_REPLACEMENT_STAGED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_SIDECAR_PUBLISHED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_PREFIX_PUBLISHED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_CLEANUP) &&
				stable == DARLING_RUNTIME_PREFIX_TEST_NEW_CURRENT)
				return DARLING_RUNTIME_PREFIX_TEST_FINISH;
			return DARLING_RUNTIME_PREFIX_TEST_INVALID;
		case DARLING_RUNTIME_PREFIX_TEST_RECREATE:
			if ((phase == DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_REPLACEMENT_STAGED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_SIDECAR_PUBLISHED) &&
				(stable == DARLING_RUNTIME_PREFIX_TEST_OLD_EMPTY ||
				 stable == DARLING_RUNTIME_PREFIX_TEST_OLD_LEGACY ||
				 stable == DARLING_RUNTIME_PREFIX_TEST_OLD_CURRENT))
				return DARLING_RUNTIME_PREFIX_TEST_ROLLBACK;
			if ((phase == DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_REPLACEMENT_STAGED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_SIDECAR_PUBLISHED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_PREFIX_PUBLISHED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_CLEANUP) &&
				stable == DARLING_RUNTIME_PREFIX_TEST_NEW_CURRENT)
				return DARLING_RUNTIME_PREFIX_TEST_FINISH;
			return DARLING_RUNTIME_PREFIX_TEST_INVALID;
		case DARLING_RUNTIME_PREFIX_TEST_DELETE:
			if ((phase == DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_SIDECAR_PUBLISHED) &&
				stable == DARLING_RUNTIME_PREFIX_TEST_OLD_CURRENT)
				return DARLING_RUNTIME_PREFIX_TEST_ROLLBACK;
			if ((phase == DARLING_RUNTIME_PREFIX_TEST_PREPARED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_SIDECAR_PUBLISHED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_PREFIX_PUBLISHED ||
				 phase == DARLING_RUNTIME_PREFIX_TEST_CLEANUP) &&
				stable == DARLING_RUNTIME_PREFIX_TEST_MISSING)
				return DARLING_RUNTIME_PREFIX_TEST_FINISH;
			return DARLING_RUNTIME_PREFIX_TEST_INVALID;
	}
	return DARLING_RUNTIME_PREFIX_TEST_INVALID;
}

static void test_recovery_matrix(void)
{
	unsigned combinations = 0;
	for (enum darling_runtime_prefix_test_operation operation =
			DARLING_RUNTIME_PREFIX_TEST_CREATE;
			operation <= DARLING_RUNTIME_PREFIX_TEST_DELETE;
			operation++) {
		for (enum darling_runtime_prefix_test_phase phase =
				DARLING_RUNTIME_PREFIX_TEST_PREPARED;
				phase <= DARLING_RUNTIME_PREFIX_TEST_CLEANUP;
				phase++) {
			for (enum darling_runtime_prefix_test_stable_relation stable =
					DARLING_RUNTIME_PREFIX_TEST_MISSING;
					stable <= DARLING_RUNTIME_PREFIX_TEST_UNEXPECTED;
					stable++) {
				require(darling_runtime_prefix_test_recovery_matrix(
						operation, phase, stable) ==
						(int)expected_recovery(
							operation, phase, stable),
					"stable-state/journal-phase recovery matrix mismatch");
				combinations++;
			}
		}
	}
	require(combinations == 90,
		"recovery matrix did not cover every combination");
}

static void test_prefix_lifecycle(void)
{
	char root[] = "/tmp/darling-prefix-lifecycle-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp lifecycle fixture");
	char prefix[2048];
	char sentinel[2048];
	snprintf(prefix, sizeof(prefix), "%s/prefix", directory);
	snprintf(sentinel, sizeof(sentinel), "%s/lower-sentinel", directory);
	write_exact_file(sentinel, "immutable-lower\n");

	darling_runtime_prefix created =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	struct darling_runtime_prefix_lifecycle_result result;
	prepare_prefix_fixture(prefix, created, &result);
	require(result.action == DARLING_RUNTIME_PREFIX_CREATED,
		"missing prefix was not created");
	require(result.recovery == DARLING_RUNTIME_PREFIX_NO_RECOVERY,
		"fresh create unexpectedly reported recovery");
	require_state(created, 1);
	struct stat generation_one;
	require(fstat(created->directory_fd, &generation_one) == 0,
		"stat generation one prefix");

	darling_runtime_prefix moved =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	char error[512] = {0};
	require(darling_runtime_prefix_move(moved, created,
			error, sizeof(error)) == 0, error);
	require(created->anchor_state == DARLING_RUNTIME_PREFIX_ANCHOR_MOVED &&
			created->directory_fd < 0 && created->parent_fd < 0,
		"move did not invalidate source capability");
	require(darling_runtime_prefix_read_state(created,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			getuid(), getgid(), &result.state,
			error, sizeof(error)) != 0,
		"moved-from capability remained usable");

	require(darling_runtime_prefix_prepare(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) == 0, error);
	require(result.action == DARLING_RUNTIME_PREFIX_REUSED &&
			result.state.generation == 1,
		"valid current prefix was not reused");

	require(darling_runtime_mode_prepare_workdir(moved,
			error, sizeof(error)) == 0, error);
	int scratch = openat(moved->workdir_fd, "owned-scratch",
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(scratch >= 0 && close(scratch) == 0,
		"create lifecycle-owned workdir content");
	require(darling_runtime_prefix_recreate(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) == 0, error);
	require(result.action == DARLING_RUNTIME_PREFIX_RECREATED &&
			result.state.generation == 2,
		"recreate did not advance generation");
	struct stat generation_two;
	require(fstat(moved->directory_fd, &generation_two) == 0 &&
			generation_two.st_ino != generation_one.st_ino,
		"recreate did not atomically replace prefix identity");
	require_state(moved, 2);
	char workdir[4096];
	snprintf(workdir, sizeof(workdir), "%s.workdir", prefix);
	require(access(workdir, F_OK) != 0 && errno == ENOENT,
		"recreate left lifecycle workdir");
	require_exact_file(sentinel, "immutable-lower\n");

	char state_content[2048];
	size_t state_size = read_fd_file(moved->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME,
		state_content, sizeof(state_content));
	require(state_size > 0, "capture canonical state fixture");
	char hostile[2048];
	snprintf(hostile, sizeof(hostile), "%s", state_content);
	char* schema = strstr(hostile, "schema_version=3\n");
	require(schema != NULL, "locate state schema fixture");
	memcpy(schema, "schema_version=9\n", strlen("schema_version=9\n"));
	replace_fd_file(moved->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME, hostile);
	require(darling_runtime_prefix_prepare(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0,
		"newer prefix schema was accepted");
	replace_fd_file(moved->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME, state_content);
	require_state(moved, 2);
	require(darling_runtime_prefix_read_state(moved,
			DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY,
			getuid(), getgid(), &result.state,
			error, sizeof(error)) != 0,
		"incompatible runtime mode reused typed prefix state");

	require(fchmodat(moved->directory_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME, 0644, 0) == 0,
		"make state metadata mode hostile");
	require(darling_runtime_prefix_prepare(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0,
		"hostile state metadata mode was accepted");
	require(fchmodat(moved->directory_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME, 0600, 0) == 0,
		"restore state metadata mode");

	char state_alias[2048];
	snprintf(state_alias, sizeof(state_alias), "%s/state-hardlink",
		directory);
	require(linkat(moved->directory_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME,
			AT_FDCWD, state_alias, 0) == 0,
		"create hostile state hardlink");
	require(darling_runtime_prefix_prepare(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0,
		"multiply linked state metadata was accepted");
	require(unlink(state_alias) == 0,
		"remove hostile state hardlink");

	require(unlinkat(moved->directory_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME, 0) == 0 &&
		symlinkat("private/etc/passwd", moved->directory_fd,
			DARLING_RUNTIME_PREFIX_STATE_NAME) == 0,
		"create hostile state symlink");
	require(darling_runtime_prefix_prepare(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0,
		"state metadata symlink was accepted");
	replace_fd_file(moved->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME, state_content);

	replace_fd_file(moved->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME,
		"DARLING_PREFIX_STATE_V3\nschema_version=3\n");
	require(darling_runtime_prefix_prepare(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &result,
			error, sizeof(error)) != 0,
		"truncated state metadata was accepted");
	replace_fd_file(moved->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME, state_content);
	require_state(moved, 2);

	char other_path[2048];
	snprintf(other_path, sizeof(other_path), "%s/other-prefix",
		directory);
	darling_runtime_prefix other =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	struct darling_runtime_prefix_lifecycle_result other_result;
	prepare_prefix_fixture(other_path, other, &other_result);
	char other_state[2048];
	read_fd_file(other->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME,
		other_state, sizeof(other_state));
	replace_fd_file(other->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME, state_content);
	require(darling_runtime_prefix_prepare(other,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			"tester", getuid(), getgid(), &other_result,
			error, sizeof(error)) != 0,
		"cross-prefix typed state was accepted");
	replace_fd_file(other->directory_fd,
		DARLING_RUNTIME_PREFIX_STATE_NAME, other_state);
	delete_prefix_fixture(other);

	struct darling_runtime_prefix_lifecycle_result deleted;
	require(darling_runtime_prefix_delete(moved,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			getuid(), getgid(), &deleted,
			error, sizeof(error)) == 0, error);
	require(deleted.action == DARLING_RUNTIME_PREFIX_DELETED &&
			moved->anchor_state == DARLING_RUNTIME_PREFIX_ANCHOR_MISSING,
		"delete did not leave a missing anchored capability");
	require(access(prefix, F_OK) != 0 && errno == ENOENT,
		"delete left prefix root");
	require_exact_file(sentinel, "immutable-lower\n");
	darling_runtime_mode_close_prefix(moved);

	test_legacy_recreate_policy(directory, "legacy-recreate-policy");

	require_no_temporary_entries(directory);
	require_exact_file(sentinel, "immutable-lower\n");
	require(unlink(sentinel) == 0, "remove lifecycle lower sentinel");
	remove_fixture_tree(directory);
}

static void test_prefix_marker(void)
{
	char root[] = "/tmp/darling-runtime-mode-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp failed");
	char error[256] = {0};
	char path[2048];
	char sentinel[4096];

	darling_runtime_prefix missing =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	snprintf(path, sizeof(path), "%s/missing", directory);
	require(darling_runtime_mode_open_prefix(path, missing,
			error, sizeof(error)) == 0 &&
			missing->anchor_state ==
				DARLING_RUNTIME_PREFIX_ANCHOR_MISSING &&
			missing->directory_fd < 0 && missing->parent_fd >= 0,
		"missing prefix did not retain its verified parent");
	require(darling_runtime_mode_setup_prefix(missing, "tester",
			getuid(), getgid(), error, sizeof(error)) == 0, error);
	require(missing->directory_fd >= 0,
		"safe setup did not retain the created prefix fd");
	require(darling_runtime_mode_initialize_prefix_marker(missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_validate_prefix_marker(missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_prefix_needs_initialization(missing,
			error, sizeof(error)) == 0,
		"fd-relative setup did not create a complete prefix");
	require(darling_runtime_mode_validate_prefix_marker(missing,
			DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY,
			error, sizeof(error)) != 0,
		"prefix mode mismatch was accepted");
	require(unlinkat(missing->directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME, 0) == 0,
		"remove regular runtime mode marker");
	require(symlinkat("private/etc/passwd", missing->directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME) == 0,
		"create marker symlink fixture");
	require(darling_runtime_mode_validate_prefix_marker(missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) != 0,
		"runtime mode marker symlink was accepted");
	require(unlinkat(missing->directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME, 0) == 0,
		"remove marker symlink fixture");
	require(darling_runtime_mode_initialize_prefix_marker(missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_prepare_workdir(missing,
			error, sizeof(error)) == 0 &&
			missing->workdir_fd >= 0,
		"fd-relative workdir preparation failed");
	struct stat workdir_status;
	require(fstat(missing->workdir_fd, &workdir_status) == 0 &&
			S_ISDIR(workdir_status.st_mode),
		"retained workdir descriptor is not a directory");
	require(darling_runtime_mode_verify_prefix_name(missing,
			error, sizeof(error)) == 0,
		"retained prefix name verification failed");
	require(darling_runtime_mode_write_relative_atomic(missing,
			".init.pid", "123\n", 0600,
			error, sizeof(error)) == 0, error);
	struct stat state_status;
	require(darling_runtime_mode_stat_relative(missing,
			".init.pid", &state_status,
			error, sizeof(error)) == 0 &&
			S_ISREG(state_status.st_mode) &&
			state_status.st_size == 4,
		"fd-relative state publication failed");
	require(darling_runtime_mode_unlink_relative(missing,
			".init.pid", 0, false,
			error, sizeof(error)) == 0,
		"fd-relative state cleanup failed");
	int descriptor_flags = fcntl(missing->directory_fd, F_GETFD);
	require(descriptor_flags >= 0 &&
			(descriptor_flags & FD_CLOEXEC) != 0,
		"prefix descriptor did not start close-on-exec");
	require(darling_runtime_mode_make_fd_inheritable(
			missing->directory_fd, error, sizeof(error)) == 0,
		"prefix descriptor could not cross launcher/server exec");
	descriptor_flags = fcntl(missing->directory_fd, F_GETFD);
	require(descriptor_flags >= 0 &&
			(descriptor_flags & FD_CLOEXEC) == 0,
		"prefix descriptor remained close-on-exec");
	require(fcntl(missing->directory_fd, F_SETFD,
			descriptor_flags | FD_CLOEXEC) == 0,
		"restore prefix descriptor close-on-exec");

	char unsafe_target[1024];
	char unsafe_link[1024];
	snprintf(unsafe_target, sizeof(unsafe_target), "%s/unsafe-target",
		directory);
	snprintf(unsafe_link, sizeof(unsafe_link), "%s/missing/unsafe",
		directory);
	require(mkdir(unsafe_target, 0700) == 0,
		"create relative-operation symlink target");
	snprintf(sentinel, sizeof(sentinel), "%s/sentinel", unsafe_target);
	write_exact_file(sentinel, "relative-target\n");
	require(symlink(unsafe_target, unsafe_link) == 0,
		"create relative-operation intermediate symlink");
	require(darling_runtime_mode_open_relative_file(missing,
			"unsafe/sentinel", O_RDONLY, 0,
			error, sizeof(error)) < 0,
		"fd-relative open followed an intermediate symlink");
	require_exact_file(sentinel, "relative-target\n");
	require(unlink(unsafe_link) == 0,
		"remove relative-operation symlink");
	require(unlink(sentinel) == 0,
		"remove relative-operation sentinel");
	require(rmdir(unsafe_target) == 0,
		"remove relative-operation target");
	darling_runtime_mode_close_prefix(missing);

	char target[1024];
	char link[1024];
	snprintf(target, sizeof(target), "%s/intermediate-target", directory);
	snprintf(link, sizeof(link), "%s/intermediate-link", directory);
	require(mkdir(target, 0700) == 0, "create intermediate target");
	snprintf(path, sizeof(path), "%s/prefix", target);
	require(mkdir(path, 0700) == 0, "create intermediate target prefix");
	snprintf(sentinel, sizeof(sentinel), "%s/sentinel", path);
	write_exact_file(sentinel, "intermediate-target\n");
	require(symlink(target, link) == 0, "create intermediate symlink");
	snprintf(path, sizeof(path), "%s/prefix", link);
	darling_runtime_prefix rejected =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	require(darling_runtime_mode_open_prefix(path, rejected,
			error, sizeof(error)) != 0,
		"intermediate prefix symlink was accepted before mutation");
	require_exact_file(sentinel, "intermediate-target\n");
	darling_runtime_mode_close_prefix(rejected);
	require(unlink(link) == 0, "remove intermediate link");
	require(unlink(sentinel) == 0, "remove intermediate sentinel");
	snprintf(path, sizeof(path), "%s/prefix", target);
	require(rmdir(path) == 0, "remove intermediate prefix");
	require(rmdir(target) == 0, "remove intermediate target");

	/*
	 * Exercise the production setup implementation, not only marker creation.
	 * The inspected directory is replaced before the first setup mutation.
	 * materialize_prefix() must reject the changed name and setup must not
	 * create even its first Volumes directory in either replacement target.
	 */
	char race_prefix[1024];
	char race_parked[1024];
	char race_target[1024];
	snprintf(race_prefix, sizeof(race_prefix), "%s/race-prefix", directory);
	snprintf(race_parked, sizeof(race_parked), "%s/race-parked", directory);
	snprintf(race_target, sizeof(race_target), "%s/race-target", directory);
	require(mkdir(race_prefix, 0700) == 0, "create race prefix");
	darling_runtime_prefix race =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	require(darling_runtime_mode_open_prefix(race_prefix, race,
			error, sizeof(error)) == 0 &&
			race->anchor_state ==
				DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY &&
			race->directory_fd >= 0,
		"initial race prefix inspection failed");
	require(rename(race_prefix, race_parked) == 0,
		"park inspected race prefix");
	require(mkdir(race_target, 0700) == 0,
		"create race replacement target");
	snprintf(sentinel, sizeof(sentinel), "%s/sentinel", race_target);
	write_exact_file(sentinel, "race-target\n");
	require(symlink(race_target, race_prefix) == 0,
		"replace inspected prefix with symlink");
	require(darling_runtime_mode_setup_prefix(race, "tester",
			getuid(), getgid(), error, sizeof(error)) != 0,
		"production prefix setup followed a post-inspection replacement");
	require_exact_file(sentinel, "race-target\n");
	snprintf(path, sizeof(path), "%s/Volumes", race_target);
	require(access(path, F_OK) != 0 && errno == ENOENT,
		"replacement target received the first setup mutation");
	snprintf(path, sizeof(path), "%s/Volumes", race_parked);
	require(access(path, F_OK) != 0 && errno == ENOENT,
		"renamed inspected directory was mutated after path replacement");
	darling_runtime_mode_close_prefix(race);
	require(unlink(race_prefix) == 0, "remove race replacement symlink");
	require(unlink(sentinel) == 0, "remove race target sentinel");
	require(rmdir(race_target) == 0, "remove unchanged race target");
	require(rmdir(race_parked) == 0, "remove parked race prefix");

	/*
	 * Also race a prefix that was missing at inspection time. An attacker
	 * creating a symlink at the retained parent/leaf boundary must turn the
	 * first mkdirat into a fail-closed result.
	 */
	char absent[1024];
	char absent_target[1024];
	snprintf(absent, sizeof(absent), "%s/absent-race", directory);
	snprintf(absent_target, sizeof(absent_target), "%s/absent-target",
		directory);
	darling_runtime_prefix absent_handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	require(darling_runtime_mode_open_prefix(absent, absent_handle,
			error, sizeof(error)) == 0 &&
			absent_handle->anchor_state ==
				DARLING_RUNTIME_PREFIX_ANCHOR_MISSING,
		"missing race prefix inspection failed");
	require(mkdir(absent_target, 0700) == 0,
		"create absent race target");
	snprintf(sentinel, sizeof(sentinel), "%s/sentinel", absent_target);
	write_exact_file(sentinel, "absent-target\n");
	require(symlink(absent_target, absent) == 0,
		"install missing-prefix race symlink");
	require(darling_runtime_mode_setup_prefix(absent_handle, "tester",
			getuid(), getgid(), error, sizeof(error)) != 0,
		"missing prefix race was accepted by setup");
	require_exact_file(sentinel, "absent-target\n");
	snprintf(path, sizeof(path), "%s/Volumes", absent_target);
	require(access(path, F_OK) != 0 && errno == ENOENT,
		"missing-prefix race target received a setup mutation");
	darling_runtime_mode_close_prefix(absent_handle);
	require(unlink(absent) == 0, "remove missing-prefix race symlink");
	require(unlink(sentinel) == 0, "remove absent target sentinel");
	require(rmdir(absent_target) == 0, "remove absent race target");

	/* The completed normal prefix is removed recursively by the fixture. */
	require_no_temporary_entries(directory);
	snprintf(path, sizeof(path), "%s/missing", directory);
	remove_fixture_tree(path);
	snprintf(path, sizeof(path), "%s/missing.workdir", directory);
	remove_fixture_tree(path);
	remove_fixture_tree(directory);
}

int main(void)
{
	const char* selected = getenv("DARLING_RUNTIME_PREFIX_TEST_CASE");
	if (selected != NULL) {
		if (strcmp(selected, "lock-race") == 0)
			test_lifecycle_lock_race();
		else if (strcmp(selected, "durability") == 0)
			test_staged_tree_durability();
		else if (strcmp(selected, "interruptions") == 0)
			test_mutation_interruption_coverage();
		else if (strcmp(selected, "capability") == 0)
			test_owned_capability_rejects_reopen();
		else if (strcmp(selected, "lifecycle") == 0)
			test_prefix_lifecycle();
		else if (strcmp(selected, "marker") == 0)
			test_prefix_marker();
		else
			require(0, "unknown focused lifecycle test case");
		puts("DARLING_RUNTIME_MODE_CONTRACT_OK");
		return 0;
	}
	test_resolution();
	test_process_boundary();
	test_rootless_credentials();
	test_recovery_matrix();
	test_lifecycle_lock_race();
	test_staged_tree_durability();
	test_mutation_interruption_coverage();
	test_owned_capability_rejects_reopen();
	test_prefix_lifecycle();
	test_prefix_marker();
	puts("DARLING_RUNTIME_MODE_CONTRACT_OK");
	return 0;
}
