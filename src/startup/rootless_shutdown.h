#ifndef DARLING_ROOTLESS_SHUTDOWN_H
#define DARLING_ROOTLESS_SHUTDOWN_H

#include <sys/types.h>

int shutdown_rootless_process_session(pid_t member);

#endif
