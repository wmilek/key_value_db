/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_BLOBFS_H_
#define APP_LIB_BLOBFS_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup lib_blobfs blobfs — path/file interface (L3)
 * @ingroup lib
 * @{
 *
 * @brief Path/file access: Map directories + Sequence file bodies.
 *
 * SKELETON — API prototypes only; see doc/layers/l3_interfaces.md. Not yet
 * implemented.
 */

int blobfs_mount(void);
int blobfs_open(const char *path, int flags);
int blobfs_read(int fd, void *out, size_t len, size_t *out_read);
int blobfs_write(int fd, const void *data, size_t len);
int blobfs_close(int fd);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* APP_LIB_BLOBFS_H_ */
