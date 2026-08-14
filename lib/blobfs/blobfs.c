/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs — file interface over a Map container.
 *
 * One Map instance is the directory; every entry maps a file name to a
 * dirent holding the id of the file's *body* blob. The body's payload is
 * the file's bytes and its payload length is the file size, so a write is
 * one blob_db_update() and inherits L1 atomicity untouched.
 *
 *   rootreg[ BLOBFS_KEY ] -> meta blob -> { dir_root }
 *                                              |
 *                            map_ops(dir_root): name -> dirent { body_id }
 *                                                                   |
 *                                              blob_db_get(body_id): bytes
 *
 * v1 is a flat namespace (no directories, no iteration) and file bodies are
 * a single blob payload — see include/app/lib/blobfs.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/blobfs.h>
#include <app/lib/containers/kvhash.h>

LOG_MODULE_REGISTER(blobfs, CONFIG_BLOBDB_BLOBFS_LOG_LEVEL);

#define BLOBFS_MAGIC          0x42465331u /* 'BFS1' */
#define BLOBFS_META_VERSION   1
#define BLOBFS_DIRENT_VERSION 1

/** dirent type tag. Only files exist in v1; the field carries the flat/
 *  hierarchical distinction forward for when directories land.
 */
#define BLOBFS_TYPE_FILE 1

/* Persisted filesystem metadata. Fixed 16-byte layout, no padding. */
struct blobfs_meta {
	uint32_t magic;
	uint8_t  version;
	uint8_t  rsvd[3];
	uint64_t dir_root;
};
BUILD_ASSERT(sizeof(struct blobfs_meta) == 16, "blobfs_meta must be a tight 16 bytes");

/* Persisted directory entry (the Map's value). Fixed 16-byte layout. */
struct blobfs_dirent {
	uint8_t  type;
	uint8_t  version;
	uint16_t rsvd;
	uint32_t rsvd2;
	uint64_t body_id;
};
BUILD_ASSERT(sizeof(struct blobfs_dirent) == 16, "blobfs_dirent must be a tight 16 bytes");

static struct {
	bool     mounted;
	uint64_t dir_root;
} st;

/* One scratch body buffer: read-modify-write of a file body happens here.
 * Single-threaded contract (blob_db v1), same discipline as kvhash's buffers.
 */
static uint8_t body_buf[BLOBFS_FILE_MAX];

/* kvhash is the only Map provider implemented; a Kconfig choice can select
 * among providers here later, exactly as kvdb does.
 */
static inline const struct map_ops *dir_ops(void)
{
	return &kvhash_map_ops;
}

/* Split a v1 path into the single name it can hold. Accepts "name" and
 * "/name" alike; anything nested cannot exist in a flat namespace.
 */
static int path_name(const char *path, const char **out_name, size_t *out_len)
{
	if (path == NULL) {
		return -EINVAL;
	}
	if (path[0] == '/') {
		path++;
	}

	size_t len = strlen(path);

	if (len == 0) {
		return -EISDIR;
	}
	if (memchr(path, '/', len) != NULL) {
		return -ENOENT;
	}
	if (len > BLOBFS_NAME_MAX) {
		return -ENAMETOOLONG;
	}

	*out_name = path;
	*out_len = len;
	return 0;
}

/* Fetch the dirent for a name. */
static int dirent_get(const char *name, size_t nlen, struct blobfs_dirent *de)
{
	size_t got = 0;
	int rc = dir_ops()->get(st.dir_root, name, nlen, de, sizeof(*de), &got);

	if (rc != 0) {
		return rc;
	}
	if (got != sizeof(*de) || de->type != BLOBFS_TYPE_FILE) {
		LOG_ERR("corrupt dirent for '%.*s'", (int)nlen, name);
		return -EIO;
	}
	return 0;
}

/* Resolve a path to its dirent in one step. */
static int resolve(const char *path, const char **name, size_t *nlen,
		   struct blobfs_dirent *de)
{
	if (!st.mounted) {
		return -ENODEV;
	}

	int rc = path_name(path, name, nlen);

	if (rc != 0) {
		return rc;
	}
	return dirent_get(*name, *nlen, de);
}

/* Load a file body into body_buf. The buffer is a full payload wide, so the
 * read never comes up short and *out_len is the file size.
 */
static int body_load(uint64_t body_id, size_t *out_len)
{
	return blob_db_get(body_id, body_buf, sizeof(body_buf), out_len);
}

int blobfs_mount(void)
{
	if (st.mounted) {
		return -EALREADY;
	}

	uint64_t meta_id = 0;
	int rc = rootreg_get_or_create(ROOTREG_KEY(BLOBFS_MAGIC, 0), &meta_id);

	if (rc != 0) {
		return rc;
	}

	struct blobfs_meta meta;
	size_t got = 0;

	rc = blob_db_get(meta_id, &meta, sizeof(meta), &got);
	if (rc == -ENOENT || (rc == 0 && got == 0)) {
		/* Virgin filesystem: build an empty directory and commit it. */
		uint64_t dir_root = blob_db_alloc_id();

		if (dir_root == 0) {
			return -EIO;
		}

		rc = dir_ops()->create(dir_root, NULL);
		if (rc != 0) {
			LOG_ERR("directory create: %d", rc);
			return rc;
		}

		meta = (struct blobfs_meta){
			.magic = BLOBFS_MAGIC,
			.version = BLOBFS_META_VERSION,
			.dir_root = dir_root,
		};

		/* A crash before this commit leaves dir_root orphaned and the
		 * meta unbound, so the next mount simply builds a fresh one.
		 */
		rc = blob_db_update(meta_id, &meta, sizeof(meta));
		if (rc != 0) {
			return rc;
		}
		LOG_DBG("created filesystem, dir_root=%llu",
			(unsigned long long)dir_root);
	} else if (rc != 0) {
		return rc;
	} else if (got < sizeof(meta) || meta.magic != BLOBFS_MAGIC ||
		   meta.version != BLOBFS_META_VERSION) {
		LOG_ERR("bad blobfs metadata (magic %08x version %u)",
			meta.magic, meta.version);
		return -EIO;
	}

	st.dir_root = meta.dir_root;
	st.mounted = true;
	return 0;
}

int blobfs_unmount(void)
{
	st.mounted = false;
	st.dir_root = 0;
	return 0;
}

int blobfs_create(const char *path)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;

	if (!st.mounted) {
		return -ENODEV;
	}

	/* Resolved in two steps on purpose: a rejected path also reports
	 * -ENOENT, and creating must not read that as "the name is free".
	 */
	int rc = path_name(path, &name, &nlen);

	if (rc != 0) {
		return rc;
	}

	rc = dirent_get(name, nlen, &de);
	if (rc == 0) {
		return -EEXIST;
	}
	if (rc != -ENOENT) {
		return rc;
	}

	uint64_t body_id = blob_db_alloc_id();

	if (body_id == 0) {
		return -EIO;
	}

	/* Bind the (empty) body first: until the name is published the blob is
	 * unreachable, so a crash here costs one orphaned blob, nothing more.
	 */
	rc = blob_db_update(body_id, NULL, 0);
	if (rc != 0) {
		return rc;
	}

	de = (struct blobfs_dirent){
		.type = BLOBFS_TYPE_FILE,
		.version = BLOBFS_DIRENT_VERSION,
		.body_id = body_id,
	};

	return dir_ops()->set(st.dir_root, name, nlen, &de, sizeof(de));
}

int blobfs_stat(const char *path, struct blobfs_stat *stat)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;

	if (stat == NULL) {
		return -EINVAL;
	}

	int rc = resolve(path, &name, &nlen, &de);

	if (rc != 0) {
		return rc;
	}

	size_t size = 0;

	rc = body_load(de.body_id, &size);
	if (rc != 0) {
		return rc;
	}

	stat->size = size;
	return 0;
}

int blobfs_read(const char *path, size_t off, void *buf, size_t len,
		size_t *out_read)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;

	if ((buf == NULL && len != 0) || out_read == NULL) {
		return -EINVAL;
	}

	int rc = resolve(path, &name, &nlen, &de);

	if (rc != 0) {
		return rc;
	}

	size_t size = 0;

	rc = body_load(de.body_id, &size);
	if (rc != 0) {
		return rc;
	}

	if (off >= size) {
		*out_read = 0;
		return 0;
	}

	size_t n = MIN(len, size - off);

	memcpy(buf, &body_buf[off], n);
	*out_read = n;
	return 0;
}

int blobfs_write(const char *path, size_t off, const void *buf, size_t len)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;

	if (buf == NULL && len != 0) {
		return -EINVAL;
	}
	if (len == 0) {
		return 0;
	}

	int rc = resolve(path, &name, &nlen, &de);

	if (rc != 0) {
		return rc;
	}
	if (off > BLOBFS_FILE_MAX || len > BLOBFS_FILE_MAX - off) {
		/* Bodies are one blob payload wide; chunked bodies (seq) are
		 * what lifts this cap.
		 */
		return -ENOSPC;
	}

	size_t size = 0;

	rc = body_load(de.body_id, &size);
	if (rc != 0) {
		return rc;
	}

	if (off > size) {
		memset(&body_buf[size], 0, off - size);
	}
	memcpy(&body_buf[off], buf, len);

	return blob_db_update(de.body_id, body_buf, MAX(size, off + len));
}

int blobfs_truncate(const char *path, size_t size)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;

	int rc = resolve(path, &name, &nlen, &de);

	if (rc != 0) {
		return rc;
	}
	if (size > BLOBFS_FILE_MAX) {
		return -ENOSPC;
	}

	size_t cur = 0;

	rc = body_load(de.body_id, &cur);
	if (rc != 0) {
		return rc;
	}

	if (size == cur) {
		return 0;
	}
	if (size > cur) {
		memset(&body_buf[cur], 0, size - cur);
	}

	return blob_db_update(de.body_id, body_buf, size);
}

int blobfs_unlink(const char *path)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;
	int rc = resolve(path, &name, &nlen, &de);

	if (rc != 0) {
		return rc;
	}

	/* Drop the name first — that single Map mutation is what makes the file
	 * disappear atomically. The body is then unreachable, so failing to
	 * delete it costs garbage, never a dangling name.
	 */
	rc = dir_ops()->del(st.dir_root, name, nlen);
	if (rc != 0) {
		return rc;
	}

	rc = blob_db_delete(de.body_id);
	if (rc != 0) {
		LOG_WRN("body %llu orphaned: %d", (unsigned long long)de.body_id, rc);
	}
	return 0;
}

int blobfs_rename(const char *from, const char *to)
{
	const char *from_name, *to_name;
	size_t from_len, to_len;
	struct blobfs_dirent de, existing;
	int rc = resolve(from, &from_name, &from_len, &de);

	if (rc != 0) {
		return rc;
	}

	rc = path_name(to, &to_name, &to_len);
	if (rc != 0) {
		return rc;
	}

	rc = dirent_get(to_name, to_len, &existing);
	if (rc == 0) {
		return -EEXIST;
	}
	if (rc != -ENOENT) {
		return rc;
	}

	/* Publish the new name before dropping the old one: a crash in between
	 * leaves both names on one body — visible, and never a lost file.
	 */
	rc = dir_ops()->set(st.dir_root, to_name, to_len, &de, sizeof(de));
	if (rc != 0) {
		return rc;
	}

	return dir_ops()->del(st.dir_root, from_name, from_len);
}
