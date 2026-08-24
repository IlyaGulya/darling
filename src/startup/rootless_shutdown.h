#ifndef DARLING_ROOTLESS_SHUTDOWN_H
#define DARLING_ROOTLESS_SHUTDOWN_H

#include <sys/types.h>

int shutdown_rootless_process_session(pid_t member);
int shutdown_rootless_lifecycle_controller(int controller_pidfd, int timeout_ms);

#endif
