/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs — persisted on-flash formats.
 *
 * Shared between blobfs.c and the blobfs test suite (which forges crash
 * states and older formats to exercise mount). Nothing here is API: the
 * layouts may change between releases, and the magic and version bytes
 * below are what makes an old image detectable rather than misread.
 *
 * No persisted layout depends on a build option. In particular
 * CONFIG_BLOBFS_MAX_NAME_LEN only bounds the names a build creates: a store
 * mounts under any setting, whichever setting wrote it.
 */

#ifndef LIB_BLOBFS_BLOBFS_INTERNAL_H_
#define LIB_BLOBFS_BLOBFS_INTERNAL_H_

#include <stdint.h>

#include <app/lib/blobfs.h> /* BLOBFS_NAME_MAX */

#define BLOBFS_MAGIC          0x42465331u /* 'BFS1' */
#define BLOBFS_META_VERSION   3
#define BLOBFS_DIRENT_VERSION 1

/** dirent type tags. A directory's dirent points at the directory's own Map
 *  root instead of a body blob.
 */
#define BLOBFS_TYPE_FILE 1
#define BLOBFS_TYPE_DIR  2

/* Filesystem metadata: blobfs's superblock, the blob the root registry
 * points at (ROOTREG_KEY(BLOBFS_MAGIC, 0)).
 *
 * Fixed size, written once when the filesystem is created and never again:
 * a rename journals in its own intent blob,
 * so day-to-day use does not rewrite the one record everything hangs off.
 *
 * Feature flags gate later additions: a build refuses a store with an
 * incompat bit it does not know (-ENOTSUP) and ignores unknown compat bits.
 * A longer payload than this struct is accepted: a compat addition may
 * append fields, and since no build rewrites a meta, none is lost.
 */
struct blobfs_meta {
	uint32_t magic;            /* BLOBFS_MAGIC */
	uint8_t  version;          /* BLOBFS_META_VERSION */
	uint8_t  name_max;         /* CONFIG_BLOBFS_MAX_NAME_LEN at creation;
				    * informational only
				    */
	uint16_t rsvd;
	uint32_t compat;           /* none defined */
	uint32_t incompat;         /* none defined */
	uint64_t dir_root;         /* the root directory's Map root */
	uint64_t intent_id;        /* the intent blob (struct blobfs_intent) */
};

#define BLOBFS_COMPAT_KNOWN   0u
#define BLOBFS_INCOMPAT_KNOWN 0u

/* Intent blob: what a multi-step mutation records before its first step,
 * so mount can roll a half-done one forward or back. Its own format,
 * separate from the meta, like a container's intent blob (see
 * doc/layers/l2_containers.md §2.2).
 *
 * An empty payload (or an id never bound) means no intent. Clearing writes
 * an empty payload rather than deleting the blob: blob_db never lets a
 * deleted id be bound again.
 *
 * Rename is two Map mutations (bind the new name, drop the old). Its
 * intent names both ends, the directory each lives in, and the body they
 * move. The names follow the header: from[from_len] then to[to_len], no
 * NUL, so the payload is exactly sizeof(header) + from_len + to_len.
 */
#define BLOBFS_INTENT_MAGIC   0x42465349u /* 'BFSI' */
#define BLOBFS_INTENT_VERSION 1
#define BLOBFS_INTENT_RENAME  1

struct blobfs_intent {
	uint32_t magic;            /* BLOBFS_INTENT_MAGIC */
	uint8_t  version;          /* BLOBFS_INTENT_VERSION */
	uint8_t  op;               /* BLOBFS_INTENT_RENAME */
	uint8_t  from_len;         /* 1..255 */
	uint8_t  to_len;           /* 1..255 */
	uint64_t from_dir;         /* directory holding 'from' */
	uint64_t to_dir;           /* directory holding 'to' */
	uint64_t body;             /* body (or directory) id being moved */
	/* uint8_t names[from_len + to_len]; */
};

/* Longest name any intent can carry: its length field is one byte. */
#define BLOBFS_INTENT_NAME_MAX 255u

/* Older metas (version 1: before directories; version 2: rename intent
 * embedded with name fields sized by CONFIG_BLOBFS_MAX_NAME_LEN) are not
 * read: mount refuses them with -ENOTSUP and writes nothing, so the store
 * can still go back to the firmware that wrote it.
 */

/* Directory entry (the Map's value): name -> body blob, or name -> the
 * Map root of a subdirectory.
 */
struct blobfs_dirent {
	uint8_t  type;
	uint8_t  version;
	uint16_t rsvd;
	uint32_t rsvd2;
	uint64_t body_id;
};

#endif /* LIB_BLOBFS_BLOBFS_INTERNAL_H_ */
