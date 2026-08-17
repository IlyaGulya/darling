#ifndef __LAUNCH_CLIENT_INIT_ERROR_H__
#define __LAUNCH_CLIENT_INIT_ERROR_H__

#include <stdbool.h>
#include <pthread.h>

int launch_client_getsocket_errno(int result, bool path_nonempty);
int launch_client_preserve_init_errno(int current, int candidate);
bool launch_client_init_allows_connect(int init_errno);
int launch_client_require_connection(
    pthread_once_t *once,
    void (*initialize)(void),
    const int *init_errno,
    bool (*is_connected)(void *),
    void *context);

#endif
