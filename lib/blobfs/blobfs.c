/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs — file interface over a Map container.
 *
 * One Map instance is the directory; every entry maps a file name to a
 * dirent holding the id of the file's *body* blob. The body's payload is
 * the file's bytes and its payload length is the file size: reads and
 * writes go straight to blob_db's pread/pwrite calls, so every mutation is
 * one crash-atomic L1 operation and no data ever stages in RAM here.
 *
 *   rootreg[ BLOBFS_KEY ] -> meta blob -> { dir_root, rename intent }
 *                                              |
 *                            map_ops(dir_root): name -> dirent { body_id }
 *                                                                   |
 *                                       blob_db_read/write(body_id): bytes
 *
 * The body id is a file's durable identity — names are directory entries
 * pointing at it. Rename re-points names and never touches the id, which is
 * what lets a handle layer above stay bound to the file across renames.
 *
 * Rename itself is two Map mutations (bind the new name, drop the old), so
 * the meta blob records a *rename intent* before the first one: mount finds
 * a half-done rename and rolls it forward or back before anything else can
 * observe it. No API caller ever sees two names on one body.
 *
 * v1 is a flat namespace (no directories, no iteration) and file bodies are
 * a single inline blob payload — see include/app/lib/blobfs.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/blobfs.h>
#include <app/lib/containers/kvhash.h>

#include "blobfs_internal.h"

LOG_MODULE_REGISTER(blobfs, CONFIG_BLOBDB_BLOBFS_LOG_LEVEL);

BUILD_ASSERT(sizeof(struct blobfs_meta) <= CONFIG_BLOB_DB_MAX_PAYLOAD_LEN,
	     "blobfs_meta (which scales with CONFIG_BLOBFS_MAX_NAME_LEN) must "
	     "fit one blob payload; raise CONFIG_BLOB_DB_MAX_PAYLOAD_LEN or "
	     "lower CONFIG_BLOBFS_MAX_NAME_LEN");
BUILD_ASSERT(sizeof(struct blobfs_dirent) == 16,
	     "blobfs_dirent must be a tight 16 bytes");

static struct {
	bool     mounted;
	uint64_t meta_id;
	uint64_t dir_root;
} st;

/* kvhash is the only Map provider implemented; a Kconfig choice can select
 * among providers here later, exactly as kvdb does.
 */
static inline const struct map_ops *dir_ops(void)
{
	return &kvhash_map_ops;
}

/* Split a v1 path into the single name it can hold. Accepts "name" and
 * "/name" alike; anything nested cannot exist in a flat namespace, and the
 * POSIX special entries would be unaddressable through path-normalizing
 * clients, so they are refused outright.
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
	if (strcmp(path, ".") == 0 || strcmp(path, "..") == 0) {
		return -EINVAL;
	}
	if (len > BLOBFS_NAME_MAX) {
		return -ENAMETOOLONG;
	}

	*out_name = path;
	*out_len = len;
	return 0;
}

/* Fetch and validate the dirent for a name. */
static int dirent_get(const char *name, size_t nlen, struct blobfs_dirent *de)
{
	size_t got = 0;
	int rc = dir_ops()->get(st.dir_root, name, nlen, de, sizeof(*de), &got);

	if (rc != 0) {
		return rc;
	}
	if (got != sizeof(*de) || de->type != BLOBFS_TYPE_FILE ||
	    de->version != BLOBFS_DIRENT_VERSION) {
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

/* Commit the meta blob — one atomic blob_db_update. */
static int meta_write(const struct blobfs_meta *meta)
{
	return blob_db_update(st.meta_id, meta, sizeof(*meta));
}

/* Finish or abandon a rename the previous boot left half done. The intent
 * names both ends and the body they move, so every crash point resolves:
 *
 *   'to' absent               -> the rename never became visible: roll back
 *                                by clearing the intent.
 *   'to' bound to the body    -> the rename is visible: roll forward by
 *                                dropping a 'from' that still aliases the
 *                                body, then clear the intent.
 *   'to' bound elsewhere      -> not a state this code can leave behind
 *                                (rename refuses an occupied destination);
 *                                keep both names, clear the intent, and say
 *                                so.
 */
static int rename_recover(struct blobfs_meta *meta)
{
	struct blobfs_dirent de;

	if (meta->from_len == 0 || meta->from_len > BLOBFS_NAME_MAX ||
	    meta->to_len == 0 || meta->to_len > BLOBFS_NAME_MAX) {
		LOG_ERR("corrupt rename intent (from_len=%u to_len=%u)",
			meta->from_len, meta->to_len);
		return -EIO;
	}

	int rc = dirent_get(meta->to, meta->to_len, &de);

	if (rc == 0 && de.body_id == meta->rename_body) {
		/* Visible: make sure the old name is gone. */
		struct blobfs_dirent from_de;

		rc = dirent_get(meta->from, meta->from_len, &from_de);
		if (rc == 0 && from_de.body_id == meta->rename_body) {
			rc = dir_ops()->del(st.dir_root, meta->from,
					    meta->from_len);
			if (rc != 0) {
				return rc;
			}
		} else if (rc != 0 && rc != -ENOENT) {
			return rc;
		}
		LOG_INF("recovered rename '%s' -> '%s' (rolled forward)",
			meta->from, meta->to);
	} else if (rc == -ENOENT) {
		LOG_INF("recovered rename '%s' -> '%s' (rolled back)",
			meta->from, meta->to);
	} else if (rc != 0) {
		return rc;
	} else {
		LOG_ERR("rename intent found '%s' bound to another body; "
			"clearing intent, keeping both names", meta->to);
	}

	meta->rename_in_flight = 0;
	meta->from_len = 0;
	meta->to_len = 0;
	meta->rename_body = 0;
	memset(meta->from, 0, sizeof(meta->from));
	memset(meta->to, 0, sizeof(meta->to));
	return meta_write(meta);
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
		/* Virgin filesystem: build an empty directory and commit it.
		 * The config binds the directory's record geometry to the
		 * largest entry blobfs may legally store, so a name length
		 * the directory cannot hold fails here, deterministically,
		 * instead of at the first unlucky create.
		 */
		uint64_t dir_root = blob_db_alloc_id();

		if (dir_root == 0) {
			return -EIO;
		}

		struct map_config cfg = {
			.max_entry_bytes = BLOBFS_NAME_MAX +
					   sizeof(struct blobfs_dirent),
		};

		rc = dir_ops()->create(dir_root, &cfg);
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
	} else if (got != sizeof(meta) || meta.magic != BLOBFS_MAGIC ||
		   meta.version != BLOBFS_META_VERSION) {
		LOG_ERR("bad blobfs metadata (magic %08x version %u len %zu)",
			meta.magic, meta.version, got);
		return -EIO;
	}

	st.meta_id = meta_id;
	st.dir_root = meta.dir_root;

	if (meta.rename_in_flight) {
		rc = rename_recover(&meta);
		if (rc != 0) {
			st.meta_id = 0;
			st.dir_root = 0;
			return rc;
		}
	}

	st.mounted = true;
	return 0;
}

int blobfs_unmount(void)
{
	st.mounted = false;
	st.meta_id = 0;
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

int blobfs_lookup(const char *path, uint64_t *body_id)
{
	const char *name;
	size_t nlen;
	struct blobfs_dirent de;

	if (body_id == NULL) {
		return -EINVAL;
	}

	int rc = resolve(path, &name, &nlen, &de);

	if (rc != 0) {
		return rc;
	}

	*body_id = de.body_id;
	return 0;
}

int blobfs_size_id(uint64_t body_id, size_t *size)
{
	if (size == NULL) {
		return -EINVAL;
	}
	return blob_db_size(body_id, size);
}

int blobfs_read_id(uint64_t body_id, size_t off, void *buf, size_t len,
		   size_t *out_read)
{
	if ((buf == NULL && len != 0) || out_read == NULL) {
		return -EINVAL;
	}
	/* blob_db_read is pread-style: short read at EOF, 0 at or past it. */
	return blob_db_read(body_id, off, buf, len, out_read);
}

int blobfs_write_id(uint64_t body_id, size_t off, const void *buf, size_t len)
{
	if (buf == NULL && len != 0) {
		return -EINVAL;
	}
	if (len == 0) {
		/* Nothing to write, but the file must exist for the caller's
		 * "0 == success" to mean anything.
		 */
		size_t size;

		return blob_db_size(body_id, &size);
	}
	if (off > BLOBFS_FILE_MAX || len > BLOBFS_FILE_MAX - off) {
		/* Bodies are one inline blob payload; L1's segmented objects
		 * are what will lift this cap.
		 */
		return -ENOSPC;
	}

	/* One crash-atomic L1 call: extends as needed, zero-fills any gap. */
	return blob_db_write(body_id, off, buf, len);
}

int blobfs_truncate_id(uint64_t body_id, size_t size)
{
	if (size > BLOBFS_FILE_MAX) {
		return -ENOSPC;
	}

	size_t cur = 0;
	int rc = blob_db_size(body_id, &cur);

	if (rc != 0) {
		return rc;
	}
	if (size == cur) {
		return 0;
	}

	if (size > cur) {
		/* Grow: one zero byte at the new last position extends the
		 * payload, and blob_db_write zero-fills the gap before it.
		 */
		const uint8_t zero = 0;

		return blob_db_write(body_id, size - 1, &zero, 1);
	}

	/* Shrink: the one operation with no partial-write shape — the payload
	 * must be re-bound at the new length, so the surviving prefix passes
	 * through RAM once. Bounded by the inline-payload cap, like the
	 * bucket buffers blob_db itself stacks.
	 */
	uint8_t buf[BLOBFS_FILE_MAX];
	size_t rd = 0;

	rc = blob_db_read(body_id, 0, buf, size, &rd);
	if (rc != 0) {
		return rc;
	}
	if (rd != size) {
		return -EIO;
	}

	return blob_db_update(body_id, buf, size);
}

int blobfs_stat(const char *path, struct blobfs_stat *stat)
{
	uint64_t body_id;

	if (stat == NULL) {
		return -EINVAL;
	}

	int rc = blobfs_lookup(path, &body_id);

	if (rc != 0) {
		return rc;
	}
	return blobfs_size_id(body_id, &stat->size);
}

int blobfs_read(const char *path, size_t off, void *buf, size_t len,
		size_t *out_read)
{
	uint64_t body_id;
	int rc = blobfs_lookup(path, &body_id);

	if (rc != 0) {
		return rc;
	}
	return blobfs_read_id(body_id, off, buf, len, out_read);
}

int blobfs_write(const char *path, size_t off, const void *buf, size_t len)
{
	uint64_t body_id;
	int rc = blobfs_lookup(path, &body_id);

	if (rc != 0) {
		return rc;
	}
	return blobfs_write_id(body_id, off, buf, len);
}

int blobfs_truncate(const char *path, size_t size)
{
	uint64_t body_id;
	int rc = blobfs_lookup(path, &body_id);

	if (rc != 0) {
		return rc;
	}
	return blobfs_truncate_id(body_id, size);
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
	 * delete it costs garbage, never a dangling name. (Aliased bodies
	 * cannot reach this point: the rename intent keeps a half-renamed pair
	 * invisible until mount has resolved it.)
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

	/* Rename is two Map mutations; the intent committed first is what
	 * makes the pair atomic. Whatever point this crashes at, mount rolls
	 * the rename forward (intent + new name visible) or back (intent
	 * only), so no caller ever observes two names on one body.
	 */
	struct blobfs_meta meta = {
		.magic = BLOBFS_MAGIC,
		.version = BLOBFS_META_VERSION,
		.rename_in_flight = 1,
		.from_len = (uint8_t)from_len,
		.to_len = (uint8_t)to_len,
		.dir_root = st.dir_root,
		.rename_body = de.body_id,
	};
	memcpy(meta.from, from_name, from_len);
	memcpy(meta.to, to_name, to_len);

	rc = meta_write(&meta);
	if (rc != 0) {
		return rc;
	}

	rc = dir_ops()->set(st.dir_root, to_name, to_len, &de, sizeof(de));
	if (rc != 0) {
		/* Nothing became visible; withdraw the intent. If even that
		 * fails, mount will roll the intent back for us.
		 */
		meta.rename_in_flight = 0;
		(void)meta_write(&meta);
		return rc;
	}

	rc = dir_ops()->del(st.dir_root, from_name, from_len);
	if (rc != 0) {
		/* Both names exist but the intent still stands: the next
		 * mount finishes the rename. Report the failure regardless.
		 */
		return rc;
	}

	meta.rename_in_flight = 0;
	meta.from_len = 0;
	meta.to_len = 0;
	meta.rename_body = 0;
	memset(meta.from, 0, sizeof(meta.from));
	memset(meta.to, 0, sizeof(meta.to));
	return meta_write(&meta);
}
