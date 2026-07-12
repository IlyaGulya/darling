/*
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling contributors
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "dthreads.h"

#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

dthread_t __darling_dthread_initialize(dthread_t dthread, size_t guard_size,
		void* stack_addr, size_t stack_size, void* base_addr, size_t total_size)
{
	// The kernel normally supplies this structure for a Darwin thread. mldr
	// supplies the same minimum representation before guest libpthread starts.
	dthread->sig = (uintptr_t)dthread;

	dthread->tsd[DTHREAD_TSD_SLOT_PTHREAD_SELF] = dthread;
	dthread->tsd[DTHREAD_TSD_SLOT_ERRNO] = &dthread->err_no;
	dthread->tsd[DTHREAD_TSD_SLOT_PTHREAD_QOS_CLASS] =
		(void*)(uintptr_t)(DTHREAD_DEFAULT_PRIORITY);
	dthread->tsd[DTHREAD_TSD_SLOT_PTR_MUNGE] = 0;
	dthread->tl_has_custom_stack = 0;
	dthread->lock = (darwin_os_unfair_lock){0};

	dthread->stackaddr = stack_addr;
	dthread->stackbottom = (char*)stack_addr - stack_size;
	dthread->freeaddr = base_addr;
	dthread->freesize = total_size;
	dthread->guardsize = guard_size;

	dthread->cancel_state = DTHREAD_CANCEL_ENABLE | DTHREAD_CANCEL_DEFERRED;
	dthread->tl_joinable = 1;
	dthread->inherit = DTHREAD_INHERIT_SCHED;
	dthread->tl_policy = DARWIN_POLICY_TIMESHARE;

	return dthread;
}

int __darling_dthread_set_tsd_base(void* tsd_base)
{
#if defined(__x86_64__)
	// Linux reserves FS for native libc TLS. Darling's x86_64 guest ABI uses GS.
	if (syscall(SYS_arch_prctl, 0x1001, tsd_base) == -1) {
		return -errno;
	}
	return 0;
#elif defined(__i386__)
	struct user_desc {
		unsigned int entry_number;
		unsigned long base_addr;
		unsigned int limit;
		unsigned int seg_32bit:1;
		unsigned int contents:2;
		unsigned int read_exec_only:1;
		unsigned int limit_in_pages:1;
		unsigned int seg_not_present:1;
		unsigned int useable:1;
	};
	static int entry_number = -1;
	struct user_desc desc = {
		.entry_number = entry_number,
		.base_addr = (unsigned long)tsd_base,
		.limit = 4096,
		.seg_32bit = 1,
		.contents = 0,
		.read_exec_only = 0,
		.limit_in_pages = 1,
		.seg_not_present = 0,
		.useable = 1,
	};

	if (syscall(SYS_set_thread_area, &desc) == -1) {
		return -errno;
	}

	entry_number = desc.entry_number;
	__asm__ volatile("movl %0, %%fs" :: "r"(entry_number * 8 + 3));
	return 0;
#else
	(void)tsd_base;
	return -ENOTSUP;
#endif
}
