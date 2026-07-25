#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>

#include <darlingserver/rpc-supplement.h>

#include <rtsig.h>

#define dserver_rpc_hooks_msghdr_t struct msghdr
#define dserver_rpc_hooks_iovec_t struct iovec
#define dserver_rpc_hooks_cmsghdr_t struct cmsghdr
#define DSERVER_RPC_HOOKS_CMSG_SPACE CMSG_SPACE
#define DSERVER_RPC_HOOKS_CMSG_FIRSTHDR CMSG_FIRSTHDR
#define DSERVER_RPC_HOOKS_SOL_SOCKET SOL_SOCKET
#define DSERVER_RPC_HOOKS_SCM_RIGHTS SCM_RIGHTS
#define DSERVER_RPC_HOOKS_CMSG_LEN CMSG_LEN
#define DSERVER_RPC_HOOKS_CMSG_DATA CMSG_DATA
#define DSERVER_RPC_HOOKS_ATTRIBUTE static

#define dserver_rpc_hooks_get_pid getpid

#define dserver_rpc_hooks_get_tid() ((pid_t)syscall(SYS_gettid))

#if __x86_64__
	#define dserver_rpc_hooks_get_architecture() dserver_rpc_architecture_x86_64
#elif __i386__
	#define dserver_rpc_hooks_get_architecture() dserver_rpc_architecture_i386
#elif __aarch64__
	#define dserver_rpc_hooks_get_architecture() dserver_rpc_architecture_arm64
#elif __arm__
	#define dserver_rpc_hooks_get_architecture() dserver_rpc_architecture_arm32
#else
	#define dserver_rpc_hooks_get_architecture() dserver_rpc_architecture_invalid
#endif

extern struct sockaddr_un __dserver_socket_address_data;

#define dserver_rpc_hooks_get_server_address() ((void*)&__dserver_socket_address_data)

#define dserver_rpc_hooks_get_server_address_length() sizeof(__dserver_socket_address_data)

#define dserver_rpc_hooks_memcpy memcpy

static long int dserver_rpc_hooks_send_message(int socket, const dserver_rpc_hooks_msghdr_t* message) {
	ssize_t ret = sendmsg(socket, message, 0);
	if (ret < 0) {
		return -errno;
	}
	return ret;
};

// perf #3 (dar-dar6x4-perf-5dq.3): adaptive recv. The synchronous checkin/RPC
// round-trip is dominated NOT by server processing (server-side p50 ~8us after
// perf #2b) nor by socket setup (~10us, ~5%), but by the ~200us scheduler
// sleep/wakeup latency of blocking in recvmsg waiting for the reply datagram. When
// the server replies quickly (the common case), a short bounded NON-BLOCKING recv
// spin can grab the reply before the thread ever sleeps, saving the full wakeup
// latency. On a slow reply it falls back to a normal BLOCKING recvmsg, so it never
// busy-waits unboundedly (that is exactly the perf #1 starvation we already fixed
// on the creator side -- this is the symmetric fix on the waiter side). The spin
// budget is tiny and capped, and tunable via DARLING_PERF3_RECVSPIN:
//   unset       -> default DARLING_PERF3_RECVSPIN_DEFAULT polls (ON, the win)
//   0           -> disabled: legacy blocking recvmsg (escape hatch / A-B baseline)
//   N (N>0)     -> spin up to N non-blocking polls, then block
// Measured: per-checkin RPC latency 237us -> 183us (~23%) single-storm, fork/exec/
// wait correctness unaffected. The default is deliberately modest: each poll is one
// MSG_DONTWAIT recvmsg (~1us) + a pause, so the default window (~500us worst case)
// comfortably covers the server's p99 reply yet exits in a few us on the common fast
// reply; a genuinely slow reply falls through to a real blocking wait.
#ifndef DARLING_PERF3_RECVSPIN_DEFAULT
#define DARLING_PERF3_RECVSPIN_DEFAULT 512
#endif
static int __perf3_recvspin_iters(void) {
	static _Atomic int cached = -1;
	int v = atomic_load_explicit(&cached, memory_order_relaxed);
	if (v == -1) {
		const char* s = getenv("DARLING_PERF3_RECVSPIN");
		v = (s && s[0]) ? atoi(s) : DARLING_PERF3_RECVSPIN_DEFAULT;
		if (v < 0) v = 0;
		if (v > 200000) v = 200000;
		atomic_store_explicit(&cached, v, memory_order_relaxed);
	}
	return v;
}

static long int dserver_rpc_hooks_receive_message(int socket, dserver_rpc_hooks_msghdr_t* out_message) {
	ssize_t ret;

	int spin = __perf3_recvspin_iters();
	if (spin > 0) {
		// Bounded non-blocking poll: catch a fast reply (server p50 ~8us) without
		// paying the ~200us recvmsg sleep/wakeup. Between polls we issue a CPU
		// PAUSE (relax) rather than sched_yield(): on a busy/oversubscribed host
		// sched_yield donates the core to every other runnable task, which both
		// lengthens the spin wall-time AND starves nobody usefully (the reply comes
		// from the server on a DIFFERENT core); pause keeps us on-core for the few
		// microseconds it takes the reply to land, so the poll is short and does not
		// fight the rest of the system for the scheduler. The count is capped, so on
		// a genuinely slow reply we fall through to a real blocking recvmsg quickly
		// and never busy-wait unboundedly (the perf #1 starvation we already fixed).
		for (int i = 0; i < spin; ++i) {
			ret = recvmsg(socket, out_message, MSG_DONTWAIT);
			if (ret >= 0) {
				goto got_message;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				return -errno;
			}
#if defined(__x86_64__) || defined(__i386__)
			__builtin_ia32_pause();
#elif defined(__aarch64__)
			__asm__ __volatile__("yield");
#else
			sched_yield();
#endif
		}
	}

	ret = recvmsg(socket, out_message, 0);
	if (ret < 0) {
		return -errno;
	}

got_message:
	if (ret >= sizeof(dserver_s2c_callhdr_t)) {
		dserver_s2c_callhdr_t* callhdr = out_message->msg_iov->iov_base;
		if (callhdr->call_number == 0x52cca11) {
			// this is an S2C call
			// mldr shouldn't need to be doing S2C calls
			fprintf(stderr, "mldr darlingserver RPC hooks received S2C call\n");
			abort();
		}
	}

	return ret;
};

#define dserver_rpc_hooks_get_bad_message_status() (-EBADMSG)

#define dserver_rpc_hooks_get_communication_error_status() (-ECOMM)

#define dserver_rpc_hooks_get_broken_pipe_status() (-EPIPE)

/*
 * The RPC generator asks each consumer to classify transport disconnects.
 * mldr's send hook returns negative host errno values, so retain the same
 * Linux-side disconnect set used by libsystem_kernel in its errno domain.
 * These statuses are only special for generated interruptible/quiet sends;
 * every other negative send result remains an ordinary RPC failure.
 */
static int dserver_rpc_hooks_is_disconnect_status(long int status) {
	return status == -EBADF
		|| status == -EPIPE
		|| status == -ECONNRESET
		|| status == -ENOTCONN
		|| status == -ECONNREFUSED;
}

#define dserver_rpc_hooks_close_fd close

extern int __dserver_main_thread_socket_fd;

#define dserver_rpc_hooks_get_socket() __dserver_main_thread_socket_fd

#define dserver_rpc_hooks_printf(...) fprintf(stderr, ## __VA_ARGS__)

#define dserver_rpc_hooks_atomic_save_t sigset_t

static void dserver_rpc_hooks_atomic_begin(dserver_rpc_hooks_atomic_save_t* atomic_save) {
	sigset_t set;
	sigfillset(&set);
	sigdelset(&set, LINUX_SIGRTMIN);
	sigdelset(&set, LINUX_SIGRTMIN + 1);
	pthread_sigmask(SIG_BLOCK, &set, atomic_save);
};

static void dserver_rpc_hooks_atomic_end(dserver_rpc_hooks_atomic_save_t* atomic_save) {
	pthread_sigmask(SIG_SETMASK, atomic_save, NULL);
};

#define dserver_rpc_hooks_get_interrupt_status() (-EINTR)

static void dserver_rpc_hooks_push_reply(int socket, const dserver_rpc_hooks_msghdr_t* reply, size_t size) {
	// we shouldn't need to push any replies in mldr
	abort();
};
