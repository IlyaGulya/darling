/*
This file is part of Darling.

Copyright (C) 2015-2018 Lubos Dolezel

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "threads.h"
#include <pthread.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <errno.h>

#include "dthreads.h"

#include <darlingserver/rpc.h>

extern int __mldr_create_rpc_socket(void);
extern void __mldr_close_rpc_socket(int socket);

// The point of this file is build macOS threads on top of native libc's threads,
// otherwise it would not be possible to make native calls from these threads.

static __thread jmp_buf t_jmpbuf;
static __thread void* t_freeaddr;
static __thread size_t t_freesize;
static __thread int t_server_socket = -1;
static __thread darling_thread_create_callbacks_t t_callbacks = NULL;

typedef void (*thread_ep)(void**, int, ...);
struct arg_struct
{
	thread_ep entry_point;
	uintptr_t real_entry_point;
	uintptr_t arg1; // `user_arg` for normal threads; `keventlist` for workqueues
	uintptr_t arg2; // `stack_addr` for normal threads; `flags` for workqueues
	uintptr_t arg3; // `flags` for normal threads; `nkevents` for workqueues
	union {
		void* _backwards_compat; // kept around to avoid modifying assembly
		int port;
	};
	unsigned long pth_obj_size;
	void* pth;
	darling_thread_create_callbacks_t callbacks;
	uintptr_t stack_bottom;
	uintptr_t stack_addr;
	bool is_workqueue;
	// perf #1 (dar-dar6x4-perf-5dq.1): completion handshake futex word. The creating
	// thread FUTEX_WAITs on this instead of busy-spinning sched_yield() until the new
	// thread has checked in with darlingserver; the new thread stores 1 + FUTEX_WAKEs.
	// Appended LAST so the hardcoded i386 byte offsets into &args below stay valid.
	_Atomic int checked_in;
};

// raw futex syscall wrappers (no glibc wrapper exists)
static inline long __dthread_futex(_Atomic int* uaddr, int op, int val) {
	return syscall(SYS_futex, (int*)uaddr, op, val, NULL, NULL, 0);
}

static void* darling_thread_entry(void* p);

#ifndef PTHREAD_STACK_MIN
#	define PTHREAD_STACK_MIN 16384
#endif

#define DEFAULT_DTHREAD_GUARD_SIZE 0x1000

static inline void *align_16(uintptr_t ptr) {
	return (void *) ((uintptr_t) ptr & ~(uintptr_t) 15);
}

static dthread_t dthread_structure_allocate(size_t stack_size, size_t guard_size, void** stack_addr) {
	size_t total_size = guard_size + stack_size + sizeof(struct _dthread);

	// allocate our stack, guard page, and dthread structure
	void* base_addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	// protect our guard page
	mprotect(base_addr, guard_size, PROT_NONE);

	/**
	 * memory layout of newly allocated block:
	 *
	 * [base_addr]         [base_addr + total_size]
	 * --------------------------------------------
	 * | guard page |       stack       | dthread |
	 */

	// stack_addr points to the top of the stack (i.e. the highest address)
	*stack_addr = ((char*)base_addr) + stack_size + guard_size;

	// the dthread sits above the stack
	// (and by "above", i mean the lowest address of the dthread is the highest address of the stack)
	dthread_t dthread = (dthread_t)*stack_addr;
	// zero-out the entrire dthread structure
	memset(dthread, 0, sizeof(struct _dthread));

	return __darling_dthread_initialize(dthread, guard_size, *stack_addr, stack_size, base_addr, total_size);
};

static struct _dthread main_dthread;

int __darling_thread_initialize_main(void* stack_top, size_t stack_size,
		uint32_t mach_thread_self)
{
	memset(&main_dthread, 0, sizeof(main_dthread));
	__darling_dthread_initialize(&main_dthread, 0, stack_top, stack_size, NULL, 0);
	main_dthread.tsd[DTHREAD_TSD_SLOT_MACH_THREAD_SELF] =
		(void*)(uintptr_t)mach_thread_self;
	return __darling_dthread_set_tsd_base(&main_dthread.tsd[0]);
}

void* __darling_thread_create(unsigned long stack_size, unsigned long pth_obj_size,
				void* entry_point, uintptr_t real_entry_point,
				uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
				darling_thread_create_callbacks_t callbacks, void* pth)
{
	struct arg_struct args = {
		.entry_point      = (thread_ep)entry_point,
		.real_entry_point = real_entry_point,
		.arg1             = arg1,
		.arg2             = arg2,
		.arg3             = arg3,
		.port             = 0,
		.pth_obj_size     = pth_obj_size,
		.pth              = NULL, // set later on
		.callbacks        = callbacks,
		.stack_addr       = 0, // set later on
		.is_workqueue     = real_entry_point == 0, // our `workq_kernreturn` sets `real_entry_point` to NULL; `bsdthread_create` actually passes a value
		.checked_in       = 0, // perf #1: cleared before launch, set to 1 by the new thread after checkin
	};
	pthread_attr_t attr;
	pthread_t nativeLibcThread;

	pthread_attr_init(&attr);
	//pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	// pthread_attr_setstacksize(&attr, stack_size);

	// in some cases, we're already given a pthread object, stack, and guard page;
	// in those cases, just use what we're given (it also contains more information)
	//
	// otherwise, allocate them ourselves
	if (pth == NULL || args.is_workqueue) {
		pth = dthread_structure_allocate(stack_size, DEFAULT_DTHREAD_GUARD_SIZE, (void**)&args.stack_addr);
	} else if (!args.is_workqueue) {
		// `arg2` is `stack_addr` for normal threads
		args.stack_addr = arg2;
	}

	args.stack_bottom = args.stack_addr - stack_size;

	// pthread_attr_setstack is buggy. The documentation states we should provide the lowest
	// address of the stack, yet some versions regard it as the highest address instead.
	// Therefore it's better to just make the pthread stack as small as possible and then switch
	// to our own stack instead.
	//pthread_attr_setstack(&attr, ((char*)pth) + pth_obj_size, stack_size - pth_obj_size - 0x1000);

	// std::cout << "Allocated stack at " << pth << ", size " << stack_size << std::endl;

	pthread_attr_setstacksize(&attr, 4096);

	//pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	args.pth = pth;
	pthread_create(&nativeLibcThread, &attr, darling_thread_entry, &args);
	pthread_attr_destroy(&attr);

	// perf #1 (dar-dar6x4-perf-5dq.1): wait for the new thread to finish its darlingserver
	// checkin. The original code was an unbounded `while (args.pth != NULL) sched_yield();`
	// busy-spin: under high thread-creation density (e.g. `make -j` link storms) every
	// in-flight creator burned a whole core, starving the new threads AND the single-threaded
	// darlingserver of the CPU they needed to complete the very checkin being waited on.
	//
	// But the checkin is usually FAST (p50 ~16-64us on a warm server), faster than a futex
	// syscall round-trip, so an immediate FUTEX_WAIT regresses the common case. Use an
	// ADAPTIVE wait: spin a bounded number of times first (wins the fast path with no
	// syscall), then fall back to FUTEX_WAIT so a slow checkin can never monopolise a core.
	// This keeps the fast-path throughput of the spin while removing the pathological
	// starvation of the unbounded spin.
	{
		// ~tuned so the spin phase covers a typical fast checkin but bails out quickly
		// (each iteration is a relaxed load + a sched_yield, i.e. a few hundred ns).
		const int kSpinIters = 4000;
		int spins = 0;
		while (atomic_load_explicit(&args.checked_in, memory_order_acquire) == 0) {
			if (spins < kSpinIters) {
				++spins;
				sched_yield();
				continue;
			}
			// slow checkin: stop burning CPU and block in the kernel.
			long r = __dthread_futex(&args.checked_in, FUTEX_WAIT_PRIVATE, 0);
			// EAGAIN => the value already changed (checkin won the race); EINTR => spurious
			// wakeup. Both just re-check the predicate. Any other error: yield so we can
			// never hang (defensive; should not happen).
			if (r != 0 && errno != EAGAIN && errno != EINTR) {
				sched_yield();
			}
		}
	}

	return pth;
}

static void* darling_thread_entry(void* p)
{
	struct arg_struct* in_args = (struct arg_struct*) p;
	struct arg_struct args;

	memcpy(&args, in_args, sizeof(args));

	dthread_t dthread = args.pth;
	uintptr_t* flags = args.is_workqueue ? &args.arg2 : &args.arg3;

	// create a new dserver RPC socket
	int new_rpc_fd = __mldr_create_rpc_socket();
	if (new_rpc_fd < 0) {
		// we can't do anything if we don't get our own separate connection to darlingserver
		fprintf(stderr, "Failed to create socket\n");
		abort();
	}

	// guard the new RPC FD
	args.callbacks->rpc_guard(new_rpc_fd);

	// the socket is ready; assign it now
	t_server_socket = new_rpc_fd;
	t_callbacks = args.callbacks;

	// libpthread now expects the kernel to set the TSD
	// so, since we're pretending to be the kernel handling threads...
	args.callbacks->thread_set_tsd_base(&dthread->tsd[0], 0);
	*flags |= args.is_workqueue ? DWQ_FLAG_THREAD_TSD_BASE_SET : DTHREAD_START_TSD_BASE_SET;

	// let's check-in with darlingserver on this new thread
	int dummy_stack_variable;
	// the lifetime pipe fd is ignored as the process should already have been registered
	if (dserver_rpc_explicit_checkin(t_server_socket, false, &dummy_stack_variable, -1) < 0) {
		// we can't do ANYTHING if darlingserver doesn't acknowledge us successfully
		abort();
	}

	int thread_self_port = args.callbacks->thread_self_trap();
	dthread->tsd[DTHREAD_TSD_SLOT_MACH_THREAD_SELF] = (void*)(intptr_t)thread_self_port;
	args.port = thread_self_port;

	// perf #1 (dar-dar6x4-perf-5dq.1): signal the creating thread that we've checked in and
	// wake it from its FUTEX_WAIT. This is our LAST access to `in_args` (the creator's stack):
	// once it observes checked_in==1 it may return and reuse that frame, so we must not touch
	// `in_args` afterward. `pth` is nulled too for backwards-compat with anything inspecting it.
	in_args->pth = NULL;
	atomic_store_explicit(&in_args->checked_in, 1, memory_order_release);
	// exactly one waiter (the creating thread) ever waits on this word
	__dthread_futex(&in_args->checked_in, FUTEX_WAKE_PRIVATE, 1);

	if (setjmp(t_jmpbuf))
	{
		// Terminate the Linux thread
		munmap(t_freeaddr, t_freesize);
		pthread_detach(pthread_self());
		return NULL;
	}

	void *stack_ptr = align_16(args.stack_addr);

	// No additional function calls should occur beyond this point. Otherwise, we will risk our
	// registers being call-clobbered. I recommend reading the following doc for more details:
	// https://gcc.gnu.org/onlinedocs/gcc/Local-Register-Variables.html
#if __x86_64__
	register void*     arg1 asm("rdi") = args.pth;
	register int       arg2 asm("esi") = args.port;
	register uintptr_t arg3 asm("rdx") = args.real_entry_point;
	register uintptr_t arg4 asm("rcx") = args.arg1;
	register uintptr_t arg5 asm("r8")  = args.arg2;
	register uintptr_t arg6 asm("r9")  = args.arg3;
#elif __i386__
	uintptr_t arg3 = args.real_entry_point;
#endif

	if (arg3 == 0) {
		arg3 = (long) args.stack_bottom;
	}

#ifdef __x86_64__
	asm volatile(
		// Zero out the frame base register.
		"xorq %%rbp, %%rbp\n"
		// Switch to the new stack.
		"movq %[stack_ptr], %%rsp\n"
		// Push a fake return address.
		"pushq $0\n"
		// Jump to the entry point.
		"jmp *%[entry_point]" ::
		
		// Function arguments
		"r"(arg1),"r"(arg2),"r"(arg3),"r"(arg4),"r"(arg5),"r"(arg6),
		
		[entry_point] "r"(args.entry_point),
		[stack_ptr] "r"(stack_ptr)
	);
#elif defined(__i386__) // args in eax, ebx, ecx, edx, edi, esi
	__asm__ __volatile__ (
		// Zero out the frame base register.
		"xorl %%ebp, %%ebp\n"
		// Switch to the new stack.
		"movl %[stack_ptr], %%esp\n"
		// Make sure stack is 16 aligned (before we push the fake return address)
		"sub $8, %%esp\n"
		// Unlike x86_64, all function arguments must be stored in the stack
		"pushl   16(%[args])\n"		// 6th argument | args.arg3
		"pushl   12(%[args])\n"		// 5th argument | args.arg2
		"pushl   8(%[args])\n"		// 4th argument | args.arg1
		"pushl   %[arg3]\n"			// 3rd argument | args3
		"pushl   20(%[args])\n"		// 2nd argument | args.port
		"pushl   28(%[args])\n"		// 1st argument | args.pth
		// Push a fake return address.
		"pushl $0\n"
		// Jump to the entry point.
		"jmp *%[entry_point]" ::
		
		// Function arguments to push to the stack.
		[args] "r"(&args), [arg3]"r"(arg3),

		[entry_point] "r"(args.entry_point),
		[stack_ptr] "r"(stack_ptr)
	);
#else
	#error Not implemented
	// args.entry_point(args.pth, args.port, args.real_entry_point, args.arg1, args.arg2, args.arg3);
#endif
	__builtin_unreachable();
}

int __darling_thread_terminate(void* stackaddr,
				unsigned long freesize, unsigned long pthobj_size)
{
	int checkout_result = 0;

	if (t_server_socket != -1) {
		checkout_result = dserver_rpc_explicit_checkout(t_server_socket, -1, false);
	} else {
		checkout_result = dserver_rpc_checkout(-1, false);
	}

	if (checkout_result < 0) {
		// failing to check-out is not fatal.
		// it's not ideal, but it's not fatal.
		#define CHECKOUT_FAILURE_MESSAGE "Failed to checkout"
		if (t_server_socket != -1) {
			dserver_rpc_explicit_kprintf(t_server_socket, CHECKOUT_FAILURE_MESSAGE, sizeof(CHECKOUT_FAILURE_MESSAGE) - 1);
		} else {
			dserver_rpc_kprintf(CHECKOUT_FAILURE_MESSAGE, sizeof(CHECKOUT_FAILURE_MESSAGE) - 1);
		}
	}

	// close the RPC FD (if necessary)
	// it should already have been unguarded by our caller
	if (t_server_socket != -1) {
		__mldr_close_rpc_socket(t_server_socket);
	}

	if (getpid() == syscall(SYS_gettid))
	{
		// dispatch_main() calls pthread_exit(NULL) on the main thread,
		// which turns our process into a zombie on Linux.
		// Let's just hang around forever.
		sigset_t mask;
		memset(&mask, 0, sizeof(mask));

		while (1)
			sigsuspend(&mask);
	}

	t_freeaddr = stackaddr;
	t_freesize = freesize;

	longjmp(t_jmpbuf, 1);

	__builtin_unreachable();
}

extern void* __mldr_main_stack_top;

void* __darling_thread_get_stack(void)
{
	return __mldr_main_stack_top;
}

extern int __dserver_main_thread_socket_fd;

int __darling_thread_rpc_socket(void) {
	if (t_server_socket == -1) {
		if (getpid() == syscall(SYS_gettid)) {
			// this is the main thread
			t_server_socket = __dserver_main_thread_socket_fd;
		} else {
			// threads should already have a per-thread socket assigned when they're created
			abort();
		}
	}
	return t_server_socket;
};

void __darling_thread_rpc_socket_refresh(void) {
	int new_rpc_fd = __mldr_create_rpc_socket();
	if (new_rpc_fd < 0) {
		abort();
	}

	t_server_socket = new_rpc_fd;

	// if this is the main thread, also update the socket used by mldr
	if (getpid() == syscall(SYS_gettid)) {
		__dserver_main_thread_socket_fd = t_server_socket;
	}
};
