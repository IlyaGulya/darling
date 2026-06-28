// Regression test for perf #1 (dar-dar6x4-perf-5dq.1):
// __darling_thread_create() must WAIT for the new thread's darlingserver checkin via
// FUTEX_WAIT, NOT a sched_yield() busy-spin. The old spin burned a whole core per
// in-flight thread creation; under `make -j` link storms that starved the very threads
// (and the single-threaded darlingserver) it was waiting on.
//
// We can't link mldr's full thread machinery in a host test, so we reproduce the exact
// creator/child handshake both ways and measure the CREATOR's own CPU time while it waits
// out a deliberately-delayed checkin:
//
//   GREEN (futex): creator blocks in the kernel  -> ~0 CPU during the wait.
//   RED   (spin):  creator burns a core          -> CPU ~= wall time during the wait.
//
// This is a HOST test (plain glibc, no Darling runtime needed); see run-thread-create-
// checkin-wait.sh. Exit 0 = PASS (futex path); the runner also proves the RED case fails.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/futex.h>

static _Atomic int checked_in = 0;
static int use_spin = 0;

static long ms_thread_cpu(void) {
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static long futex(_Atomic int* uaddr, int op, int val) {
	return syscall(SYS_futex, (int*)uaddr, op, val, NULL, NULL, 0);
}

// emulates darling_thread_entry: slow "checkin", then signal + wake the creator
static void* child(void* p) {
	(void)p;
	struct timespec d = { 0, 300 * 1000 * 1000 }; // 300ms artificial checkin latency
	nanosleep(&d, NULL);
	atomic_store_explicit(&checked_in, 1, memory_order_release);
	if (!use_spin)
		futex(&checked_in, FUTEX_WAKE_PRIVATE, 1);
	return NULL;
}

int main(int argc, char** argv) {
	if (argc > 1 && strcmp(argv[1], "spin") == 0)
		use_spin = 1;

	pthread_t th;
	long cpu0 = ms_thread_cpu();
	struct timespec w0, w1;
	clock_gettime(CLOCK_MONOTONIC, &w0);

	if (pthread_create(&th, NULL, child, NULL) != 0) {
		perror("pthread_create");
		return 2;
	}

	// THE WAIT under test -- exactly the two variants from __darling_thread_create()
	if (use_spin) {
		// RED: the original unbounded busy-spin.
		while (atomic_load_explicit(&checked_in, memory_order_acquire) == 0)
			sched_yield();
	} else {
		// GREEN: the production ADAPTIVE wait -- bounded spin (fast path) then FUTEX_WAIT
		// so a slow checkin can never monopolise a core.
		const int kSpinIters = 4000;
		int spins = 0;
		while (atomic_load_explicit(&checked_in, memory_order_acquire) == 0) {
			if (spins < kSpinIters) {
				++spins;
				sched_yield();
				continue;
			}
			long r = futex(&checked_in, FUTEX_WAIT_PRIVATE, 0);
			if (r != 0 && errno != EAGAIN && errno != EINTR)
				sched_yield();
		}
	}

	long cpu1 = ms_thread_cpu();
	clock_gettime(CLOCK_MONOTONIC, &w1);
	pthread_join(th, NULL);

	long cpu_ms = cpu1 - cpu0;
	long wall_ms = (w1.tv_sec - w0.tv_sec) * 1000L + (w1.tv_nsec - w0.tv_nsec) / 1000000L;
	printf("variant=%s creator_cpu=%ldms wall=%ldms\n", use_spin ? "spin" : "futex", cpu_ms, wall_ms);

	if (wall_ms < 250) {
		fprintf(stderr, "FAIL: wait too short (%ldms) -- handshake did not block\n", wall_ms);
		return 2;
	}

	const long CPU_LIMIT_MS = 30;
	if (cpu_ms > CPU_LIMIT_MS) {
		fprintf(stderr, "FAIL: creator burned %ldms CPU while waiting (limit %ldms) -- busy-spin\n", cpu_ms, CPU_LIMIT_MS);
		return 1;
	}
	printf("PASS: creator slept through the checkin (%ldms CPU <= %ldms)\n", cpu_ms, CPU_LIMIT_MS);
	return 0;
}
