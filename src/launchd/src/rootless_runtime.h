/*
 * Copyright (c) 2026 Darling contributors.
 *
 * Rootless launchd owns the writable directory contract required before it
 * can read the bootstrap database or publish its socket.
 */

#ifndef __LAUNCHD_ROOTLESS_RUNTIME_H__
#define __LAUNCHD_ROOTLESS_RUNTIME_H__

int rootless_runtime_prepare(void);

#endif /* __LAUNCHD_ROOTLESS_RUNTIME_H__ */
