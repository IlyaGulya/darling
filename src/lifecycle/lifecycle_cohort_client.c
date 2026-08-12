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
#define PROTOCOL_VERSION 1

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
	uint64_t device;
	uint64_t inode;
};

static int decode_nibble(char value) {
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	return -1;
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

static int connect_controller(void) {
	char name[CONTROL_NAME_CAPACITY];
#ifdef DARLING_LIFECYCLE_COHORT_TESTING
	char test_name[4096];
#endif
	uint8_t ignored_nonce[NONCE_BYTES];
	size_t name_length;
	if (load_envelope(name, &name_length, ignored_nonce) != 0)
		return -1;
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

static int transact(enum darling_lifecycle_endpoint_kind kind, uint16_t operation) {
	char ignored_name[CONTROL_NAME_CAPACITY];
	size_t ignored_name_length;
	struct wire_request request;
	memset(&request, 0, sizeof(request));
	if (load_envelope(ignored_name, &ignored_name_length, request.nonce) != 0)
		return -1;
	memcpy(request.magic, protocol_magic, sizeof(protocol_magic));
	request.version = PROTOCOL_VERSION;
	request.operation = operation;
	request.endpoint = (uint16_t)kind;

	int control = connect_controller();
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
	close(control);
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
		response.version != PROTOCOL_VERSION || response.status != 0 ||
		response.endpoint != (uint16_t)kind) {
		if (received_fd >= 0)
			close(received_fd);
		return -1;
	}
	if (operation == 1) {
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
			return -1;
		}
		return received_fd;
	}
	if (response.has_fd != 0 || received_fd >= 0 || response.device != 0 || response.inode != 0) {
		if (received_fd >= 0)
			close(received_fd);
		return -1;
	}
	return 0;
}

int darling_lifecycle_publish_endpoint(enum darling_lifecycle_endpoint_kind kind) {
	return transact(kind, 1);
}

int darling_lifecycle_retire_endpoint(enum darling_lifecycle_endpoint_kind kind) {
	return transact(kind, 2);
}

int darling_lifecycle_publish_and_activate_endpoint(
	enum darling_lifecycle_endpoint_kind kind,
	darling_lifecycle_endpoint_activate_fn activate,
	void* context
) {
	if (!activate)
		return -1;
	int endpoint = darling_lifecycle_publish_endpoint(kind);
	if (endpoint < 0)
		return -1;
	if (activate(endpoint, context) == 0)
		return endpoint;

	int activation_errno = errno;
	close(endpoint);
	if (darling_lifecycle_retire_endpoint(kind) != 0)
		return -1;
	errno = activation_errno;
	return -1;
}
