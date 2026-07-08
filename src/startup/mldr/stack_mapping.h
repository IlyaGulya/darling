#ifndef _MLDR_STACK_MAPPING_H_
#define _MLDR_STACK_MAPPING_H_

#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>

typedef void* (*mldr_stack_map_fn)(void* addr, size_t length, int prot, int flags, int fd, off_t offset);

static inline void* mldr_map_guest_stack(mldr_stack_map_fn map_fn, unsigned long preferred_top,
		unsigned long size, unsigned long* stack_top)
{
	void* preferred_base = (void*)(preferred_top - size);
	void* stack = map_fn(preferred_base, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_GROWSDOWN, -1, 0);
	if (stack == MAP_FAILED && errno == EEXIST) {
		stack = map_fn(preferred_base, size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
	}
	if (stack != MAP_FAILED && stack_top) {
		*stack_top = (unsigned long)stack + size;
	}
	return stack;
}

#endif // _MLDR_STACK_MAPPING_H_
