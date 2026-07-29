#ifndef DARLING_RUNTIME_MODE_PREFIX_H
#define DARLING_RUNTIME_MODE_PREFIX_H

#include "runtime_mode.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#define DARLING_RUNTIME_PREFIX_STATE_SCHEMA_VERSION 2
#define DARLING_RUNTIME_PREFIX_STATE_NAME ".darling-prefix-state-v2"
#define DARLING_RUNTIME_PREFIX_PROVENANCE \
	"darling-runtime-prefix-lifecycle-v2"

enum darling_runtime_prefix_anchor_state {
	DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED,
	DARLING_RUNTIME_PREFIX_ANCHOR_MISSING,
	DARLING_RUNTIME_PREFIX_ANCHOR_EMPTY,
	DARLING_RUNTIME_PREFIX_ANCHOR_POPULATED,
	DARLING_RUNTIME_PREFIX_ANCHOR_MOVED,
};

/*
 * Owning, move-only capability. The one-element array typedef makes ordinary
 * C assignment and copy-initialization ill-formed while retaining the same
 * zero-overhead representation and natural pointer decay at API boundaries.
 * The retained descriptors are the authority; the original path is
 * deliberately not retained. Transfer ownership only with
 * darling_runtime_prefix_move().
 */
typedef struct {
	int directory_fd;
	int parent_fd;
	int workdir_fd;
	char leaf[NAME_MAX + 1];
	char workdir_leaf[NAME_MAX + 1];
	enum darling_runtime_prefix_anchor_state anchor_state;
} darling_runtime_prefix[1];

#define DARLING_RUNTIME_PREFIX_INITIALIZER \
	{{ .directory_fd = -1, .parent_fd = -1, .workdir_fd = -1, \
		.leaf = {0}, .workdir_leaf = {0}, \
		.anchor_state = DARLING_RUNTIME_PREFIX_ANCHOR_UNINITIALIZED }}

enum darling_runtime_prefix_lifecycle_action {
	DARLING_RUNTIME_PREFIX_CREATED,
	DARLING_RUNTIME_PREFIX_REUSED,
	DARLING_RUNTIME_PREFIX_UPGRADED,
	DARLING_RUNTIME_PREFIX_REPAIRED,
	DARLING_RUNTIME_PREFIX_RECREATED,
	DARLING_RUNTIME_PREFIX_DELETED,
};

struct darling_runtime_prefix_state {
	unsigned schema_version;
	enum darling_runtime_mode runtime_mode;
	uint64_t generation;
	dev_t prefix_device;
	ino_t prefix_inode;
	uid_t owner_uid;
	gid_t owner_gid;
	char provenance[64];
};

struct darling_runtime_prefix_lifecycle_result {
	enum darling_runtime_prefix_lifecycle_action action;
	enum {
		DARLING_RUNTIME_PREFIX_NO_RECOVERY,
		DARLING_RUNTIME_PREFIX_RECOVERED_TRANSACTION,
	} recovery;
	struct darling_runtime_prefix_state state;
};

int darling_runtime_mode_open_prefix(
	const char* prefix,
	darling_runtime_prefix handle,
	char* error,
	size_t error_size
);

void darling_runtime_mode_close_prefix(
	darling_runtime_prefix handle
);

int darling_runtime_prefix_move(
	darling_runtime_prefix destination,
	darling_runtime_prefix source,
	char* error,
	size_t error_size
);

int darling_runtime_mode_setup_prefix(
	darling_runtime_prefix handle,
	const char* user_name,
	uid_t user_id,
	gid_t group_id,
	char* error,
	size_t error_size
);

int darling_runtime_mode_initialize_prefix_marker(
	const darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_validate_prefix_marker(
	const darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_prefix_needs_initialization(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
);

int darling_runtime_prefix_prepare(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	const char* user_name,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
);

int darling_runtime_prefix_recreate(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	const char* user_name,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
);

int darling_runtime_prefix_delete(
	darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_lifecycle_result* result,
	char* error,
	size_t error_size
);

int darling_runtime_prefix_read_state(
	const darling_runtime_prefix handle,
	enum darling_runtime_mode mode,
	uid_t owner_uid,
	gid_t owner_gid,
	struct darling_runtime_prefix_state* state,
	char* error,
	size_t error_size
);

int darling_runtime_mode_verify_prefix_name(
	const darling_runtime_prefix handle,
	char* error,
	size_t error_size
);

int darling_runtime_mode_prepare_workdir(
	darling_runtime_prefix handle,
	char* error,
	size_t error_size
);

int darling_runtime_mode_open_relative_directory(
	const darling_runtime_prefix handle,
	const char* relative,
	bool create,
	char* error,
	size_t error_size
);

int darling_runtime_mode_open_relative_file(
	const darling_runtime_prefix handle,
	const char* relative,
	int flags,
	mode_t mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_stat_relative(
	const darling_runtime_prefix handle,
	const char* relative,
	struct stat* status,
	char* error,
	size_t error_size
);

int darling_runtime_mode_unlink_relative(
	const darling_runtime_prefix handle,
	const char* relative,
	int flags,
	bool missing_ok,
	char* error,
	size_t error_size
);

int darling_runtime_mode_write_relative_atomic(
	const darling_runtime_prefix handle,
	const char* relative,
	const char* content,
	mode_t mode,
	char* error,
	size_t error_size
);

int darling_runtime_mode_make_fd_inheritable(
	int fd,
	char* error,
	size_t error_size
);

#ifdef DARLING_RUNTIME_PREFIX_LIFECYCLE_TESTING
int darling_runtime_prefix_test_checkpoint(const char* phase);

enum darling_runtime_prefix_test_operation {
	DARLING_RUNTIME_PREFIX_TEST_CREATE = 1,
	DARLING_RUNTIME_PREFIX_TEST_UPGRADE,
	DARLING_RUNTIME_PREFIX_TEST_RECREATE,
	DARLING_RUNTIME_PREFIX_TEST_DELETE,
};

enum darling_runtime_prefix_test_phase {
	DARLING_RUNTIME_PREFIX_TEST_PREPARED = 1,
	DARLING_RUNTIME_PREFIX_TEST_REPLACEMENT_STAGED,
	DARLING_RUNTIME_PREFIX_TEST_STATE_STAGED,
	DARLING_RUNTIME_PREFIX_TEST_STATE_PUBLISHED,
	DARLING_RUNTIME_PREFIX_TEST_PREFIX_PUBLISHED,
	DARLING_RUNTIME_PREFIX_TEST_CLEANUP,
};

enum darling_runtime_prefix_test_stable_relation {
	DARLING_RUNTIME_PREFIX_TEST_MISSING,
	DARLING_RUNTIME_PREFIX_TEST_OLD_EMPTY,
	DARLING_RUNTIME_PREFIX_TEST_OLD_LEGACY,
	DARLING_RUNTIME_PREFIX_TEST_OLD_CURRENT,
	DARLING_RUNTIME_PREFIX_TEST_NEW_CURRENT,
	DARLING_RUNTIME_PREFIX_TEST_UNEXPECTED,
};

enum darling_runtime_prefix_test_recovery {
	DARLING_RUNTIME_PREFIX_TEST_INVALID,
	DARLING_RUNTIME_PREFIX_TEST_ROLLBACK,
	DARLING_RUNTIME_PREFIX_TEST_FINISH,
};

int darling_runtime_prefix_test_recovery_matrix(
	enum darling_runtime_prefix_test_operation operation,
	enum darling_runtime_prefix_test_phase phase,
	enum darling_runtime_prefix_test_stable_relation stable
);
#endif

#endif
