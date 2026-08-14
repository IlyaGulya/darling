#ifndef DARLING_LIFECYCLE_COHORT_CLIENT_H
#define DARLING_LIFECYCLE_COHORT_CLIENT_H

#include <stddef.h>

enum darling_lifecycle_endpoint_kind {
	DARLING_LIFECYCLE_ENDPOINT_DARLINGSERVER = 1,
	DARLING_LIFECYCLE_ENDPOINT_SHELLSPAWN = 2,
	DARLING_LIFECYCLE_ENDPOINT_LAUNCHD = 3,
	DARLING_LIFECYCLE_ENDPOINT_PER_USER_LAUNCHD = 5,
};

typedef int (*darling_lifecycle_endpoint_activate_fn)(int endpoint_fd, void* context);

int darling_lifecycle_cohort_enabled(void);
int darling_lifecycle_publish_endpoint(enum darling_lifecycle_endpoint_kind kind);
int darling_lifecycle_retire_endpoint(enum darling_lifecycle_endpoint_kind kind);
int darling_lifecycle_publish_and_activate_endpoint(
	enum darling_lifecycle_endpoint_kind kind,
	darling_lifecycle_endpoint_activate_fn activate,
	void* context
);
int darling_lifecycle_publish_and_activate_dynamic_endpoint(
	enum darling_lifecycle_endpoint_kind kind,
	char* endpoint_path,
	size_t endpoint_path_capacity,
	darling_lifecycle_endpoint_activate_fn activate,
	void* context
);

#endif
