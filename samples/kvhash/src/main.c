/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * kvhash sample — the L2 Map shape, one operation at a time.
 *
 * kvhash is a *container*, not an interface. It exports exactly one thing,
 * the op vector `kvhash_map_ops` (create / get / set / del), and every op
 * takes the map's **root id** as its first argument. There is no handle to
 * keep alive, nothing to close, and no RAM state between calls: the
 * `uint64_t` root id, stored somewhere durable, *is* the map.
 *
 * So a caller has two jobs beyond the four operations, and this sample shows
 * both:
 *
 *   - where the root id comes from — the root registry maps a compile-time
 *     key to a root id, so the next boot re-finds the same map;
 *   - when to call create() — once, when that root does not hold a structure
 *     yet; never again on a later boot.
 *
 * The sample keeps a handful of device settings in a map and narrates every
 * call and its return code on the console. Nothing here is timed; for
 * throughput numbers see app_perf_kvdb/.
 *
 * Run it:
 *   west build -b native_sim key_value_db/samples/kvhash && ./build/zephyr/zephyr.exe
 *
 * Keys and values are opaque byte slices (pointer + length) at this layer —
 * kvhash never inspects them. String keys are passed *without* their NUL
 * terminator here, which is why every key is `key, strlen(key)`. For
 * NUL-terminated C-string keys with the same container underneath, use kvdb
 * (L3, include/app/lib/kvdb.h).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <app/lib/blob_db.h>
#include <app/lib/rootreg.h>
#include <app/lib/containers/kvhash.h>

#include <zephyr/app_version.h>

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

LOG_MODULE_REGISTER(sample_kvhash, CONFIG_SAMPLE_KVHASH_LOG_LEVEL);

/*
 * This sample's map in the root registry: FourCC 'SMPL', instance 0. Every
 * module that owns a structure picks a key like this and keeps it forever —
 * it is the name under which the root id is found again after a reboot.
 */
#define SAMPLE_MAP_KEY ROOTREG_KEY(0x534d504cu /* 'SMPL' */, 0)

/*
 * Bucket count, frozen into the map when it is created. kvhash reads
 * map_config.initial_capacity as the size of its bucket directory, clamped so
 * the directory fits one blob payload — with the default
 * CONFIG_BLOB_DB_MAX_PAYLOAD_LEN of 256 the ceiling is 31 buckets. There is no
 * online resize in v1, so pick it for the store you expect: a key lands in
 * bucket fnv1a(key) % n_buckets and each bucket is a short linear scan.
 */
#define N_BUCKETS 16

/* The container's whole API surface. */
static const struct map_ops *const map = &kvhash_map_ops;

/* A value does not have to be text — it is bytes. This one is a packed
 * struct, written and read back by the same build (blob_db's host-order
 * convention); a store shared between differing builds would use an explicit
 * encoding such as CBOR, as app_cbor_persondb does. */
struct calibration {
	int16_t  offset_mdeg;
	uint16_t gain_ppm;
	uint32_t stamp;
};

static void step(int n, const char *title)
{
	printk("\n[%d] %s\n", n, title);
}

/* set() a text value, narrating the call. */
static int set_str(uint64_t root, const char *key, const char *val)
{
	int rc = map->set(root, key, strlen(key), val, strlen(val));

	printk("    set(\"%s\", \"%s\") -> %d\n", key, val, rc);
	return rc;
}

/* get() a text value into a local buffer, narrating the call. */
static int get_str(uint64_t root, const char *key)
{
	char buf[64];
	size_t len = 0;
	int rc = map->get(root, key, strlen(key), buf, sizeof(buf) - 1, &len);

	if (rc == 0) {
		buf[len] = '\0';
		printk("    get(\"%s\") -> 0, %u bytes: \"%s\"\n",
		       key, (unsigned)len, buf);
	} else {
		printk("    get(\"%s\") -> %d\n", key, rc);
	}
	return rc;
}

static int run(void)
{
	uint64_t root;
	int rc;

	step(1, "bring the storage stack up");

	/* blob_db is the store every container lives in; -EALREADY just means
	 * something else mounted it first. */
	rc = blob_db_mount();
	if (rc != 0 && rc != -EALREADY) {
		printk("    blob_db_mount() -> %d  FAILED\n", rc);
		return rc;
	}
	printk("    blob_db_mount() -> %d\n", rc);

	if (IS_ENABLED(CONFIG_SAMPLE_KVHASH_FRESH_START)) {
		/* Only so every run of the sample prints the same thing.
		 * A real application never does this at boot. */
		rc = blob_db_erase_all();
		printk("    blob_db_erase_all() -> %d  (start from an empty store)\n", rc);
		if (rc != 0) {
			return rc;
		}
	}

	/* The registry owns blob id 1 and must be bootstrapped before anything
	 * that keeps a root in it. Idempotent. */
	rc = rootreg_init();
	printk("    rootreg_init() -> %d\n", rc);
	if (rc != 0) {
		return rc;
	}

	step(2, "find the map's root id, and create the map the first time");

	/* Absent: allocate an id and register it, atomically. Present: return
	 * the id registered by an earlier boot. */
	rc = rootreg_get_or_create(SAMPLE_MAP_KEY, &root);
	if (rc != 0) {
		printk("    rootreg_get_or_create() -> %d  FAILED\n", rc);
		return rc;
	}
	printk("    rootreg_get_or_create('SMPL':0) -> 0, root id %" PRIu64 "\n", root);

	/* A registered id is not yet a map: rootreg hands out the id, the
	 * container builds the structure behind it. An id with no blob bound to
	 * it means "never created" (or a crash before the create) — which is
	 * exactly the create-once condition. */
	if (!blob_db_exists(root)) {
		const struct map_config cfg = {
			.initial_capacity = N_BUCKETS,
		};

		rc = map->create(root, &cfg);
		printk("    create(root, .initial_capacity=%u) -> %d   [first run]\n",
		       (unsigned)N_BUCKETS, rc);
		if (rc != 0) {
			return rc;
		}
	} else {
		/* A later boot lands here: the map was built by an earlier run and
		 * everything it stored is still in it. Nothing was "loaded" — the
		 * read below goes straight to flash through the same root id. */
		printk("    root already holds a map, create() skipped   [reopen]\n");
		(void)get_str(root, "device/name");
	}

	step(3, "set — insert pairs");

	/* set() inserts a key or replaces its value; both are the same call.
	 * It rewrites the one bucket the key hashes to, and (only when that
	 * bucket is created) the directory — one or two atomic blob updates. */
	rc = set_str(root, "device/name", "kv-demo-01");
	if (rc != 0) {
		return rc;
	}
	rc = set_str(root, "net/ssid", "workshop-2g");
	if (rc != 0) {
		return rc;
	}
	rc = set_str(root, "net/channel", "6");
	if (rc != 0) {
		return rc;
	}

	const struct calibration cal = {
		.offset_mdeg = -125,
		.gain_ppm = 1004,
		.stamp = 0x5f2c1180u,
	};

	rc = map->set(root, "sensor/cal", strlen("sensor/cal"), &cal, sizeof(cal));
	printk("    set(\"sensor/cal\", %u raw bytes) -> %d\n", (unsigned)sizeof(cal), rc);
	if (rc != 0) {
		return rc;
	}

	step(4, "get — read the values back");

	rc = get_str(root, "device/name");
	if (rc != 0) {
		return rc;
	}
	rc = get_str(root, "net/ssid");
	if (rc != 0) {
		return rc;
	}

	struct calibration read_back;
	size_t len = 0;

	rc = map->get(root, "sensor/cal", strlen("sensor/cal"),
		      &read_back, sizeof(read_back), &len);
	printk("    get(\"sensor/cal\") -> %d, %u bytes: "
	       "offset=%d mdeg gain=%u ppm stamp=0x%08x\n",
	       rc, (unsigned)len, (int)read_back.offset_mdeg,
	       (unsigned)read_back.gain_ppm, (unsigned)read_back.stamp);
	if (rc != 0) {
		return rc;
	}

	step(5, "get — sizing a buffer, and probing for existence");

	/* A short buffer never truncates: get() refuses with -ENOMEM and sets
	 * *out_len to the true length, so the caller can size and retry. */
	char small[4];
	size_t need = 0;

	rc = map->get(root, "net/ssid", strlen("net/ssid"), small, sizeof(small), &need);
	printk("    get(\"net/ssid\", out_sz=%u) -> %d (-ENOMEM), true length %u\n",
	       (unsigned)sizeof(small), rc, (unsigned)need);

	char right_sized[32];

	rc = map->get(root, "net/ssid", strlen("net/ssid"), right_sized,
		      sizeof(right_sized), &need);
	printk("    get(\"net/ssid\", out_sz=%u) -> %d, %u bytes: \"%.*s\"\n",
	       (unsigned)sizeof(right_sized), rc, (unsigned)need,
	       (int)need, right_sized);

	/* out = NULL with out_sz = 0 is the existence probe: the key is present
	 * iff the call returns 0 (empty value) or -ENOMEM (value does not fit
	 * in zero bytes). Only -ENOENT means absent. */
	rc = map->get(root, "net/ssid", strlen("net/ssid"), NULL, 0, NULL);
	printk("    probe(\"net/ssid\")   -> %d -> present: %s\n",
	       rc, (rc == 0 || rc == -ENOMEM) ? "yes" : "no");

	rc = map->get(root, "net/secret", strlen("net/secret"), NULL, 0, NULL);
	printk("    probe(\"net/secret\") -> %d (-ENOENT) -> present: %s\n",
	       rc, (rc == 0 || rc == -ENOMEM) ? "yes" : "no");

	step(6, "set — replacing the value of an existing key");

	/* Same call as an insert. The key keeps its place in the map; the old
	 * value is gone the moment the bucket write commits. */
	rc = get_str(root, "net/channel");
	if (rc != 0) {
		return rc;
	}
	rc = set_str(root, "net/channel", "11");
	if (rc != 0) {
		return rc;
	}
	rc = get_str(root, "net/channel");
	if (rc != 0) {
		return rc;
	}

	step(7, "del — remove a key, and what a missing key returns");

	rc = map->del(root, "net/channel", strlen("net/channel"));
	printk("    del(\"net/channel\") -> %d\n", rc);
	if (rc != 0) {
		return rc;
	}

	/* Deleting or reading a key that is not there is not a failure of the
	 * map — it is the answer. -ENOENT, both times. */
	rc = map->del(root, "net/channel", strlen("net/channel"));
	printk("    del(\"net/channel\") -> %d (-ENOENT, already gone)\n", rc);

	(void)get_str(root, "net/channel");

	step(8, "the root id is the map — nothing else is kept");

	/* Every call above went through the same stateless op vector. Resolving
	 * the id from the registry again — what the next boot does — yields the
	 * same map, with no reopen step in between. */
	uint64_t again = 0;

	rc = rootreg_get(SAMPLE_MAP_KEY, &again);
	printk("    rootreg_get('SMPL':0) -> %d, root id %" PRIu64 " (same id: %s)\n",
	       rc, again, (again == root) ? "yes" : "no");
	if (rc != 0) {
		return rc;
	}
	(void)get_str(again, "device/name");

	step(9, "the v1 bound — a bucket must fit one blob payload");

	/* kvhash packs a whole bucket into a single blob payload
	 * (CONFIG_BLOB_DB_MAX_PAYLOAD_LEN bytes). A pair that cannot fit is
	 * refused with -ENOSPC and the map is left exactly as it was — worth
	 * handling, because it is the one bound a caller can hit while the
	 * store is otherwise healthy. */
	static uint8_t oversized[CONFIG_BLOB_DB_MAX_PAYLOAD_LEN];

	memset(oversized, 'x', sizeof(oversized));
	rc = map->set(root, "log/dump", strlen("log/dump"), oversized, sizeof(oversized));
	printk("    set(\"log/dump\", %u bytes) -> %d (-ENOSPC; payload limit is %u)\n",
	       (unsigned)sizeof(oversized), rc,
	       (unsigned)CONFIG_BLOB_DB_MAX_PAYLOAD_LEN);

	(void)get_str(root, "device/name");   /* untouched by the refusal */

	return 0;
}

int main(void)
{
	printk("\nkvhash sample %s — the L2 Map shape, one operation at a time\n",
	       APP_VERSION_STRING);

	int rc = run();

	if (rc != 0) {
		printk("\nsample failed: %d\n", rc);
		LOG_ERR("aborted at rc=%d", rc);
	} else {
		printk("\nsample complete\n");
	}

#ifdef CONFIG_ARCH_POSIX
	posix_exit(rc == 0 ? 0 : 1);
#endif
	return 0;
}
