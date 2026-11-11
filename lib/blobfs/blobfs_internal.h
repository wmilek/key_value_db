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

#include <zephyr/sys/util.h>  /* ROUND_UP */
#include <zephyr/toolchain.h> /* __packed */

#include <app/lib/blobfs.h> /* BLOBFS_NAME_MAX */

#define BLOBFS_MAGIC          0x42465331u /* 'BFS1' */
#define BLOBFS_META_VERSION   2
#define BLOBFS_DIRENT_VERSION 1

/** dirent type tags. A directory's dirent points at the directory's own Map
 *  root instead of a body blob.
 */
#define BLOBFS_TYPE_FILE 1
#define BLOBFS_TYPE_DIR  2

/* Filesystem metadata, bound to the blob the root registry points at.
 *
 * Besides the root directory it carries the *rename intent*: rename is two
 * Map mutations (bind the new name, drop the old), and the intent recorded
 * before the first makes the pair atomic against power loss — mount rolls a
 * half-done rename forward or back before anything else can observe it.
 *
 * The intent names the directory each end lives in. Today both are always
 * the same directory (rename across directories reports -EXDEV); the two
 * fields are what will let a cross-directory rename use this format as is.
 *
 * Both meta versions are __packed, so the record is the same bytes whatever
 * the writing build's ABI. Every field before the names is naturally
 * aligned, so packing moves no offset; it only drops the trailing padding
 * an unpacked struct got from its u64 alignment. Builds before this wrote
 * that padding — the record rounded up to 8 bytes on ARM and x86-64, to 4
 * on native_sim (x86-32) — and mount still accepts all three lengths, see
 * BLOBFS_META_LEN_OK().
 */
struct __packed blobfs_meta {
	uint32_t magic;
	uint8_t  version;
	uint8_t  rename_in_flight; /* the fields below hold a live intent */
	uint8_t  from_len;
	uint8_t  to_len;
	uint64_t dir_root;         /* the root directory's Map root */
	uint64_t rename_body;      /* body (or directory) id being moved */
	uint64_t from_dir;         /* directory holding 'from' */
	uint64_t to_dir;           /* directory holding 'to' */
	char     from[BLOBFS_NAME_MAX + 1];
	char     to[BLOBFS_NAME_MAX + 1];
};

/* Version 1 of the meta, written by blobfs before directories existed: the
 * intent had no directory fields, because there was only the root. Mount
 * still reads it; every write is version 2.
 */
#define BLOBFS_META_VERSION_1 1

struct __packed blobfs_meta_v1 {
	uint32_t magic;
	uint8_t  version;
	uint8_t  rename_in_flight;
	uint8_t  from_len;
	uint8_t  to_len;
	uint64_t dir_root;
	uint64_t rename_body;
	char     from[BLOBFS_NAME_MAX + 1];
	char     to[BLOBFS_NAME_MAX + 1];
};

/* Whether a meta record of @p got bytes is a struct of @p packed bytes, as
 * this build writes it or as an earlier build padded it to 4 or 8 bytes.
 * The padding bytes carry nothing and are not checked.
 */
#define BLOBFS_META_LEN_OK(got, packed)                                       \
	((got) == (packed) || (got) == ROUND_UP((packed), 4) ||               \
	 (got) == ROUND_UP((packed), 8))

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
