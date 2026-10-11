/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db shell — read-only inspection of the mounted store.
 *
 * Parsing and printing only, over the public introspection API
 * (<app/lib/blob_db_inspect.h>, CONFIG_BLOB_DB_INSPECT). No command
 * writes to flash. Every blob_db call takes the blob_db lock, so commands
 * are safe while the application uses the store; a command that reads
 * several buckets sees each as it is then, not one snapshot.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_inspect.h>
#include <app/lib/blob_db_iostats.h>

/* Fullest buckets listed by `stats` when no count is given, and the most
 * it will track. */
#define TOP_DEFAULT  5
#define TOP_MAX      16

/* Bins of the bucket fill histogram, each 100 / HIST_BINS percent wide. */
#define HIST_BINS    10
#define HIST_BAR     40

/* Bytes per `dump` read; one hexdump line is 16. */
#define DUMP_CHUNK   64
#define DUMP_DEFAULT 256

static int need_info(const struct shell *sh, struct blob_db_inspect_info *info)
{
	int rc = blob_db_inspect_info_get(info);

	if (rc == -ENODEV) {
		shell_error(sh, "blob_db not mounted");
	} else if (rc < 0) {
		shell_error(sh, "info: %d", rc);
	}
	return rc;
}

static int parse_u64(const struct shell *sh, const char *s, uint64_t *out)
{
	char *end;

	errno = 0;
	*out = strtoull(s, &end, 0);
	if (errno || end == s || *end != '\0') {
		shell_error(sh, "not a number: %s", s);
		return -EINVAL;
	}
	return 0;
}

/* Integer per-mille, so the output needs no floating-point printf. */
static uint32_t permille(uint64_t part, uint64_t whole)
{
	return whole ? (uint32_t)((part * 1000 + whole / 2) / whole) : 0;
}

#define PM_FMT     "%3u.%u%%"
#define PM_ARG(x)  (unsigned int)((x) / 10), (unsigned int)((x) % 10)

static const char *state_str(const struct blob_db_inspect_slot *s)
{
	switch (s->state) {
	case BLOB_DB_INSPECT_LIVE:
		return s->segment ? "live-seg" : (s->index ? "live-idx" : "live");
	case BLOB_DB_INSPECT_SUPERSEDED:
		return "stale";
	case BLOB_DB_INSPECT_TOMBSTONE:
		return "tomb";
	}
	return "?";
}

/* blob_db info ----------------------------------------------------------- */

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (need_info(sh, &in) < 0) {
		return -ENODEV;
	}

	shell_print(sh, "backend       : %s",
		    IS_ENABLED(CONFIG_BLOB_DB_BACKEND_UBI) ? "UBI volume"
							    : "flash_area");
	shell_print(sh, "format        : %u.%u", in.format_major,
		    in.format_minor);
	shell_print(sh, "partition     : %zu B", in.partition_size);
	shell_print(sh, "sector        : %zu B x %u (2 master, 1 scratch, "
		    "%u buckets)", in.sector_size, in.n_sectors, in.n_buckets);
	shell_print(sh, "bucket data   : %zu B each, %llu B total",
		    in.bucket_capacity,
		    (unsigned long long)in.bucket_capacity * in.n_buckets);
	shell_print(sh, "write align   : %zu B", in.write_align);
	shell_print(sh, "slot          : %zu B overhead, payload <= %zu B",
		    in.slot_overhead, in.max_payload_len);
	shell_print(sh, "master        : %c, generation %u",
		    in.active_master ? 'B' : 'A', in.master_gen);
	shell_print(sh, "next id       : %llu (durable ceiling %llu)",
		    (unsigned long long)in.next_id,
		    (unsigned long long)in.next_id_hint);
	if (in.seg_owner) {
		shell_print(sh, "seg write     : in flight for id %llu",
			    (unsigned long long)in.seg_owner);
	}
	if (in.wedged) {
		shell_warn(sh, "WEDGED        : torn compaction; mutations "
			   "refused until remount");
	}
	return 0;
}

/* blob_db stats [top] ---------------------------------------------------- */

struct top_entry {
	uint16_t bid;
	uint32_t used;
};

static void top_insert(struct top_entry *top, size_t *n, size_t cap,
		       uint16_t bid, uint32_t used)
{
	size_t i = *n;

	if (i == cap) {
		if (used <= top[cap - 1].used) {
			return;
		}
		i = cap - 1;
	} else {
		(*n)++;
	}
	while (i > 0 && top[i - 1].used < used) {
		top[i] = top[i - 1];
		i--;
	}
	top[i] = (struct top_entry){ .bid = bid, .used = used };
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	size_t top_n = TOP_DEFAULT;

	if (need_info(sh, &in) < 0) {
		return -ENODEV;
	}
	if (argc > 1) {
		uint64_t v;

		if (parse_u64(sh, argv[1], &v) < 0) {
			return -EINVAL;
		}
		top_n = MIN((size_t)v, (size_t)TOP_MAX);
	}

	uint64_t live = 0, payload = 0, garbage = 0, tail = 0, free_b = 0;
	uint32_t objects = 0, segments = 0, superseded = 0, tombs = 0;
	uint32_t formatted = 0, compactions = 0, io_err = 0;
	uint32_t obj_min = UINT32_MAX, obj_max = 0;
	uint32_t hist[HIST_BINS] = { 0 };
	struct top_entry top[TOP_MAX];
	size_t n_top = 0;

	for (uint16_t bid = 0; bid < in.n_buckets; bid++) {
		struct blob_db_inspect_bucket bs;
		int rc = blob_db_inspect_bucket_get(bid, &bs, NULL, NULL);

		if (rc < 0) {
			io_err++;
			continue;
		}

		live += bs.live_bytes;
		payload += bs.payload_bytes;
		garbage += bs.garbage_bytes;
		tail += bs.tail_bytes;
		free_b += bs.free_bytes;
		objects += bs.objects;
		segments += bs.segments;
		superseded += bs.superseded;
		tombs += bs.tombstones;
		obj_min = MIN(obj_min, (uint32_t)bs.objects);
		obj_max = MAX(obj_max, (uint32_t)bs.objects);

		if (!bs.formatted) {
			continue;
		}
		formatted++;
		compactions += bs.gen - 1;

		const uint32_t used = bs.capacity - bs.free_bytes;
		const uint32_t bin = MIN(permille(used, bs.capacity) /
					 (1000 / HIST_BINS),
					 (uint32_t)HIST_BINS - 1);

		hist[bin]++;
		if (top_n) {
			top_insert(top, &n_top, top_n, bid, used);
		}
	}

	const uint64_t cap = (uint64_t)in.bucket_capacity * in.n_buckets;
	const uint64_t used = cap - free_b;

	shell_print(sh, "buckets     : %u formatted, %u never used (of %u)",
		    formatted, in.n_buckets - formatted, in.n_buckets);
	if (io_err) {
		shell_warn(sh, "              %u could not be read", io_err);
	}
	shell_print(sh, "objects     : %u live ids, %u segments",
		    objects, segments);
	shell_print(sh, "dead slots  : %u stale, %u tombstones",
		    superseded, tombs);
	shell_print(sh, "compactions : %u since the buckets were formatted",
		    compactions);
	shell_print(sh, "");
	shell_print(sh, "space         bytes        share of data area");
	shell_print(sh, "  capacity    %-12llu", (unsigned long long)cap);
	shell_print(sh, "  live        %-12llu " PM_FMT,
		    (unsigned long long)live, PM_ARG(permille(live, cap)));
	shell_print(sh, "    payload   %-12llu " PM_FMT "  (overhead "
		    "%llu B)", (unsigned long long)payload,
		    PM_ARG(permille(payload, cap)),
		    (unsigned long long)(live - payload));
	shell_print(sh, "  garbage     %-12llu " PM_FMT,
		    (unsigned long long)garbage, PM_ARG(permille(garbage, cap)));
	shell_print(sh, "  torn/rotten %-12llu " PM_FMT,
		    (unsigned long long)tail, PM_ARG(permille(tail, cap)));
	shell_print(sh, "  free        %-12llu " PM_FMT,
		    (unsigned long long)free_b, PM_ARG(permille(free_b, cap)));
	shell_print(sh, "  used        %-12llu " PM_FMT
		    "  (reclaimable by compaction: %llu B)",
		    (unsigned long long)used, PM_ARG(permille(used, cap)),
		    (unsigned long long)(garbage + tail));

	if (in.n_buckets) {
		const uint64_t mean_milli =
			(uint64_t)objects * 1000 / in.n_buckets;

		shell_print(sh, "");
		shell_print(sh, "ids/bucket  : min %u, mean %u.%03u, max %u",
			    obj_min, (unsigned int)(mean_milli / 1000),
			    (unsigned int)(mean_milli % 1000), obj_max);
	}

	if (formatted) {
		uint32_t peak = 1;

		for (int i = 0; i < HIST_BINS; i++) {
			peak = MAX(peak, hist[i]);
		}

		shell_print(sh, "");
		shell_print(sh, "fill of formatted buckets (used / capacity):");
		for (int i = 0; i < HIST_BINS; i++) {
			char bar[HIST_BAR + 1];
			size_t w = DIV_ROUND_UP((size_t)hist[i] * HIST_BAR, peak);

			memset(bar, '#', w);
			bar[w] = '\0';
			shell_print(sh, "  %3d-%3d%% %6u %s",
				    i * (100 / HIST_BINS),
				    (i + 1) * (100 / HIST_BINS), hist[i], bar);
		}
	}

	if (n_top) {
		shell_print(sh, "");
		shell_print(sh, "fullest buckets:");
		for (size_t i = 0; i < n_top; i++) {
			shell_print(sh, "  bucket %-5u %6u B used " PM_FMT,
				    top[i].bid, top[i].used,
				    PM_ARG(permille(top[i].used,
						    in.bucket_capacity)));
		}
	}
	return 0;
}

/* blob_db buckets [first [count]] --------------------------------------- */

static int cmd_buckets(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	uint64_t first = 0, count;

	if (need_info(sh, &in) < 0) {
		return -ENODEV;
	}
	count = in.n_buckets;
	if (argc > 1 && parse_u64(sh, argv[1], &first) < 0) {
		return -EINVAL;
	}
	if (argc > 2 && parse_u64(sh, argv[2], &count) < 0) {
		return -EINVAL;
	}
	if (first >= in.n_buckets) {
		shell_error(sh, "bucket %llu out of range (0..%u)",
			    (unsigned long long)first, in.n_buckets - 1);
		return -EINVAL;
	}
	const uint64_t last = MIN(first + count, (uint64_t)in.n_buckets);

	shell_print(sh, "bucket   gen  ids  seg stale tomb    live garbage"
		    "   torn    free   used");

	uint32_t skipped = 0;

	for (uint64_t bid = first; bid < last; bid++) {
		struct blob_db_inspect_bucket bs;
		int rc = blob_db_inspect_bucket_get((uint16_t)bid, &bs, NULL, NULL);

		if (rc < 0) {
			shell_error(sh, "%6llu  read error %d",
				    (unsigned long long)bid, rc);
			continue;
		}
		if (!bs.formatted) {
			skipped++;
			continue;
		}
		const uint32_t used = bs.capacity - bs.free_bytes;

		shell_print(sh, "%6llu %5u %4u %4u %5u %4u %7u %7u %6u %7u "
			    PM_FMT, (unsigned long long)bid, bs.gen,
			    bs.objects, bs.segments, bs.superseded,
			    bs.tombstones, bs.live_bytes, bs.garbage_bytes,
			    bs.tail_bytes, bs.free_bytes,
			    PM_ARG(permille(used, bs.capacity)));
	}
	if (skipped) {
		shell_print(sh, "(%u never-used buckets not shown)", skipped);
	}
	return 0;
}

/* blob_db bucket <bid> ---------------------------------------------------- */

static int print_slot(const struct blob_db_inspect_slot *s, void *user)
{
	const struct shell *sh = user;

	shell_print(sh, "  0x%05x %5u %20llu %5u  0x%02x %s", s->offset,
		    s->size, (unsigned long long)s->id, s->val_len, s->flags,
		    state_str(s));
	return 0;
}

static int cmd_bucket(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;
	uint64_t bid;

	ARG_UNUSED(argc);
	if (need_info(sh, &in) < 0) {
		return -ENODEV;
	}
	if (parse_u64(sh, argv[1], &bid) < 0) {
		return -EINVAL;
	}
	if (bid >= in.n_buckets) {
		shell_error(sh, "bucket %llu out of range (0..%u)",
			    (unsigned long long)bid, in.n_buckets - 1);
		return -EINVAL;
	}

	shell_print(sh, "bucket %llu (sector %llu)", (unsigned long long)bid,
		    (unsigned long long)(bid + in.n_sectors - in.n_buckets));
	shell_print(sh, "  offset  size                   id   len flags state");

	int rc = blob_db_inspect_bucket_get((uint16_t)bid, &bs, print_slot,
					    (void *)sh);

	if (rc < 0) {
		shell_error(sh, "read: %d", rc);
		return rc;
	}
	if (!bs.formatted) {
		shell_print(sh, "  (never used — no valid bucket header)");
		return 0;
	}

	const uint32_t used = bs.capacity - bs.free_bytes;

	shell_print(sh, "generation %u: %u ids, %u segments, %u stale, "
		    "%u tombstones", bs.gen, bs.objects, bs.segments,
		    bs.superseded, bs.tombstones);
	shell_print(sh, "live %u B (payload %u B), garbage %u B, torn %u B, "
		    "free %u B of %u B — " PM_FMT " used", bs.live_bytes,
		    bs.payload_bytes, bs.garbage_bytes, bs.tail_bytes,
		    bs.free_bytes, bs.capacity,
		    PM_ARG(permille(used, bs.capacity)));
	return 0;
}

/* blob_db id <id> --------------------------------------------------------- */

struct id_filter {
	const struct shell *sh;
	uint64_t id;
	uint32_t hits;
};

static int print_slot_of(const struct blob_db_inspect_slot *s, void *user)
{
	struct id_filter *f = user;

	if (s->id == f->id) {
		f->hits++;
		print_slot(s, (void *)f->sh);
	}
	return 0;
}

static int cmd_id(const struct shell *sh, size_t argc, char **argv)
{
	struct blob_db_inspect_info in;
	struct blob_db_inspect_bucket bs;
	struct id_filter f = { .sh = sh };

	ARG_UNUSED(argc);
	if (need_info(sh, &in) < 0) {
		return -ENODEV;
	}
	if (parse_u64(sh, argv[1], &f.id) < 0) {
		return -EINVAL;
	}

	const uint16_t bid = (uint16_t)(f.id % in.n_buckets);
	size_t size;
	int rc = blob_db_size(f.id, &size);

	shell_print(sh, "id %llu -> bucket %u", (unsigned long long)f.id, bid);
	if (rc == 0) {
		shell_print(sh, "live, %zu B", size);
	} else if (rc == -ENOENT) {
		shell_print(sh, "not live%s", f.id >= in.next_id
			    ? " (never allocated)" : "");
	} else {
		shell_error(sh, "size: %d", rc);
	}

	shell_print(sh, "slots in bucket %u for this id (oldest first):", bid);
	rc = blob_db_inspect_bucket_get(bid, &bs, print_slot_of, &f);
	if (rc < 0) {
		shell_error(sh, "read: %d", rc);
		return rc;
	}
	if (!f.hits) {
		shell_print(sh, "  (none)");
	}
	return 0;
}

/* blob_db dump <id> [len] ------------------------------------------------- */

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint64_t id, max = DUMP_DEFAULT;
	size_t size;

	if (parse_u64(sh, argv[1], &id) < 0) {
		return -EINVAL;
	}
	if (argc > 2 && parse_u64(sh, argv[2], &max) < 0) {
		return -EINVAL;
	}

	int rc = blob_db_size(id, &size);

	if (rc < 0) {
		shell_error(sh, "id %llu: %d", (unsigned long long)id, rc);
		return rc;
	}
	shell_print(sh, "id %llu: %zu B", (unsigned long long)id, size);

	const size_t want = MIN((size_t)max, size);
	uint8_t buf[DUMP_CHUNK];

	for (size_t off = 0; off < want; off += sizeof(buf)) {
		size_t got;

		rc = blob_db_read(id, off, buf, MIN(sizeof(buf), want - off),
				  &got);
		if (rc < 0) {
			shell_error(sh, "read at %zu: %d", off, rc);
			return rc;
		}
		for (size_t i = 0; i < got; i += 16) {
			shell_hexdump_line(sh, off + i, buf + i,
					   MIN((size_t)16, got - i));
		}
		if (got < MIN(sizeof(buf), want - off)) {
			break;
		}
	}
	if (want < size) {
		shell_print(sh, "... %zu more bytes", size - want);
	}
	return 0;
}

#if defined(CONFIG_BLOB_DB_IOSTATS)
/* blob_db iostats [reset] ------------------------------------------------- */

static int cmd_iostats(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "reset") != 0) {
			shell_error(sh, "usage: blob_db iostats [reset]");
			return -EINVAL;
		}
		blob_db_iostats_reset();
		shell_print(sh, "counters reset");
		return 0;
	}

	struct blob_db_iostats io;

	blob_db_iostats_get(&io);
	shell_print(sh, "reads  : %u ops, %llu B", io.reads,
		    (unsigned long long)io.bytes_read);
	shell_print(sh, "writes : %u ops, %llu B", io.writes,
		    (unsigned long long)io.bytes_written);
	shell_print(sh, "erases : %u ops, %llu B", io.erases,
		    (unsigned long long)io.bytes_erased);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_blob_db,
	SHELL_CMD_ARG(info, NULL,
		"Geometry and allocator state (no flash access)",
		cmd_info, 1, 0),
	SHELL_CMD_ARG(stats, NULL,
		"Whole-store occupancy and fill distribution\n"
		"usage: stats [top_n]  (fullest buckets to list, default 5)",
		cmd_stats, 1, 1),
	SHELL_CMD_ARG(buckets, NULL,
		"Per-bucket occupancy table (never-used buckets skipped)\n"
		"usage: buckets [first [count]]",
		cmd_buckets, 1, 2),
	SHELL_CMD_ARG(bucket, NULL,
		"Slot-by-slot view of one bucket\n"
		"usage: bucket <bid>",
		cmd_bucket, 2, 0),
	SHELL_CMD_ARG(id, NULL,
		"Where an id lives: its bucket, state and slot history\n"
		"usage: id <id>",
		cmd_id, 2, 0),
	SHELL_CMD_ARG(dump, NULL,
		"Hex dump of a blob's payload\n"
		"usage: dump <id> [max_bytes]  (default 256)",
		cmd_dump, 2, 1),
#if defined(CONFIG_BLOB_DB_IOSTATS)
	SHELL_CMD_ARG(iostats, NULL,
		"Flash I/O counters\n"
		"usage: iostats [reset]",
		cmd_iostats, 1, 1),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(blob_db, &sub_blob_db, "blob_db inspection (read-only)",
		   NULL);
