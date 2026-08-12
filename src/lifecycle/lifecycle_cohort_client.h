#ifndef DARLING_LIFECYCLE_COHORT_CLIENT_H
#define DARLING_LIFECYCLE_COHORT_CLIENT_H

enum darling_lifecycle_endpoint_kind {
	DARLING_LIFECYCLE_ENDPOINT_DARLINGSERVER = 1,
	DARLING_LIFECYCLE_ENDPOINT_SHELLSPAWN = 2,
	DARLING_LIFECYCLE_ENDPOINT_LAUNCHD = 3,
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

#endif
