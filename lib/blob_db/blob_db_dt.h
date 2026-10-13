/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Where the store lives, from devicetree (dts/bindings/storage/kvdb,blob-db.yaml).
 *
 * The chosen node "kvdb,blob-db" names the store's node; its "storage"
 * phandle names either a UBI volume (a "volumes" child of a "kvdb,ubi"
 * partition) or a plain fixed partition. Both backends resolve the partition
 * they open from here, so nothing in the library names a partition label.
 */

#ifndef BLOB_DB_DT_H_
#define BLOB_DB_DT_H_

#include <zephyr/devicetree.h>
#include <zephyr/devicetree/partitions.h>

#if !DT_HAS_CHOSEN(kvdb_blob_db)
#error "blob_db needs a chosen node: chosen { kvdb,blob-db = &<your kvdb,blob-db node>; }"
#endif

/** The "kvdb,blob-db" node. */
#define BLOB_DB_DT_NODE          DT_CHOSEN(kvdb_blob_db)

/** What its "storage" property points at: a UBI volume or a partition. */
#define BLOB_DB_DT_STORAGE       DT_PHANDLE(BLOB_DB_DT_NODE, storage)

/** 1 when "storage" is a UBI volume: volume -> "volumes" -> "kvdb,ubi". */
#define BLOB_DB_DT_ON_UBI_VOLUME \
	DT_NODE_HAS_COMPAT(DT_GPARENT(BLOB_DB_DT_STORAGE), kvdb_ubi)

/** The fixed partition under the store: the UBI device's, or "storage". */
#define BLOB_DB_DT_PARTITION                                           \
	COND_CODE_1(BLOB_DB_DT_ON_UBI_VOLUME,                          \
		    (DT_GPARENT(BLOB_DB_DT_STORAGE)), (BLOB_DB_DT_STORAGE))

/** flash_map id of that partition. */
#define BLOB_DB_DT_PARTITION_ID  DT_PARTITION_ID(BLOB_DB_DT_PARTITION)

BUILD_ASSERT(DT_NODE_HAS_COMPAT(BLOB_DB_DT_NODE, kvdb_blob_db),
	     "chosen kvdb,blob-db must point at a kvdb,blob-db node");
BUILD_ASSERT(DT_PARTITION_EXISTS(BLOB_DB_DT_PARTITION),
	     "kvdb,blob-db 'storage' must be a UBI volume or a fixed partition");

#endif /* BLOB_DB_DT_H_ */
