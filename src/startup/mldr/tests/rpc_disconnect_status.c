#include <errno.h>
#include <stdio.h>

#include "resources/dserver-rpc-defs.h"

static int expect_disconnect(long int status, int expected) {
	int actual = dserver_rpc_hooks_is_disconnect_status(status);
	if (actual != expected) {
		fprintf(stderr, "status %ld: expected disconnect=%d, got %d\n", status, expected, actual);
		return 1;
	}
	return 0;
}

int main(void) {
	int failures = 0;

	failures += expect_disconnect(-EBADF, 1);
	failures += expect_disconnect(-EPIPE, 1);
	failures += expect_disconnect(-ECONNRESET, 1);
	failures += expect_disconnect(-ENOTCONN, 1);
	failures += expect_disconnect(-ECONNREFUSED, 1);

	/* Ordinary failures must retain generated-wrapper diagnostics/semantics. */
	failures += expect_disconnect(-EIO, 0);
	failures += expect_disconnect(-ECOMM, 0);
	failures += expect_disconnect(-EINTR, 0);
	failures += expect_disconnect(0, 0);
	failures += expect_disconnect(1, 0);

	return failures ? 1 : 0;
}
