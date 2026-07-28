#define _GNU_SOURCE 1

#include "../runtime_credentials.h"
#include "../runtime_mode.h"
#include "../runtime_mode_prefix.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
		strstr(name, ".tmp.") != NULL)
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

static void test_prefix_marker(void)
{
	char root[] = "/tmp/darling-runtime-mode-test.XXXXXX";
	char* directory = mkdtemp(root);
	require(directory != NULL, "mkdtemp failed");
	char error[256] = {0};
	char path[2048];
	char sentinel[4096];

	struct darling_runtime_prefix missing =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	snprintf(path, sizeof(path), "%s/missing", directory);
	require(darling_runtime_mode_open_prefix(path, &missing,
			error, sizeof(error)) == 0 &&
			!missing.existed && missing.empty &&
			missing.directory_fd < 0 && missing.parent_fd >= 0,
		"missing prefix did not retain its verified parent");
	require(darling_runtime_mode_setup_prefix(&missing, "tester",
			getuid(), getgid(), error, sizeof(error)) == 0, error);
	require(missing.directory_fd >= 0,
		"safe setup did not retain the created prefix fd");
	require(darling_runtime_mode_initialize_prefix_marker(&missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_validate_prefix_marker(&missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_prefix_needs_initialization(&missing,
			error, sizeof(error)) == 0,
		"fd-relative setup did not create a complete prefix");
	require(darling_runtime_mode_validate_prefix_marker(&missing,
			DARLING_RUNTIME_MODE_PRIVILEGED_OVERLAY,
			error, sizeof(error)) != 0,
		"prefix mode mismatch was accepted");
	require(unlinkat(missing.directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME, 0) == 0,
		"remove regular runtime mode marker");
	require(symlinkat("private/etc/passwd", missing.directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME) == 0,
		"create marker symlink fixture");
	require(darling_runtime_mode_validate_prefix_marker(&missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) != 0,
		"runtime mode marker symlink was accepted");
	require(unlinkat(missing.directory_fd,
			DARLING_RUNTIME_MODE_MARKER_NAME, 0) == 0,
		"remove marker symlink fixture");
	require(darling_runtime_mode_initialize_prefix_marker(&missing,
			DARLING_RUNTIME_MODE_ROOTLESS_EUNION,
			error, sizeof(error)) == 0, error);
	require(darling_runtime_mode_prepare_workdir(&missing,
			error, sizeof(error)) == 0 &&
			missing.workdir_fd >= 0,
		"fd-relative workdir preparation failed");
	struct stat workdir_status;
	require(fstat(missing.workdir_fd, &workdir_status) == 0 &&
			S_ISDIR(workdir_status.st_mode),
		"retained workdir descriptor is not a directory");
	require(darling_runtime_mode_verify_prefix_name(&missing,
			error, sizeof(error)) == 0,
		"retained prefix name verification failed");
	require(darling_runtime_mode_write_relative_atomic(&missing,
			".init.pid", "123\n", 0600,
			error, sizeof(error)) == 0, error);
	struct stat state_status;
	require(darling_runtime_mode_stat_relative(&missing,
			".init.pid", &state_status,
			error, sizeof(error)) == 0 &&
			S_ISREG(state_status.st_mode) &&
			state_status.st_size == 4,
		"fd-relative state publication failed");
	require(darling_runtime_mode_unlink_relative(&missing,
			".init.pid", 0, false,
			error, sizeof(error)) == 0,
		"fd-relative state cleanup failed");
	int descriptor_flags = fcntl(missing.directory_fd, F_GETFD);
	require(descriptor_flags >= 0 &&
			(descriptor_flags & FD_CLOEXEC) != 0,
		"prefix descriptor did not start close-on-exec");
	require(darling_runtime_mode_make_fd_inheritable(
			missing.directory_fd, error, sizeof(error)) == 0,
		"prefix descriptor could not cross launcher/server exec");
	descriptor_flags = fcntl(missing.directory_fd, F_GETFD);
	require(descriptor_flags >= 0 &&
			(descriptor_flags & FD_CLOEXEC) == 0,
		"prefix descriptor remained close-on-exec");
	require(fcntl(missing.directory_fd, F_SETFD,
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
	require(darling_runtime_mode_open_relative_file(&missing,
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
	darling_runtime_mode_close_prefix(&missing);

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
	struct darling_runtime_prefix rejected =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	require(darling_runtime_mode_open_prefix(path, &rejected,
			error, sizeof(error)) != 0,
		"intermediate prefix symlink was accepted before mutation");
	require_exact_file(sentinel, "intermediate-target\n");
	darling_runtime_mode_close_prefix(&rejected);
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
	struct darling_runtime_prefix race =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	require(darling_runtime_mode_open_prefix(race_prefix, &race,
			error, sizeof(error)) == 0 &&
			race.existed && race.empty && race.directory_fd >= 0,
		"initial race prefix inspection failed");
	require(rename(race_prefix, race_parked) == 0,
		"park inspected race prefix");
	require(mkdir(race_target, 0700) == 0,
		"create race replacement target");
	snprintf(sentinel, sizeof(sentinel), "%s/sentinel", race_target);
	write_exact_file(sentinel, "race-target\n");
	require(symlink(race_target, race_prefix) == 0,
		"replace inspected prefix with symlink");
	require(darling_runtime_mode_setup_prefix(&race, "tester",
			getuid(), getgid(), error, sizeof(error)) != 0,
		"production prefix setup followed a post-inspection replacement");
	require_exact_file(sentinel, "race-target\n");
	snprintf(path, sizeof(path), "%s/Volumes", race_target);
	require(access(path, F_OK) != 0 && errno == ENOENT,
		"replacement target received the first setup mutation");
	snprintf(path, sizeof(path), "%s/Volumes", race_parked);
	require(access(path, F_OK) != 0 && errno == ENOENT,
		"renamed inspected directory was mutated after path replacement");
	darling_runtime_mode_close_prefix(&race);
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
	struct darling_runtime_prefix absent_handle =
		DARLING_RUNTIME_PREFIX_INITIALIZER;
	require(darling_runtime_mode_open_prefix(absent, &absent_handle,
			error, sizeof(error)) == 0 && !absent_handle.existed,
		"missing race prefix inspection failed");
	require(mkdir(absent_target, 0700) == 0,
		"create absent race target");
	snprintf(sentinel, sizeof(sentinel), "%s/sentinel", absent_target);
	write_exact_file(sentinel, "absent-target\n");
	require(symlink(absent_target, absent) == 0,
		"install missing-prefix race symlink");
	require(darling_runtime_mode_setup_prefix(&absent_handle, "tester",
			getuid(), getgid(), error, sizeof(error)) != 0,
		"missing prefix race was accepted by setup");
	require_exact_file(sentinel, "absent-target\n");
	snprintf(path, sizeof(path), "%s/Volumes", absent_target);
	require(access(path, F_OK) != 0 && errno == ENOENT,
		"missing-prefix race target received a setup mutation");
	darling_runtime_mode_close_prefix(&absent_handle);
	require(unlink(absent) == 0, "remove missing-prefix race symlink");
	require(unlink(sentinel) == 0, "remove absent target sentinel");
	require(rmdir(absent_target) == 0, "remove absent race target");

	/* The completed normal prefix is removed recursively by the fixture. */
	require_no_temporary_entries(directory);
	snprintf(path, sizeof(path), "%s/missing", directory);
	remove_fixture_tree(path);
	snprintf(path, sizeof(path), "%s/missing.workdir", directory);
	remove_fixture_tree(path);
	require(rmdir(directory) == 0, "remove runtime mode fixture");
}

int main(void)
{
	test_resolution();
	test_process_boundary();
	test_rootless_credentials();
	test_prefix_marker();
	puts("DARLING_RUNTIME_MODE_CONTRACT_OK");
	return 0;
}
