/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blobfs — file interface over a Map container.
 *
 * Every directory is one Map instance; each entry maps a name to a dirent.
 * A file's dirent holds the id of the file's *body* blob, whose payload is
 * the file's bytes and whose payload length is the file size: reads and
 * writes go straight to blob_db's pread/pwrite calls, so every mutation is
 * one crash-atomic L1 operation and no data ever stages in RAM here. A
 * subdirectory's dirent holds the Map root of that directory instead, so
 * a path resolves with one Map lookup per component.
 *
 *   rootreg[ BLOBFS_KEY ] -> meta blob -> { dir_root, rename intent }
 *                                              |
 *          map_ops(dir_root):  name -> dirent { FILE, body_id } -> bytes
 *                              name -> dirent { DIR,  map root }
 *                                                        |
 *          map_ops(map root):  name -> dirent ...   (the same shape)
 *
 * The body id is a file's durable identity — names are directory entries
 * pointing at it. Rename re-points names and never touches the id, which is
 * what lets a handle layer above stay bound to the file across renames.
 *
 * Rename itself is two Map mutations (bind the new name, drop the old), so
 * the meta blob records a *rename intent* before the first one: mount finds
 * a half-done rename and rolls it forward or back before anything else can
 * observe it. No API caller ever sees two names on one body. Rename stays
 * within one directory for now (-EXDEV otherwise).
 *
 * Listing a directory is the Map's own enumeration (map_ops.next): the
 * name is the cursor, so a listing holds no state here.
 *
 * File bodies are a single inline blob payload — see
 * include/app/lib/blobfs.h.
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

/* Build an empty directory Map at @p root. The config binds the record
 * geometry to the largest entry blobfs may legally store, so a name length
 * the directory cannot hold fails here, deterministically, instead of at
 * the first unlucky create.
 */
static int dir_create(uint64_t root)
{
	struct map_config cfg = {
		.max_entry_bytes = BLOBFS_NAME_MAX + sizeof(struct blobfs_dirent),
	};

	return dir_ops()->create(root, &cfg);
}

/* Where a path leads: the directory that holds its last component, and that
 * component. A path naming the root directory has no last component.
 */
struct where {
	uint64_t    dir;      /* Map root of the directory holding name */
	const char *name;     /* last component; not NUL-terminated */
	size_t      nlen;     /* 0: the path is the root directory itself */
	bool        want_dir; /* written with a trailing '/' */
};

/* The component of *p that starts at the next non-'/' byte, advancing *p
 * past it. Repeated slashes collapse. Returns its length: 0 at the end.
 */
static size_t component(const char **p, const char **out)
{
	const char *s = *p;

	while (*s == '/') {
		s++;
	}

	const char *e = s;

	while (*e != '\0' && *e != '/') {
		e++;
	}

	*out = s;
	*p = e;
	return (size_t)(e - s);
}

/* A component blobfs can store. The POSIX special entries are refused
 * outright: they would be unaddressable through path-normalizing clients.
 */
static int check_name(const char *name, size_t len)
{
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.')) {
		return -EINVAL;
	}
	if (len > BLOBFS_NAME_MAX) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/* Fetch and validate the dirent for a name in directory @p dir. */
static int dirent_get(uint64_t dir, const char *name, size_t nlen,
		      struct blobfs_dirent *de)
{
	size_t got = 0;
	int rc = dir_ops()->get(dir, name, nlen, de, sizeof(*de), &got);

	if (rc != 0) {
		return rc;
	}
	if (got != sizeof(*de) ||
	    (de->type != BLOBFS_TYPE_FILE && de->type != BLOBFS_TYPE_DIR) ||
	    de->version != BLOBFS_DIRENT_VERSION) {
		LOG_ERR("corrupt dirent for '%.*s'", (int)nlen, name);
		return -EIO;
	}
	return 0;
}

/* Walk @p path to the directory holding its last component, one Map lookup
 * per directory on the way. Accepts a leading '/' or none. Only the
 * directories on the way must exist; the last component is not looked up.
 *
 * -ENOENT: a directory on the way is missing. -ENOTDIR: one is a file.
 */
static int walk(const char *path, struct where *w)
{
	if (path == NULL) {
		return -EINVAL;
	}
	if (!st.mounted) {
		return -ENODEV;
	}

	uint64_t dir = st.dir_root;
	const char *name;
	size_t nlen = component(&path, &name);

	while (nlen != 0) {
		int rc = check_name(name, nlen);

		if (rc != 0) {
			return rc;
		}

		const char *next;
		size_t next_len = component(&path, &next);

		if (next_len == 0) {
			break;
		}

		struct blobfs_dirent de;

		rc = dirent_get(dir, name, nlen, &de);
		if (rc != 0) {
			return rc;
		}
		if (de.type != BLOBFS_TYPE_DIR) {
			return -ENOTDIR;
		}
		dir = de.body_id;
		name = next;
		nlen = next_len;
	}

	w->dir = dir;
	w->name = name;
	w->nlen = nlen;
	w->want_dir = (nlen != 0 && name[nlen] == '/');
	return 0;
}

/* Walk @p path and fetch the dirent of its last component. The root has no
 * dirent and reports -EISDIR, which callers that accept the root test for.
 * A trailing '/' on a file reports -ENOTDIR.
 */
static int resolve(const char *path, struct where *w, struct blobfs_dirent *de)
{
	int rc = walk(path, w);

	if (rc != 0) {
		return rc;
	}
	if (w->nlen == 0) {
		return -EISDIR;
	}

	rc = dirent_get(w->dir, w->name, w->nlen, de);
	if (rc != 0) {
		return rc;
	}
	if (w->want_dir && de->type != BLOBFS_TYPE_DIR) {
		return -ENOTDIR;
	}
	return 0;
}

/* Commit the meta blob — one atomic blob_db_update. */
static int meta_write(const struct blobfs_meta *meta)
{
	return blob_db_update(st.meta_id, meta, sizeof(*meta));
}

static void intent_clear(struct blobfs_meta *meta)
{
	meta->rename_in_flight = 0;
	meta->from_len = 0;
	meta->to_len = 0;
	meta->rename_body = 0;
	meta->from_dir = 0;
	meta->to_dir = 0;
	memset(meta->from, 0, sizeof(meta->from));
	memset(meta->to, 0, sizeof(meta->to));
}

/* Finish or abandon a rename the previous boot left half done. The intent
 * names both ends, the directory each lives in, and the body they move, so
 * every crash point resolves:
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
	    meta->to_len == 0 || meta->to_len > BLOBFS_NAME_MAX ||
	    meta->from_dir == 0 || meta->to_dir == 0) {
		LOG_ERR("corrupt rename intent (from_len=%u to_len=%u)",
			meta->from_len, meta->to_len);
		return -EIO;
	}

	int rc = dirent_get(meta->to_dir, meta->to, meta->to_len, &de);

	if (rc == 0 && de.body_id == meta->rename_body) {
		/* Visible: make sure the old name is gone. */
		struct blobfs_dirent from_de;

		rc = dirent_get(meta->from_dir, meta->from, meta->from_len,
				&from_de);
		if (rc == 0 && from_de.body_id == meta->rename_body) {
			rc = dir_ops()->del(meta->from_dir, meta->from,
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

	intent_clear(meta);
	return meta_write(meta);
}

/* Read the meta blob, in either version, as version 2. A version 1 meta
 * (blobfs before directories) differs only in its intent having no
 * directory fields: there was only the root, so both ends live there.
 * Nothing is written here; the next meta write stores version 2.
 */
static int meta_read(uint64_t meta_id, struct blobfs_meta *meta)
{
	union {
		struct blobfs_meta    v2;
		struct blobfs_meta_v1 v1;
	} buf;
	size_t got = 0;
	int rc = blob_db_get(meta_id, &buf, sizeof(buf), &got);

	if (rc != 0) {
		return rc;
	}
	if (got == 0) {
		return -ENOENT;
	}

	if (got == sizeof(buf.v2) && buf.v2.magic == BLOBFS_MAGIC &&
	    buf.v2.version == BLOBFS_META_VERSION) {
		*meta = buf.v2;
		return 0;
	}
	if (got == sizeof(buf.v1) && buf.v1.magic == BLOBFS_MAGIC &&
	    buf.v1.version == BLOBFS_META_VERSION_1) {
		*meta = (struct blobfs_meta){
			.magic = BLOBFS_MAGIC,
			.version = BLOBFS_META_VERSION,
			.rename_in_flight = buf.v1.rename_in_flight,
			.from_len = buf.v1.from_len,
			.to_len = buf.v1.to_len,
			.dir_root = buf.v1.dir_root,
			.rename_body = buf.v1.rename_body,
			.from_dir = buf.v1.dir_root,
			.to_dir = buf.v1.dir_root,
		};
		memcpy(meta->from, buf.v1.from, sizeof(meta->from));
		memcpy(meta->to, buf.v1.to, sizeof(meta->to));
		return 0;
	}

	LOG_ERR("bad blobfs metadata (magic %08x version %u len %zu)",
		buf.v2.magic, buf.v2.version, got);
	return -EIO;
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

	rc = meta_read(meta_id, &meta);
	if (rc == -ENOENT) {
		/* Virgin filesystem: build an empty root directory and commit
		 * it.
		 */
		uint64_t dir_root = blob_db_alloc_id();

		if (dir_root == 0) {
			return -EIO;
		}

		rc = dir_create(dir_root);
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

/* Walk to a name that must not exist yet: the common half of create and
 * mkdir. Creating must not read a rejected path as "the name is free",
 * which is why the walk and the lookup stay separate steps.
 */
static int walk_to_free_name(const char *path, struct where *w)
{
	struct blobfs_dirent de;
	int rc = walk(path, w);

	if (rc != 0) {
		return rc;
	}
	if (w->nlen == 0) {
		return -EEXIST; /* the root directory */
	}

	rc = dirent_get(w->dir, w->name, w->nlen, &de);
	if (rc == 0) {
		return -EEXIST;
	}
	return (rc == -ENOENT) ? 0 : rc;
}

int blobfs_create(const char *path)
{
	struct where w = { 0 };
	int rc = walk_to_free_name(path, &w);

	if (rc == -EEXIST && w.nlen == 0) {
		return -EISDIR;
	}
	if (rc != 0) {
		return rc;
	}
	if (w.want_dir) {
		return -EISDIR; /* "name/" can only be a directory */
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

	struct blobfs_dirent de = {
		.type = BLOBFS_TYPE_FILE,
		.version = BLOBFS_DIRENT_VERSION,
		.body_id = body_id,
	};

	return dir_ops()->set(w.dir, w.name, w.nlen, &de, sizeof(de));
}

int blobfs_mkdir(const char *path)
{
	struct where w = { 0 };
	int rc = walk_to_free_name(path, &w);

	if (rc != 0) {
		return rc;
	}

	uint64_t dir_id = blob_db_alloc_id();

	if (dir_id == 0) {
		return -EIO;
	}

	/* Build the directory first, exactly like a file's body: until the
	 * name is published it is unreachable, so a crash here costs one
	 * orphaned (empty) Map, nothing more.
	 */
	rc = dir_create(dir_id);
	if (rc != 0) {
		return rc;
	}

	struct blobfs_dirent de = {
		.type = BLOBFS_TYPE_DIR,
		.version = BLOBFS_DIRENT_VERSION,
		.body_id = dir_id,
	};

	rc = dir_ops()->set(w.dir, w.name, w.nlen, &de, sizeof(de));
	if (rc != 0) {
		(void)dir_ops()->destroy(dir_id);
	}
	return rc;
}

int blobfs_lookup(const char *path, uint64_t *body_id)
{
	struct where w = { 0 };
	struct blobfs_dirent de;

	if (body_id == NULL) {
		return -EINVAL;
	}

	int rc = resolve(path, &w, &de);

	if (rc != 0) {
		return rc;
	}
	if (de.type == BLOBFS_TYPE_DIR) {
		return -EISDIR;
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

/* Fill @p stat from a dirent: a directory has no size. */
static int stat_of(const struct blobfs_dirent *de, struct blobfs_stat *stat)
{
	if (de->type == BLOBFS_TYPE_DIR) {
		*stat = (struct blobfs_stat){ .type = BLOBFS_ENTRY_DIR };
		return 0;
	}

	stat->type = BLOBFS_ENTRY_FILE;
	return blob_db_size(de->body_id, &stat->size);
}

int blobfs_stat(const char *path, struct blobfs_stat *stat)
{
	struct where w = { 0 };
	struct blobfs_dirent de;

	if (stat == NULL) {
		return -EINVAL;
	}

	int rc = resolve(path, &w, &de);

	if (rc == -EISDIR && w.nlen == 0) {
		*stat = (struct blobfs_stat){ .type = BLOBFS_ENTRY_DIR };
		return 0;
	}
	if (rc != 0) {
		return rc;
	}
	return stat_of(&de, stat);
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

/* Whether the directory Map at @p dir holds no entry: one enumeration step
 * from the start, with no buffers, answers it without reading the rest.
 */
static int dir_is_empty(uint64_t dir)
{
	size_t klen = 0;
	int rc = dir_ops()->next(dir, NULL, 0, NULL, 0, &klen,
				 NULL, 0, NULL);

	if (rc == -ENODATA) {
		return 1;
	}
	if (rc == 0 || rc == -ENOMEM) {
		return 0;
	}
	return rc;
}

int blobfs_unlink(const char *path)
{
	struct where w = { 0 };
	struct blobfs_dirent de;
	int rc = resolve(path, &w, &de);

	if (rc == -EISDIR && w.nlen == 0) {
		return -EBUSY; /* the root directory */
	}
	if (rc != 0) {
		return rc;
	}

	if (de.type == BLOBFS_TYPE_DIR) {
		rc = dir_is_empty(de.body_id);
		if (rc < 0) {
			return rc;
		}
		if (rc == 0) {
			return -ENOTEMPTY;
		}
	}

	/* Drop the name first — that single Map mutation is what makes the
	 * entry disappear atomically. The body (or directory Map) is then
	 * unreachable, so failing to release it costs garbage, never a
	 * dangling name. (Aliased bodies cannot reach this point: the rename
	 * intent keeps a half-renamed pair invisible until mount has resolved
	 * it.)
	 */
	rc = dir_ops()->del(w.dir, w.name, w.nlen);
	if (rc != 0) {
		return rc;
	}

	if (de.type == BLOBFS_TYPE_DIR) {
		rc = dir_ops()->destroy(de.body_id);
	} else {
		rc = blob_db_delete(de.body_id);
	}
	if (rc != 0) {
		LOG_WRN("%s %llu orphaned: %d",
			(de.type == BLOBFS_TYPE_DIR) ? "directory" : "body",
			(unsigned long long)de.body_id, rc);
	}
	return 0;
}

int blobfs_rename(const char *from, const char *to)
{
	struct where wf = { 0 }, wt = { 0 };
	struct blobfs_dirent de, existing;
	int rc = resolve(from, &wf, &de);

	if (rc == -EISDIR && wf.nlen == 0) {
		return -EBUSY; /* the root directory */
	}
	if (rc != 0) {
		return rc;
	}

	rc = walk(to, &wt);
	if (rc != 0) {
		return rc;
	}
	if (wt.nlen == 0) {
		return -EBUSY;
	}
	if (wt.want_dir && de.type != BLOBFS_TYPE_DIR) {
		return -ENOTDIR;
	}
	if (wt.dir != wf.dir) {
		/* Both ends must share a directory until the intent's
		 * cross-directory recovery is wired up.
		 */
		return -EXDEV;
	}

	rc = dirent_get(wt.dir, wt.name, wt.nlen, &existing);
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
		.from_len = (uint8_t)wf.nlen,
		.to_len = (uint8_t)wt.nlen,
		.dir_root = st.dir_root,
		.rename_body = de.body_id,
		.from_dir = wf.dir,
		.to_dir = wt.dir,
	};
	memcpy(meta.from, wf.name, wf.nlen);
	memcpy(meta.to, wt.name, wt.nlen);

	rc = meta_write(&meta);
	if (rc != 0) {
		return rc;
	}

	rc = dir_ops()->set(wt.dir, wt.name, wt.nlen, &de, sizeof(de));
	if (rc != 0) {
		/* Nothing became visible; withdraw the intent. If even that
		 * fails, mount will roll the intent back for us.
		 */
		intent_clear(&meta);
		(void)meta_write(&meta);
		return rc;
	}

	rc = dir_ops()->del(wf.dir, wf.name, wf.nlen);
	if (rc != 0) {
		/* Both names exist but the intent still stands: the next
		 * mount finishes the rename. Report the failure regardless.
		 */
		return rc;
	}

	intent_clear(&meta);
	return meta_write(&meta);
}

int blobfs_opendir(const char *path, uint64_t *dir_id)
{
	struct where w = { 0 };
	struct blobfs_dirent de;

	if (dir_id == NULL) {
		return -EINVAL;
	}

	int rc = resolve(path, &w, &de);

	if (rc == -EISDIR && w.nlen == 0) {
		*dir_id = st.dir_root;
		return 0;
	}
	if (rc != 0) {
		return rc;
	}
	if (de.type != BLOBFS_TYPE_DIR) {
		return -ENOTDIR;
	}

	*dir_id = de.body_id;
	return 0;
}

int blobfs_next_id(uint64_t dir_id, const char *after,
		   char name[BLOBFS_NAME_MAX + 1], struct blobfs_stat *stat)
{
	if (after == NULL || name == NULL) {
		return -EINVAL;
	}
	if (!st.mounted) {
		return -ENODEV;
	}

	size_t alen = strlen(after);

	if (alen > BLOBFS_NAME_MAX) {
		return -ENAMETOOLONG;
	}

	struct blobfs_dirent de;
	size_t nlen = 0;
	size_t got = 0;
	int rc = dir_ops()->next(dir_id, after, alen,
				 name, BLOBFS_NAME_MAX, &nlen,
				 &de, sizeof(de), &got);

	if (rc == -ENOMEM) {
		/* Every name was checked against BLOBFS_NAME_MAX when it was
		 * stored: a longer one is not something blobfs wrote.
		 */
		LOG_ERR("directory walk after '%s': entry too large", after);
		return -EIO;
	}
	if (rc != 0) {
		/* -ENOENT: the directory was removed. */
		return rc;
	}
	if (got != sizeof(de) ||
	    (de.type != BLOBFS_TYPE_FILE && de.type != BLOBFS_TYPE_DIR) ||
	    de.version != BLOBFS_DIRENT_VERSION) {
		LOG_ERR("corrupt dirent for '%.*s'", (int)nlen, name);
		return -EIO;
	}
	name[nlen] = '\0';

	if (stat == NULL) {
		return 0;
	}

	/* A name is dropped before its body is deleted, so a listed file
	 * always has a body.
	 */
	rc = stat_of(&de, stat);
	if (rc == -ENOENT) {
		LOG_ERR("'%s' names a missing body %llu", name,
			(unsigned long long)de.body_id);
		return -EIO;
	}
	return rc;
}
