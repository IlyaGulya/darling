#include "../rootless_shutdown.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum fixture_mode {
	FIXTURE_THREADED_FORK,
	FIXTURE_DOUBLE_FORK,
	FIXTURE_LATE_FORK,
	FIXTURE_ROOT_EXIT,
	FIXTURE_FORK_CHURN,
};

struct fixture_report {
	pid_t observed_child;
};

struct fixture {
	pid_t root;
	pid_t observed_child;
};

static int report_fd = -1;
static pid_t barrier_exit_target = -1;
static int barrier_exit_pidfd = -1;
static unsigned barrier_exit_count;
static unsigned barrier_stop_count;
static unsigned gone_children_reads;
static unsigned interrupt_startup_phase;
static unsigned delegated_capability_fault_phase;
static int delegated_fixture_parent_fd = -1;
static char delegated_fixture_leaf[256];

static void exit_at_stopped_barrier(pid_t pid, int result)
{
	if (pid != barrier_exit_target || result != 1)
		return;
	barrier_stop_count++;
	/* shutdown_rootless_runtime first validates init and session membership.
	 * Trigger only at the subsequent destructive closure acquisition. */
	if (barrier_stop_count < 3 || barrier_exit_count != 0)
		return;
	barrier_exit_count++;
	if (syscall(SYS_pidfd_send_signal, barrier_exit_pidfd, SIGKILL,
			NULL, 0) != 0)
		return;
	struct pollfd wait_for_exit = {
		.fd = barrier_exit_pidfd,
		.events = POLLIN,
	};
	(void)poll(&wait_for_exit, 1, 1000);
}

static void observe_children_read(pid_t pid)
{
	if (pid == barrier_exit_target && barrier_exit_count != 0)
		gone_children_reads++;
}

static void ignore_signal(int signal_number)
{
	(void)signal_number;
}

static void interrupt_startup(unsigned phase)
{
	if (phase == interrupt_startup_phase)
		raise(SIGINT);
}

static void populate_delegated_capability_fixture(unsigned phase,
	int parent_fd, const char* leaf)
{
	if (parent_fd < 0 || leaf == NULL)
		return;
	if (strlen(leaf) >= sizeof(delegated_fixture_leaf))
		return;
	strcpy(delegated_fixture_leaf, leaf);
	if (delegated_capability_fault_phase !=
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MEMBERSHIP)
		return;
	int directory_fd = openat(parent_fd, leaf,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (directory_fd < 0)
		return;
	if (phase == ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_EVENTS) {
		int events = openat(directory_fd, "cgroup.events",
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (events >= 0) {
			(void)write(events, "populated 0\n", 12);
			close(events);
		}
	} else if (phase == ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MEMBERSHIP) {
		(void)unlinkat(directory_fd, "cgroup.events", 0);
	}
	close(directory_fd);
}

static int validate_typed_state(
	const struct rootless_shutdown_closure_capability* closure)
{
	char content[2048];
	if (closure->state_fd < 0 || lseek(closure->state_fd, 0, SEEK_SET) < 0)
		return -1;
	ssize_t length = read(closure->state_fd, content, sizeof(content) - 1);
	if (length <= 0)
		return -1;
	content[length] = '\0';
	return strstr(content, "DARLING_ROOTLESS_SHUTDOWN_SESSION_V2\n") != NULL &&
		strstr(content, "backend=PIDFD_SUBREAPER\n") != NULL &&
		strstr(content, "proc_device=0\n") == NULL &&
		strstr(content, "proc_inode=0\n") == NULL &&
		strstr(content, "anchor_pid=0\n") == NULL &&
		strstr(content, "anchor_start_time=0\n") == NULL ? 0 : -1;
}

static int make_runtime_layout(const char* prefix_path)
{
	char path[512];
	const char* entries[] = { "var", "var/run", "var/tmp", "var/tmp/launchd" };
	for (size_t index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
		int length = snprintf(path, sizeof(path), "%s/%s", prefix_path,
			entries[index]);
		if (length < 0 || (size_t)length >= sizeof(path) ||
			mkdir(path, 0700) != 0)
			return -1;
	}
	return 0;
}

static int remove_runtime_layout(darling_runtime_prefix prefix,
	const char* prefix_path)
{
	darling_runtime_mode_close_prefix(prefix);
	char path[512];
	const char* entries[] = {
		"var/tmp/launchd", "var/tmp", "var/run", "var", "",
	};
	for (size_t index = 0; index < sizeof(entries) / sizeof(entries[0]); ++index) {
		int length = entries[index][0] == '\0'
			? snprintf(path, sizeof(path), "%s", prefix_path)
			: snprintf(path, sizeof(path), "%s/%s", prefix_path,
				entries[index]);
		if (length < 0 || (size_t)length >= sizeof(path) || rmdir(path) != 0)
			return -1;
	}
	return 0;
}

static void publish_child(pid_t child)
{
	struct fixture_report report = { .observed_child = child };
	if (write(report_fd, &report, sizeof(report)) != sizeof(report))
		_exit(90);
}

static void* threaded_fork(void* unused)
{
	(void)unused;
	pid_t child = fork();
	if (child < 0)
		_exit(91);
	if (child == 0)
		for (;;) pause();
	publish_child(child);
	return NULL;
}

static void late_fork_handler(int signal_number)
{
	(void)signal_number;
	signal(SIGTERM, SIG_DFL);
	pid_t child = fork();
	if (child < 0)
		_exit(92);
	if (child == 0)
		for (;;) pause();
	publish_child(child);
}

static void run_fixture_child(enum fixture_mode mode)
{
	if (mode == FIXTURE_THREADED_FORK) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, threaded_fork, NULL) != 0 ||
			pthread_join(thread, NULL) != 0)
			_exit(93);
	} else if (mode == FIXTURE_DOUBLE_FORK || mode == FIXTURE_ROOT_EXIT) {
		int child_report[2];
		if (pipe(child_report) != 0)
			_exit(94);
		pid_t child = fork();
		if (child < 0)
			_exit(95);
		if (child == 0) {
			close(child_report[0]);
			pid_t grandchild = fork();
			if (grandchild < 0)
				_exit(96);
			if (grandchild == 0)
				for (;;) pause();
			if (write(child_report[1], &grandchild, sizeof(grandchild)) !=
					sizeof(grandchild))
				_exit(101);
			_exit(0);
		}
		close(child_report[1]);
		pid_t grandchild = -1;
		if (read(child_report[0], &grandchild, sizeof(grandchild)) !=
				sizeof(grandchild))
			_exit(97);
		close(child_report[0]);
		publish_child(grandchild);
		if (mode == FIXTURE_ROOT_EXIT)
			_exit(0);
	} else if (mode == FIXTURE_LATE_FORK) {
		struct sigaction action = {0};
		action.sa_handler = late_fork_handler;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGTERM, &action, NULL) != 0)
			_exit(98);
		publish_child(0);
	} else {
		for (unsigned index = 0; index < 24; ++index) {
			pid_t child = fork();
			if (child < 0)
				_exit(99);
			if (child == 0)
				for (;;) pause();
		}
		publish_child(0);
	}
	for (;;) pause();
}

static struct fixture spawn_fixture(darling_runtime_prefix prefix,
	enum fixture_mode mode, struct rootless_shutdown_closure_capability* closure)
{
	struct fixture fixture = { .root = -1, .observed_child = -1 };
	char error[512] = {0};
	int reports[2];
	if (rootless_shutdown_prepare_closure(prefix, closure,
			error, sizeof(error)) != 0 ||
		closure->backend != ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER ||
		pipe(reports) != 0)
		return fixture;
	report_fd = reports[1];
	fixture.root = rootless_shutdown_fork_runtime(closure,
		error, sizeof(error));
	if (fixture.root < 0)
		return fixture;
	if (fixture.root == 0) {
		close(reports[0]);
		rootless_shutdown_release_closure(closure);
		run_fixture_child(mode);
		_exit(100);
	}
	if (validate_typed_state(closure) != 0) {
		fixture.root = -1;
		return fixture;
	}
	close(reports[1]);
	struct fixture_report report;
	if (read(reports[0], &report, sizeof(report)) == sizeof(report))
		fixture.observed_child = report.observed_child;
	close(reports[0]);
	return fixture;
}

static int process_alive(pid_t pid)
{
	return pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH);
}

static int run_success_case(darling_runtime_prefix prefix,
	enum fixture_mode mode, const char* name)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	struct fixture fixture = spawn_fixture(prefix, mode, &closure);
	if (fixture.root <= 0)
		return -1;
	if (mode != FIXTURE_ROOT_EXIT &&
		rootless_shutdown_closure_contains(&closure, fixture.root,
			NULL, 0) != 0)
		return -1;
	rootless_shutdown_release_closure(&closure);
	pid_t unrelated = fork();
	if (unrelated < 0)
		return -1;
	if (unrelated == 0)
		for (;;) pause();
	usleep(30000);
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.root, fixture.root, prefix,
		NULL, &result, error, sizeof(error));
	int unrelated_alive = process_alive(unrelated);
	(void)kill(unrelated, SIGKILL);
	(void)waitpid(unrelated, NULL, 0);
	if (rc != 0 || result.phase != ROOTLESS_SHUTDOWN_STOPPED ||
		result.backend != ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER ||
		!unrelated_alive || process_alive(fixture.root) ||
		(fixture.observed_child > 0 && process_alive(fixture.observed_child))) {
		fprintf(stderr, "%s failed rc=%d phase=%d backend=%d error=%s\n",
			name, rc, (int)result.phase, (int)result.backend, error);
		return -1;
	}
	return 0;
}

static int bounded_unwind_case(darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	struct fixture fixture = spawn_fixture(prefix, FIXTURE_FORK_CHURN, &closure);
	if (fixture.root <= 0)
		return -1;
	rootless_shutdown_release_closure(&closure);
	const struct rootless_shutdown_policy policy = {
		.acquisition_timeout_ms = 100,
		.pidfd_budget = 4,
		.term_timeout_ms = 100,
		.kill_timeout_ms = 100,
		.poll_interval_ms = 1,
	};
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.root, fixture.root, prefix,
		&policy, &result, error, sizeof(error));
	if (rc == 0 || !process_alive(fixture.root))
		return -1;
	/* The failed bounded acquisition resumed every identity it stopped. A
	 * normal-budget retry must therefore be able to drain the same tree. */
	rc = shutdown_rootless_runtime(fixture.root, fixture.root, prefix,
		NULL, &result, error, sizeof(error));
	return rc == 0 && result.phase == ROOTLESS_SHUTDOWN_STOPPED ? 0 : -1;
}

static int pid_replacement_case(darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	struct fixture fixture = spawn_fixture(prefix, FIXTURE_THREADED_FORK,
		&closure);
	if (fixture.root <= 0)
		return -1;
	rootless_shutdown_release_closure(&closure);
	rootless_shutdown_test_set_snapshot_replacement(fixture.root, 1);
	const struct rootless_shutdown_policy policy = {
		.acquisition_timeout_ms = 50,
		.term_timeout_ms = 50,
		.kill_timeout_ms = 50,
		.poll_interval_ms = 1,
	};
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.root, fixture.root, prefix,
		&policy, &result, error, sizeof(error));
	rootless_shutdown_test_set_snapshot_replacement(-1, 0);
	if (rc == 0 || !process_alive(fixture.root))
		return -1;
	rc = shutdown_rootless_runtime(fixture.root, fixture.root, prefix,
		NULL, &result, error, sizeof(error));
	return rc == 0 && result.phase == ROOTLESS_SHUTDOWN_STOPPED ? 0 : -1;
}

static int gone_barrier_case(darling_runtime_prefix prefix)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	struct fixture fixture = spawn_fixture(prefix, FIXTURE_THREADED_FORK,
		&closure);
	if (fixture.root <= 0 || fixture.observed_child <= 0)
		return -1;
	rootless_shutdown_release_closure(&closure);
	barrier_exit_target = fixture.root;
	barrier_exit_pidfd = (int)syscall(SYS_pidfd_open, fixture.root, 0);
	barrier_exit_count = 0;
	barrier_stop_count = 0;
	gone_children_reads = 0;
	if (barrier_exit_pidfd < 0)
		return -1;
	rootless_shutdown_test_set_proc_barrier_checkpoint(
		exit_at_stopped_barrier);
	rootless_shutdown_test_set_proc_children_checkpoint(
		observe_children_read);
	struct rootless_shutdown_result result;
	char error[512] = {0};
	int rc = shutdown_rootless_runtime(fixture.root, fixture.root, prefix,
		NULL, &result, error, sizeof(error));
	rootless_shutdown_test_set_proc_barrier_checkpoint(NULL);
	rootless_shutdown_test_set_proc_children_checkpoint(NULL);
	close(barrier_exit_pidfd);
	barrier_exit_pidfd = -1;
	barrier_exit_target = -1;
	if (rc != 0 || result.phase != ROOTLESS_SHUTDOWN_STOPPED ||
		barrier_exit_count != 1 || gone_children_reads != 0 ||
		process_alive(fixture.root) || process_alive(fixture.observed_child)) {
		fprintf(stderr, "gone barrier failed rc=%d phase=%d barrier=%u "
			"children=%u root=%d child=%d error=%s\n", rc,
			(int)result.phase, barrier_exit_count, gone_children_reads,
			process_alive(fixture.root), process_alive(fixture.observed_child),
			error);
		return -1;
	}
	return 0;
}

static int close_range_fallback_case(void)
{
	pid_t child = fork();
	if (child < 0)
		return -1;
	if (child == 0) {
		int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (probe < 3)
			_exit(2);
		rootless_shutdown_test_set_close_range_error(EPERM);
		rootless_shutdown_test_close_controller_descriptors();
		int rejected = fcntl(probe, F_GETFD) < 0 && errno == EBADF;
		_exit(rejected ? 0 : 3);
	}
	int status = 0;
	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int session_state_absent(darling_runtime_prefix prefix)
{
	struct stat state;
	if (fstatat(prefix->directory_fd,
			ROOTLESS_SHUTDOWN_SESSION_STATE_NAME, &state,
			AT_SYMLINK_NOFOLLOW) == 0)
		return 0;
	return errno == ENOENT;
}

static int startup_rollback_case(darling_runtime_prefix prefix,
	unsigned phase, int interrupt)
{
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	rootless_shutdown_test_force_backend(
		ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER);
	if (rootless_shutdown_prepare_closure(prefix, &closure,
			error, sizeof(error)) != 0)
		return -1;
	if (interrupt) {
		interrupt_startup_phase = phase;
		rootless_shutdown_test_set_startup_checkpoint(interrupt_startup);
	} else {
		rootless_shutdown_test_set_startup_error(phase, EIO);
	}
	pid_t runtime = rootless_shutdown_fork_runtime(&closure,
		error, sizeof(error));
	if (runtime == 0)
		_exit(97);
	rootless_shutdown_test_set_startup_checkpoint(NULL);
	rootless_shutdown_test_set_startup_error(0, 0);
	interrupt_startup_phase = 0;
	int anchor_drained = 1;
	if (closure.anchor_pidfd >= 0) {
		struct pollfd exited = {
			.fd = closure.anchor_pidfd,
			.events = POLLIN,
		};
		anchor_drained = poll(&exited, 1, 0) == 1 &&
			(exited.revents & POLLIN) != 0;
	}
	int state_removed = session_state_absent(prefix);
	rootless_shutdown_release_closure(&closure);
	int expected = interrupt ? -EINTR : -EIO;
	if (runtime != expected || !anchor_drained || !state_removed) {
		fprintf(stderr, "startup rollback failed phase=%u interrupt=%d "
			"rc=%d anchor=%d state=%d error=%s\n", phase, interrupt,
			(int)runtime, anchor_drained, state_removed, error);
		return -1;
	}
	return 0;
}

static int startup_rollback_matrix(darling_runtime_prefix prefix)
{
	struct sigaction action = {0};
	struct sigaction previous;
	action.sa_handler = ignore_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &previous) != 0)
		return -1;
	for (unsigned phase = ROOTLESS_SHUTDOWN_TEST_STARTUP_CONTROLLER_READY;
		phase <= ROOTLESS_SHUTDOWN_TEST_STARTUP_RUNTIME_REPORTED; ++phase) {
		if (startup_rollback_case(prefix, phase, 0) != 0 ||
			startup_rollback_case(prefix, phase, 1) != 0) {
			(void)sigaction(SIGINT, &previous, NULL);
			return -1;
		}
	}
	return sigaction(SIGINT, &previous, NULL) == 0 ? 0 : -1;
}

static int delegated_capability_fallback_case(
	darling_runtime_prefix prefix, unsigned phase)
{
	char parent_path[] = "/tmp/darling-cgroup-capability.XXXXXX";
	if (mkdtemp(parent_path) == NULL)
		return -1;
	delegated_fixture_parent_fd = open(parent_path,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (delegated_fixture_parent_fd < 0)
		return -1;
	delegated_fixture_leaf[0] = '\0';
	delegated_capability_fault_phase = phase;
	rootless_shutdown_test_clear_forced_backend();
	rootless_shutdown_test_set_delegated_parent_override(
		delegated_fixture_parent_fd, "/test-delegated");
	rootless_shutdown_test_set_cgroup_create_checkpoint(
		populate_delegated_capability_fixture);
	rootless_shutdown_test_set_cgroup_create_error(phase, EACCES);
	struct rootless_shutdown_closure_capability closure =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	char error[512] = {0};
	int rc = rootless_shutdown_prepare_closure(prefix, &closure,
		error, sizeof(error));
	rootless_shutdown_test_set_cgroup_create_error(0, 0);
	rootless_shutdown_test_set_cgroup_create_checkpoint(NULL);
	rootless_shutdown_test_set_delegated_parent_override(-1, NULL);
	struct stat leftover;
	int leaf_absent = delegated_fixture_leaf[0] != '\0' &&
		fstatat(delegated_fixture_parent_fd, delegated_fixture_leaf,
			&leftover, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
	int state_removed = session_state_absent(prefix);
	int backend_ok = rc == 0 &&
		closure.backend == ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER;
	rootless_shutdown_release_closure(&closure);
	close(delegated_fixture_parent_fd);
	delegated_fixture_parent_fd = -1;
	int removed = rmdir(parent_path) == 0;
	if (!backend_ok || !leaf_absent || !state_removed || !removed) {
		fprintf(stderr, "delegated capability fallback failed phase=%u "
			"rc=%d backend=%d leaf=%d state=%d removed=%d error=%s\n",
			phase, rc, (int)closure.backend, leaf_absent, state_removed,
			removed, error);
		return -1;
	}
	return 0;
}

int main(void)
{
	darling_runtime_prefix prefix = DARLING_RUNTIME_PREFIX_INITIALIZER;
	char prefix_path[] = "/tmp/darling-rootless-backend.XXXXXX";
	char error[512] = {0};
	if (mkdtemp(prefix_path) == NULL || make_runtime_layout(prefix_path) != 0 ||
		darling_runtime_mode_open_prefix(prefix_path, prefix,
			error, sizeof(error)) != 0)
		return 1;
	struct rootless_shutdown_closure_capability probe =
		ROOTLESS_SHUTDOWN_CLOSURE_CAPABILITY_INITIALIZER;
	rootless_shutdown_test_clear_forced_backend();
	rootless_shutdown_test_set_parent_lookup_error(EACCES);
	if (rootless_shutdown_prepare_closure(prefix, &probe,
			error, sizeof(error)) != 0 ||
		probe.backend != ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER)
		return 8;
	rootless_shutdown_release_closure(&probe);
	rootless_shutdown_test_set_parent_lookup_error(0);
	rootless_shutdown_test_force_backend(
		ROOTLESS_SHUTDOWN_BACKEND_UNSUPPORTED);
	struct stat forbidden_state;
	if (rootless_shutdown_prepare_closure(prefix, &probe,
			error, sizeof(error)) == 0 ||
		fstatat(prefix->directory_fd, ROOTLESS_SHUTDOWN_SESSION_STATE_NAME,
			&forbidden_state, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
		return 9;
	rootless_shutdown_test_force_backend(
		ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER);
	if (close_range_fallback_case() != 0)
		return 11;
	if (startup_rollback_matrix(prefix) != 0)
		return 12;
	if (delegated_capability_fallback_case(prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_EVENTS) != 0)
		return 14;
	if (delegated_capability_fallback_case(prefix,
			ROOTLESS_SHUTDOWN_TEST_CGROUP_BEFORE_MEMBERSHIP) != 0)
		return 15;
	rootless_shutdown_test_force_backend(
		ROOTLESS_SHUTDOWN_BACKEND_PIDFD_SUBREAPER);
	if (run_success_case(prefix, FIXTURE_THREADED_FORK, "threaded-fork") != 0)
		return 2;
	if (run_success_case(prefix, FIXTURE_DOUBLE_FORK, "double-fork") != 0)
		return 3;
	if (run_success_case(prefix, FIXTURE_LATE_FORK, "late-fork") != 0)
		return 4;
	if (run_success_case(prefix, FIXTURE_ROOT_EXIT, "root-exit") != 0)
		return 5;
	if (pid_replacement_case(prefix) != 0)
		return 6;
	if (gone_barrier_case(prefix) != 0)
		return 13;
	if (bounded_unwind_case(prefix) != 0)
		return 7;
	if (remove_runtime_layout(prefix, prefix_path) != 0)
		return 10;
	printf("ROOTLESS_SHUTDOWN_BACKEND_OK backend=PIDFD_SUBREAPER "
		"nondelegated=AUTO_FALLBACK unsupported=FAIL_CLOSED "
		"threaded_fork=PASS double_fork=PASS late_fork=PASS "
		"root_exit=PASS pid_reuse=FAIL_CLOSED gone=NO_TRAVERSAL "
		"startup_rollback=8/8 close_range_eprem=FALLBACK "
		"delegated_capability=ROLLBACK_TO_SUBREAPER "
		"churn=BOUNDED fd_budget=BOUNDED unrelated=ALIVE unwind=SAFE\n");
	return 0;
}
