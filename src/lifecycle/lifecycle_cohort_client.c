#define _GNU_SOURCE
#include "lifecycle_cohort_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_NAME_CAPACITY 80
#define NONCE_BYTES 32
#define PROTOCOL_VERSION 2
#define SHELLSPAWN_LISTEN_BACKLOG 16384
#define IMPORTED_ENDPOINT_FD_MIN 256
#define DYNAMIC_PATH_CAPACITY 96
#define OPERATION_PUBLISH 1
#define OPERATION_RETIRE 2
#define OPERATION_COMMIT 3
#define OPERATION_ABORT 4
#define RESPONSE_PHASE_REQUEST 1
#define RESPONSE_PHASE_PUBLISH 2
#define RESPONSE_PHASE_COMMIT 3
#define RESPONSE_PHASE_ABORT 4
#define RESPONSE_PHASE_RETIRE 5
#define RESPONSE_ERROR_NONE 0
#define RESPONSE_ERROR_IO 1
#define RESPONSE_ERROR_IDENTITY 2
#define RESPONSE_ERROR_LOCK_BUSY 3
#define RESPONSE_ERROR_PROTOCOL 4
#define RESPONSE_ERROR_ENDPOINT_EXISTS 5
#define RESPONSE_ERROR_ENDPOINT_MISSING 6
#define RESPONSE_ERROR_PROCESS 7

static const uint8_t protocol_magic[8] = {'D', 'L', 'C', 'O', 'H', 'R', '1', 0};

struct wire_request {
	uint8_t magic[8];
	uint16_t version;
	uint16_t operation;
	uint16_t endpoint;
	uint16_t reserved;
	uint8_t nonce[NONCE_BYTES];
};

struct wire_response {
	uint8_t magic[8];
	uint16_t version;
	int16_t status;
	uint16_t endpoint;
	uint16_t has_fd;
	uint16_t phase;
	uint16_t reserved;
	int32_t error;
	uint64_t device;
	uint64_t inode;
	uint16_t path_len;
	uint8_t path[DYNAMIC_PATH_CAPACITY];
};

struct pending_publication {
	int control;
	enum darling_lifecycle_endpoint_kind kind;
	uint8_t nonce[NONCE_BYTES];
};

static int response_error_to_errno(int32_t error) {
	switch (error) {
	case RESPONSE_ERROR_IO:
		return EIO;
	case RESPONSE_ERROR_IDENTITY:
		return ESTALE;
	case RESPONSE_ERROR_LOCK_BUSY:
		return EBUSY;
	case RESPONSE_ERROR_PROTOCOL:
		return EPROTO;
	case RESPONSE_ERROR_ENDPOINT_EXISTS:
		return EEXIST;
	case RESPONSE_ERROR_ENDPOINT_MISSING:
		return ENOENT;
	case RESPONSE_ERROR_PROCESS:
		return ECHILD;
	default:
		return EPROTO;
	}
}

static int decode_nibble(char value) {
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	return -1;
}

static int validate_dynamic_path(const uint8_t* path, size_t length) {
	static const char prefix[] = "/private/var/tmp/launchd-";
	static const char suffix[] = "/sock";
	const size_t prefix_length = sizeof(prefix) - 1;
	const size_t suffix_length = sizeof(suffix) - 1;
	if (length <= prefix_length + 1 + 8 + suffix_length ||
		memcmp(path, prefix, sizeof(prefix) - 1) != 0 ||
		memcmp(path + length - suffix_length, suffix, suffix_length) != 0)
		return -1;
	size_t index = prefix_length;
	if (path[index] < '1' || path[index] > '9')
		return -1;
	while (index < length - suffix_length && path[index] >= '0' && path[index] <= '9')
		++index;
	if (index >= length - suffix_length || path[index++] != '-' ||
		length - suffix_length - index != 8)
		return -1;
	for (; index < length - suffix_length; ++index) {
		uint8_t value = path[index];
		if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
			return -1;
	}
	return 0;
}

static int load_envelope(char* name, size_t* name_length, uint8_t nonce[NONCE_BYTES]) {
	const char* raw_name = getenv("DARLING_LIFECYCLE_CONTROL_NAME");
	const char* raw_nonce = getenv("DARLING_LIFECYCLE_CONTROL_NONCE");
	if (!raw_name || !raw_nonce)
		return -1;
	*name_length = strlen(raw_name);
	if (*name_length == 0 || *name_length >= CONTROL_NAME_CAPACITY || strlen(raw_nonce) != NONCE_BYTES * 2)
		return -1;
	memcpy(name, raw_name, *name_length);
	for (size_t index = 0; index < NONCE_BYTES; ++index) {
		int high = decode_nibble(raw_nonce[index * 2]);
		int low = decode_nibble(raw_nonce[index * 2 + 1]);
		if (high < 0 || low < 0)
			return -1;
		nonce[index] = (uint8_t)((high << 4) | low);
	}
	return 0;
}

int darling_lifecycle_cohort_enabled(void) {
	const char* enabled = getenv("DARLING_LIFECYCLE_COHORT_V1");
	return enabled && strcmp(enabled, "1") == 0;
}

static int connect_controller(const char* name, size_t name_length) {
#ifdef DARLING_LIFECYCLE_COHORT_TESTING
	char test_name[4096];
#endif
	const char* connect_name = name;
	size_t connect_name_length = name_length;
#ifdef DARLING_LIFECYCLE_COHORT_TESTING
	const char* test_root = getenv("DARLING_LIFECYCLE_CONTROL_TEST_ROOT");
	if (test_root) {
		int length = snprintf(test_name, sizeof(test_name), "%s%.*s", test_root,
			(int)name_length, name);
		if (length <= 0 || (size_t)length >= sizeof(test_name))
			return -1;
		connect_name = test_name;
		connect_name_length = (size_t)length;
	}
#endif

	int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -1;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
		close(fd);
		return -1;
	}
	struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
		close(fd);
		return -1;
	}
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (connect_name_length == 0 || connect_name_length >= sizeof(address.sun_path)) {
		close(fd);
		return -1;
	}
	memcpy(address.sun_path, connect_name, connect_name_length);
	socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + connect_name_length + 1);
#ifdef __APPLE__
	address.sun_len = (uint8_t)length;
#endif
	if (connect(fd, (struct sockaddr*)&address, length) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int begin_transaction(
	enum darling_lifecycle_endpoint_kind kind,
	uint16_t operation,
	struct pending_publication* pending,
	char* dynamic_path,
	size_t dynamic_path_capacity
) {
	char control_name[CONTROL_NAME_CAPACITY];
	size_t control_name_length;
	struct wire_request request;
	memset(&request, 0, sizeof(request));
	if (load_envelope(control_name, &control_name_length, request.nonce) != 0)
		return -1;
	memcpy(request.magic, protocol_magic, sizeof(protocol_magic));
	request.version = PROTOCOL_VERSION;
	request.operation = operation;
	request.endpoint = (uint16_t)kind;

	int control = connect_controller(control_name, control_name_length);
	if (control < 0)
		return -1;
	if (send(control, &request, sizeof(request), MSG_NOSIGNAL) != (ssize_t)sizeof(request)) {
		close(control);
		return -1;
	}

	struct wire_response response;
	memset(&response, 0, sizeof(response));
	struct iovec iov = {.iov_base = &response, .iov_len = sizeof(response)};
	uint8_t ancillary[CMSG_SPACE(sizeof(int))];
	memset(ancillary, 0, sizeof(ancillary));
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = ancillary;
	message.msg_controllen = sizeof(ancillary);
	// Darwin has no MSG_CMSG_CLOEXEC. Both cohort consumers perform this
	// transaction in their single-threaded bootstrap phase, before launchd
	// starts jobs or shellspawn accepts/forks, and set FD_CLOEXEC below before
	// exposing the descriptor to either event loop.
	ssize_t received = recvmsg(control, &message, 0);
	int received_fd = -1;
	int ancillary_valid = 1;
	for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
			header->cmsg_len < CMSG_LEN(0)) {
			ancillary_valid = 0;
			continue;
		}
		size_t payload = header->cmsg_len - CMSG_LEN(0);
		if (payload % sizeof(int) != 0 || payload != sizeof(int) || received_fd >= 0)
			ancillary_valid = 0;
		for (size_t offset = 0; offset + sizeof(int) <= payload; offset += sizeof(int)) {
			int descriptor = -1;
			memcpy(&descriptor, (char*)CMSG_DATA(header) + offset, sizeof(descriptor));
			if (received_fd < 0 && payload == sizeof(int) && ancillary_valid) {
				received_fd = descriptor;
			} else if (descriptor >= 0) {
				close(descriptor);
			}
		}
	}
	if (received != (ssize_t)sizeof(response) ||
		(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
		!ancillary_valid ||
		memcmp(response.magic, protocol_magic, sizeof(protocol_magic)) != 0 ||
		response.version != PROTOCOL_VERSION ||
		response.endpoint != (uint16_t)kind) {
		if (received_fd >= 0)
			close(received_fd);
		close(control);
		return -1;
	}
	uint16_t expected_phase = operation == OPERATION_PUBLISH
		? RESPONSE_PHASE_PUBLISH : RESPONSE_PHASE_RETIRE;
	if (response.phase != expected_phase || response.reserved != 0 ||
		(response.status == 0 ? response.error != RESPONSE_ERROR_NONE : response.error == RESPONSE_ERROR_NONE)) {
		if (received_fd >= 0)
			close(received_fd);
		close(control);
		errno = EPROTO;
		return -1;
	}
	if (response.status != 0) {
		if (received_fd >= 0)
			close(received_fd);
		close(control);
		errno = response_error_to_errno(response.error);
		return -1;
	}
	if (operation == OPERATION_PUBLISH) {
		int dynamic = kind == DARLING_LIFECYCLE_ENDPOINT_PER_USER_LAUNCHD;
		if (response.path_len >= DYNAMIC_PATH_CAPACITY ||
			(dynamic && (!dynamic_path || dynamic_path_capacity <= response.path_len ||
				response.path_len == 0 || response.path[response.path_len] != 0 ||
				validate_dynamic_path(response.path, response.path_len) != 0)) ||
			(!dynamic && (response.path_len != 0 || response.path[0] != 0))) {
			if (received_fd >= 0)
				close(received_fd);
			close(control);
			return -1;
		}
		struct stat status;
		int socket_type = 0;
		int accepting = 0;
		socklen_t option_length = sizeof(int);
		if (response.has_fd != 1 || received_fd < 0 || fstat(received_fd, &status) != 0 ||
			!S_ISSOCK(status.st_mode) || response.device != (uint64_t)status.st_dev ||
			response.inode != (uint64_t)status.st_ino || response.inode == 0 ||
			getsockopt(received_fd, SOL_SOCKET, SO_TYPE, &socket_type, &option_length) != 0 ||
			option_length != sizeof(int) || socket_type != SOCK_STREAM ||
			getsockopt(received_fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &option_length) != 0 ||
			option_length != sizeof(int) || accepting != 1 ||
			fcntl(received_fd, F_SETFD, FD_CLOEXEC) != 0) {
			if (received_fd >= 0)
				close(received_fd);
			close(control);
			return -1;
		}
		/* Avoid a stale libkqueue knote keyed by a low descriptor number that
		 * was used and closed during launchd bootstrap before SCM_RIGHTS chose
		 * the same number for this imported capability. */
		#ifdef DARLING_LIFECYCLE_COHORT_TESTING
		const char* adoption_fault = getenv("DARLING_LIFECYCLE_COHORT_TEST_ADOPTION_FAULT");
		if (adoption_fault && strcmp(adoption_fault, "dup") == 0) {
			close(received_fd);
			close(control);
			errno = EMFILE;
			return -1;
		}
		#endif
		int imported_fd = fcntl(received_fd, F_DUPFD_CLOEXEC, IMPORTED_ENDPOINT_FD_MIN);
		if (imported_fd < 0) {
			close(received_fd);
			close(control);
			return -1;
		}
		close(received_fd);
		received_fd = imported_fd;
		/*
		 * The Rust controller creates and listens on the Linux socket before
		 * transferring it. Darling's guest libkqueue listen registry is
		 * process-local, so adopt the already-listening descriptor with an
		 * idempotent listen(2) before a consumer registers EVFILT_READ. This
		 * performs no namespace mutation and preserves the endpoint-specific
		 * production backlog.
		 */
		int backlog = kind == DARLING_LIFECYCLE_ENDPOINT_SHELLSPAWN
			? SHELLSPAWN_LISTEN_BACKLOG : SOMAXCONN;
		#ifdef DARLING_LIFECYCLE_COHORT_TESTING
		if (adoption_fault && strcmp(adoption_fault, "listen") == 0) {
			close(received_fd);
			close(control);
			errno = EIO;
			return -1;
		}
		#endif
		if (listen(received_fd, backlog) != 0) {
			close(received_fd);
			close(control);
			return -1;
		}
		if (!pending) {
			close(received_fd);
			close(control);
			return -1;
		}
		pending->control = control;
		pending->kind = kind;
		memcpy(pending->nonce, request.nonce, sizeof(pending->nonce));
		if (dynamic) {
			memcpy(dynamic_path, response.path, response.path_len);
			dynamic_path[response.path_len] = 0;
		}
		return received_fd;
	}
	if (response.has_fd != 0 || received_fd >= 0 || response.device != 0 ||
		response.inode != 0 || response.path_len != 0 || response.reserved != 0 ||
		response.path[0] != 0) {
		if (received_fd >= 0)
			close(received_fd);
		close(control);
		return -1;
	}
	close(control);
	return 0;
}

static int finish_publication(
	struct pending_publication* pending,
	uint16_t operation
) {
	struct wire_request decision;
	memset(&decision, 0, sizeof(decision));
	if ((operation != OPERATION_COMMIT && operation != OPERATION_ABORT) ||
		!pending || pending->control < 0) {
		if (pending && pending->control >= 0) {
			close(pending->control);
			pending->control = -1;
		}
		return -1;
	}
	int control = pending->control;
	pending->control = -1;
	memcpy(decision.magic, protocol_magic, sizeof(protocol_magic));
	decision.version = PROTOCOL_VERSION;
	decision.operation = operation;
	decision.endpoint = (uint16_t)pending->kind;
	memcpy(decision.nonce, pending->nonce, sizeof(decision.nonce));
	if (send(control, &decision, sizeof(decision), MSG_NOSIGNAL) !=
		(ssize_t)sizeof(decision)) {
		close(control);
		return -1;
	}

	/* A complete COMMIT message is the point of no return. The controller
	 * records ownership before attempting this diagnostic acknowledgement, so
	 * a lost, interrupted, or timed-out ACK must not make the client close its
	 * already adopted endpoint and leave a live owner without a listener. */
	int skip_ack = 0;
	#ifdef DARLING_LIFECYCLE_COHORT_TESTING
	const char* final_ack_fault = getenv("DARLING_LIFECYCLE_COHORT_TEST_FINAL_ACK_FAULT");
	skip_ack = operation == OPERATION_COMMIT && final_ack_fault &&
		strcmp(final_ack_fault, "lost") == 0;
	#endif
	struct wire_response response;
	memset(&response, 0, sizeof(response));
	ssize_t received = -1;
	if (!skip_ack) {
		do {
			received = recv(control, &response, sizeof(response), 0);
		} while (received < 0 && errno == EINTR);
	}
	int acknowledged = received == (ssize_t)sizeof(response) &&
		memcmp(response.magic, protocol_magic, sizeof(protocol_magic)) == 0 &&
		response.version == PROTOCOL_VERSION && response.status == 0 &&
		response.endpoint == (uint16_t)pending->kind && response.has_fd == 0 &&
		response.phase == (operation == OPERATION_COMMIT ? RESPONSE_PHASE_COMMIT : RESPONSE_PHASE_ABORT) &&
		response.error == RESPONSE_ERROR_NONE &&
		response.device == 0 && response.inode == 0 && response.path_len == 0 &&
		response.reserved == 0 && response.path[0] == 0;
	close(control);
	return operation == OPERATION_COMMIT ? 0 : (acknowledged ? 0 : -1);
}

int darling_lifecycle_publish_endpoint(enum darling_lifecycle_endpoint_kind kind) {
	struct pending_publication pending = {.control = -1, .kind = kind};
	int endpoint = begin_transaction(kind, OPERATION_PUBLISH, &pending, NULL, 0);
	if (endpoint < 0)
		return -1;
	if (finish_publication(&pending, OPERATION_COMMIT) != 0) {
		close(endpoint);
		return -1;
	}
	return endpoint;
}

int darling_lifecycle_retire_endpoint(enum darling_lifecycle_endpoint_kind kind) {
	return begin_transaction(kind, OPERATION_RETIRE, NULL, NULL, 0);
}

int darling_lifecycle_publish_and_activate_endpoint(
	enum darling_lifecycle_endpoint_kind kind,
	darling_lifecycle_endpoint_activate_fn activate,
	void* context
) {
	if (!activate)
		return -1;
	struct pending_publication pending = {.control = -1, .kind = kind};
	int endpoint = begin_transaction(kind, OPERATION_PUBLISH, &pending, NULL, 0);
	if (endpoint < 0)
		return -1;
	if (activate(endpoint, context) == 0) {
		if (finish_publication(&pending, OPERATION_COMMIT) != 0) {
			close(endpoint);
			return -1;
		}
		return endpoint;
	}

	int activation_errno = errno;
	(void)finish_publication(&pending, OPERATION_ABORT);
	close(endpoint);
	errno = activation_errno;
	return -1;
}

int darling_lifecycle_publish_and_activate_dynamic_endpoint(
	enum darling_lifecycle_endpoint_kind kind,
	char* endpoint_path,
	size_t endpoint_path_capacity,
	darling_lifecycle_endpoint_activate_fn activate,
	void* context
) {
	if (kind != DARLING_LIFECYCLE_ENDPOINT_PER_USER_LAUNCHD || !endpoint_path ||
		endpoint_path_capacity == 0 || !activate)
		return -1;
	struct pending_publication pending = {.control = -1, .kind = kind};
	int endpoint = begin_transaction(
		kind, OPERATION_PUBLISH, &pending, endpoint_path, endpoint_path_capacity
	);
	if (endpoint < 0)
		return -1;
	if (activate(endpoint, context) == 0) {
		if (finish_publication(&pending, OPERATION_COMMIT) != 0) {
			close(endpoint);
			return -1;
		}
		return endpoint;
	}
	int activation_errno = errno;
	(void)finish_publication(&pending, OPERATION_ABORT);
	close(endpoint);
	endpoint_path[0] = 0;
	errno = activation_errno;
	return -1;
}
