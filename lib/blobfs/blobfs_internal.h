/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs — persisted on-flash formats.
 *
 * Shared between blobfs.c and the blobfs test suite (which forges a
 * mid-rename crash state to exercise mount recovery). Nothing here is API:
 * the layouts may change freely between releases, and the version bytes
 * below are what makes an old image detectable rather than misread.
 */

#ifndef LIB_BLOBFS_BLOBFS_INTERNAL_H_
#define LIB_BLOBFS_BLOBFS_INTERNAL_H_

#include <stdint.h>

#include <app/lib/blobfs.h> /* BLOBFS_NAME_MAX */

#define BLOBFS_MAGIC          0x42465331u /* 'BFS1' */
#define BLOBFS_META_VERSION   1
#define BLOBFS_DIRENT_VERSION 1

/** dirent type tag. Only files exist in v1; the field carries the
 *  file/directory distinction forward for when directories land.
 */
#define BLOBFS_TYPE_FILE 1

/* Filesystem metadata, bound to the blob the root registry points at.
 *
 * Besides the directory root it carries the *rename intent*: rename is two
 * Map mutations (bind the new name, drop the old), and the intent recorded
 * before the first makes the pair atomic against power loss — mount rolls a
 * half-done rename forward or back before anything else can observe it.
 */
struct blobfs_meta {
	uint32_t magic;
	uint8_t  version;
	uint8_t  rename_in_flight; /* the fields below hold a live intent */
	uint8_t  from_len;
	uint8_t  to_len;
	uint64_t dir_root;
	uint64_t rename_body;      /* body id the in-flight rename moves */
	char     from[BLOBFS_NAME_MAX + 1];
	char     to[BLOBFS_NAME_MAX + 1];
};

/* Directory entry (the Map's value): name -> body blob. */
struct blobfs_dirent {
	uint8_t  type;
	uint8_t  version;
	uint16_t rsvd;
	uint32_t rsvd2;
	uint64_t body_id;
};

#endif /* LIB_BLOBFS_BLOBFS_INTERNAL_H_ */
