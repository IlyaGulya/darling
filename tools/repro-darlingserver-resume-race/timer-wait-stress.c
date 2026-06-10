#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct worker_args {
	unsigned int iterations;
	mach_timespec_t timeout;
};

static void* worker(void* context) {
	const struct worker_args* args = context;
	semaphore_t semaphore = MACH_PORT_NULL;
	kern_return_t result = semaphore_create(mach_task_self(), &semaphore, SYNC_POLICY_FIFO, 0);
	if (result != KERN_SUCCESS) {
		fprintf(stderr, "semaphore_create failed: %d\n", result);
		return (void*)1;
	}

	for (unsigned int i = 0; i < args->iterations; ++i) {
		result = semaphore_timedwait(semaphore, args->timeout);
		if (result != KERN_OPERATION_TIMED_OUT) {
			fprintf(stderr, "semaphore_timedwait returned: %d\n", result);
			semaphore_destroy(mach_task_self(), semaphore);
			return (void*)1;
		}
	}

	semaphore_destroy(mach_task_self(), semaphore);
	return NULL;
}

int main(int argc, char** argv) {
	const unsigned int thread_count = argc > 1 ? strtoul(argv[1], NULL, 10) : 32;
	const unsigned int iterations = argc > 2 ? strtoul(argv[2], NULL, 10) : 1000;
	const unsigned int delay_ns = argc > 3 ? strtoul(argv[3], NULL, 10) : 500000;

	if (thread_count == 0 || iterations == 0 || delay_ns >= 1000000000) {
		fprintf(stderr, "invalid arguments\n");
		return 2;
	}

	struct worker_args args = {
		.iterations = iterations,
		.timeout = {
			.tv_sec = 0,
			.tv_nsec = delay_ns,
		},
	};
	pthread_t* threads = calloc(thread_count, sizeof(*threads));
	if (threads == NULL) {
		perror("calloc");
		return 2;
	}

	for (unsigned int i = 0; i < thread_count; ++i) {
		int result = pthread_create(&threads[i], NULL, worker, &args);
		if (result != 0) {
			fprintf(stderr, "pthread_create failed: %d\n", result);
			return 2;
		}
	}

	for (unsigned int i = 0; i < thread_count; ++i) {
		void* result = NULL;
		pthread_join(threads[i], &result);
		if (result != NULL) {
			return 1;
		}
	}

	puts("done");
	free(threads);
	return 0;
}
