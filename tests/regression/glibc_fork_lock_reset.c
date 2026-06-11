// Regression test for dar-gwn.5: the glibc loader/stack-lock reset that mldr runs
// in a raw-fork child (src/startup/mldr/glibc_fork_reset.c).
//
// Darling forks with a raw clone/__NR_fork (sys_fork), which is _Fork-level: it
// resets only the robust-mutex list and skips glibc's __libc_fork child path. That
// path unconditionally resets GL(_dl_load_lock), GL(_dl_load_tls_lock) and the
// stack-cache lock/lists. Without it, a raw-fork child inherits whatever those
// locks held in the parent and deadlocks the first time it dlopen()s or
// pthread_create()s.
//
// This is a HOST test: it runs against the same host glibc that mldr resets at
// runtime, so it needs no Darling build and finishes in a couple of seconds. Each
// case deterministically reproduces the inherited-held-lock deadlock:
//   1. detect the lock's address (single-threaded, via the production code),
//   2. write the exact "held by a now-gone thread" bytes into it,
//   3. raw-fork (syscall, no atfork handlers - exactly like Darling),
//   4. in the child: optionally run the reset, then perform the operation that
//      takes that lock; a watchdog alarm turns a deadlock into a killed child.
// The parent restores the lock immediately after forking so it stays healthy for
// the next case (the child already has its own held copy via COW).
//
// Exit 0 == every case deadlocks without the reset and passes with it.
#define _GNU_SOURCE
#include "glibc_fork_reset.h"
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// Test-only accessors exported by glibc_fork_reset.c under GLIBC_FORK_RESET_TEST_HOOKS.
void* __mldr_test_stack_cache_lock(void);
void* __mldr_test_load_lock(void);
void* __mldr_test_load_tls_lock(void);

enum { OP_PTHREAD, OP_DLOPEN_LIBM, OP_DLOPEN_TLS };

#define MUTEX_SIZE_BYTES 40

static int raw_fork(void) { return (int) syscall(SYS_fork); }

static void* thr(void* a) { (void) a; return NULL; }

static void child_op(int op) {
	if (op == OP_PTHREAD) {
		pthread_t t;
		pthread_create(&t, NULL, thr, NULL);
		pthread_join(t, NULL);
	} else if (op == OP_DLOPEN_LIBM) {
		void* h = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
		if (h) dlclose(h);
	} else {
		void* h = dlopen("./tls_mod.so", RTLD_NOW | RTLD_LOCAL);
		if (h) dlclose(h);
	}
}

// Mark an LLL int lock (e.g. _dl_stack_cache_lock) as contended/held.
static void hold_int_lock(void* p) { *(volatile int*) p = 2; }

// Mark a recursive pthread mutex as locked by some other (now-gone) thread.
// The owner must NOT equal the forking thread's cached tid: a raw __NR_fork does
// not refresh THREAD_SELF->tid, so the child still believes it is the parent's
// main thread. If we set __owner to that tid, the recursive mutex would just
// re-enter instead of blocking. A foreign owner reproduces the real bug, where
// the lock was held by a *different* thread that no longer exists in the child.
static void hold_recursive_mutex(void* m) {
	unsigned char* p = m;
	int foreign_owner = (int) syscall(SYS_gettid) + 1; // some other, non-self tid
	*(volatile int*)(p + 0)  = 2;             // __lock = locked (contended)
	*(volatile int*)(p + 4)  = 1;             // __count
	*(volatile int*)(p + 8)  = foreign_owner; // __owner
	*(volatile int*)(p + 12) = 1;             // __nusers
	// __kind (+16) stays PTHREAD_MUTEX_RECURSIVE_NP
}

static int run_case(const char* name, void* lock, int recursive, int op, int do_reset) {
	if (!lock) {
		printf("[%-22s] FAIL - lock not detected\n", name);
		return 0;
	}
	size_t n = recursive ? MUTEX_SIZE_BYTES : 4;
	unsigned char saved[MUTEX_SIZE_BYTES];
	memcpy(saved, lock, n);

	if (recursive) hold_recursive_mutex(lock);
	else           hold_int_lock(lock);

	int pid = raw_fork();
	if (pid == 0) {
		alarm(3); // watchdog: a deadlocked child is killed by SIGALRM
		if (do_reset)
			__mldr_glibc_fork_reset_child();
		child_op(op);
		_exit(0); // reached only if the operation did NOT deadlock
	}

	memcpy(lock, saved, n); // heal the parent

	int st = 0;
	waitpid(pid, &st, 0);
	int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	printf("[%-22s] reset=%d -> %-18s (%s %d)\n",
		name, do_reset, ok ? "PASS (no deadlock)" : "HANG/DEADLOCK",
		WIFSIGNALED(st) ? "killed by sig" : "exit",
		WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
	return ok;
}

int main(void) {
	__mldr_glibc_fork_reset_detect();
	void* sc  = __mldr_test_stack_cache_lock();
	void* ll  = __mldr_test_load_lock();
	void* ltl = __mldr_test_load_tls_lock();
	printf("detected: stack_cache_lock=%p  load_lock=%p  load_tls_lock=%p\n\n", sc, ll, ltl);
	if (!sc || !ll || !ltl) {
		printf("FAIL: detection did not locate every lock on this glibc\n");
		return 1;
	}

	printf("=== sanity: with reset disabled, every case must deadlock ===\n");
	int n0 = run_case("stack_cache/pthread",  sc,  0, OP_PTHREAD,     0);
	int n1 = run_case("load_lock/dlopen",     ll,  1, OP_DLOPEN_LIBM, 0);
	int n2 = run_case("load_tls_lock/dlopen", ltl, 1, OP_DLOPEN_TLS,  0);

	printf("\n=== with reset enabled, every case must pass ===\n");
	int a = run_case("stack_cache/pthread",  sc,  0, OP_PTHREAD,     1);
	int b = run_case("load_lock/dlopen",     ll,  1, OP_DLOPEN_LIBM, 1);
	int c = run_case("load_tls_lock/dlopen", ltl, 1, OP_DLOPEN_TLS,  1);

	// The test is only meaningful if the bug reproduces without the reset.
	int reproduced = (n0 == 0) && (n1 == 0) && (n2 == 0);
	int fixed = (a == 1) && (b == 1) && (c == 1);
	int pass = reproduced && fixed;
	printf("\nRESULT: %s\n", pass ? "PASS"
		: !reproduced ? "INCONCLUSIVE (bug did not reproduce without reset)"
		: "FAIL (a case still deadlocks with reset)");
	return pass ? 0 : 1;
}
