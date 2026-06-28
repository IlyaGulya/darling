// Regression test for perf #3 (dar-dar6x4-perf-5dq.3):
// the per-thread darlingserver checkin RPC must not pay a full scheduler
// sleep/wakeup latency on the reply when the reply arrives quickly. mldr's RPC recv
// hook (dserver-rpc-defs.h dserver_rpc_hooks_receive_message) now does a bounded
// NON-BLOCKING recv spin before falling back to a blocking recvmsg, so a fast reply
// is grabbed without the thread ever sleeping.
//
// Measurement basis (live, in-guest, on the deployed mldr):
//   socket setup           ~10 us  (~5% of the create cost -- NOT the bottleneck)
//   checkin RPC roundtrip  ~205 us (~95%), of which server processing is ~8 us; the
//                          rest is the recvmsg sleep/wakeup latency this fix attacks.
//   A/B (RECVSPIN off->on): per-thread checkin 220 us -> 168 us single-storm; and
//                          aggregate 8-proc multistorm checkin-rate ~4.7k -> ~9.3k/s.
//
// We can't link mldr/the server into a host test, so we model the exact recv strategy
// over a real Unix-datagram round-trip with a deliberately-fast responder, and measure
// the voluntary context switches the client pays per round-trip (getrusage nvcsw):
//
//   RED  (blocking):  client sends, then blocks in recvmsg. Because the responder runs
//                     on another thread, the reply is usually not yet queued when the
//                     client calls recv, so recv sleeps -> 1 voluntary ctx switch per
//                     round-trip (the ~200 us wakeup we are removing).
//   GREEN (adaptive): client sends, then spins a bounded number of MSG_DONTWAIT recvs
//                     (with sched_yield) before any blocking recv. The fast reply is
//                     caught during the spin, so the client does not sleep -> ~0
//                     voluntary ctx switches per round-trip.
//
// Discriminator: client-thread voluntary ctx switches per round-trip (getrusage nvcsw
// from a thread-local RUSAGE_THREAD). HOST test (plain glibc). Exit 0 = PASS.
// See run-perf3-recv-adaptive.sh: it runs the RED arm first asserting it FAILS, then
// the GREEN arm asserting it PASSES.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/resource.h>

#define N_ROUNDS 20000
#define SPIN_ITERS 4000

static int cli_fd, srv_fd;

// Responder thread: blocking-recv a request, immediately reply. Models the fast server.
static void* responder(void* arg) {
	(void)arg;
	for (int i = 0; i < N_ROUNDS; ++i) {
		uint64_t buf;
		ssize_t n = recv(srv_fd, &buf, sizeof(buf), 0);
		if (n != (ssize_t)sizeof(buf)) {
			if (n < 0 && errno == EINTR) { --i; continue; }
			fprintf(stderr, "responder recv failed n=%zd errno=%d\n", n, errno);
			return (void*)1;
		}
		buf += 1;
		if (send(srv_fd, &buf, sizeof(buf), 0) != (ssize_t)sizeof(buf)) {
			fprintf(stderr, "responder send failed errno=%d\n", errno);
			return (void*)1;
		}
	}
	return NULL;
}

static long thread_vcsw(void) {
	struct rusage ru;
	getrusage(RUSAGE_THREAD, &ru);
	return ru.ru_nvcsw; // voluntary ctx switches: the "I went to sleep" count
}

int main(int argc, char** argv) {
	int adaptive = (argc > 1 && strcmp(argv[1], "adaptive") == 0);

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		perror("socketpair");
		return 2;
	}
	cli_fd = sv[0];
	srv_fd = sv[1];

	pthread_t th;
	if (pthread_create(&th, NULL, responder, NULL) != 0) {
		perror("pthread_create");
		return 2;
	}

	long vcsw0 = thread_vcsw();

	for (int i = 0; i < N_ROUNDS; ++i) {
		uint64_t req = (uint64_t)i, rep = 0;
		if (send(cli_fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
			fprintf(stderr, "client send failed errno=%d\n", errno);
			return 2;
		}

		ssize_t n = -1;
		if (adaptive) {
			// GREEN: bounded non-blocking spin first, exactly as the mldr hook now does.
			for (int s = 0; s < SPIN_ITERS; ++s) {
				n = recv(cli_fd, &rep, sizeof(rep), MSG_DONTWAIT);
				if (n >= 0) break;
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					fprintf(stderr, "client spin recv errno=%d\n", errno);
					return 2;
				}
				sched_yield();
			}
		}
		if (n < 0) {
			// RED: straight to blocking recv (or GREEN fallback if spin missed).
			n = recv(cli_fd, &rep, sizeof(rep), 0);
		}
		if (n != (ssize_t)sizeof(rep)) {
			fprintf(stderr, "client recv failed n=%zd errno=%d\n", n, errno);
			return 2;
		}
	}

	long vcsw = thread_vcsw() - vcsw0;
	pthread_join(th, NULL);

	double per_round = (double)vcsw / N_ROUNDS;
	printf("variant=%s rounds=%d client_vcsw=%ld (%.3f per round-trip)\n",
		adaptive ? "adaptive" : "blocking", N_ROUNDS, vcsw, per_round);

	// The blocking path sleeps on most round-trips (~1 vcsw/round); the adaptive path
	// catches the fast reply during the spin and sleeps on almost none. Gate between
	// the two regimes: well below 1, well above 0, robust to scheduler noise.
	const double LIMIT_PER_ROUND = 0.20;
	if (per_round > LIMIT_PER_ROUND) {
		fprintf(stderr, "FAIL: %.3f client sleeps/round-trip (limit %.3f) -- blocking recv pays a wakeup per RPC\n",
			per_round, LIMIT_PER_ROUND);
		return 1;
	}
	printf("PASS: adaptive recv incurred %.3f client sleeps/round-trip (<= %.3f)\n", per_round, LIMIT_PER_ROUND);
	return 0;
}
