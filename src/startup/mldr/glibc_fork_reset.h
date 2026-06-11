#ifndef _MLDR_GLIBC_FORK_RESET_H_
#define _MLDR_GLIBC_FORK_RESET_H_

// Darling forks with a raw clone/__NR_fork (see sys_fork), which is _Fork-level:
// it resets only the robust-mutex list and skips glibc's __libc_fork child path.
// That child path (posix/fork.c) unconditionally resets the dynamic-loader locks
// GL(_dl_load_lock), GL(_dl_load_tls_lock) and the stack-cache lock/lists. Without
// it, a raw-fork child inherits whatever those locks held in the parent and
// deadlocks the first time it dlopen()s or pthread_create()s.
//
// These two entry points reimplement that reset faithfully for the raw-fork child.

// Locate the glibc loader/stack locks. MUST be called once, early, while the
// process is still single-threaded (so the lists/locks are in their initial,
// recognisable state).
void __mldr_glibc_fork_reset_detect(void);

// Reset the inherited glibc loader/stack locks in a raw-fork child. Safe to call
// even if detection failed (it then does nothing). Async-signal-safe.
void __mldr_glibc_fork_reset_child(void);

#endif // _MLDR_GLIBC_FORK_RESET_H_
