/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * blob_db — stable-id blob storage on flash_area.
 *
 * Stage 1 (scaffold): the file exists to give the BUILD_ASSERTs in
 * blob_db_internal.h a translation unit to fire in when CONFIG_BLOB_DB=y.
 * The real implementation arrives in stage 3.
 */

#include <zephyr/logging/log.h>

#include <app/lib/blob_db.h>

#include "blob_db_internal.h"

LOG_MODULE_REGISTER(blob_db, CONFIG_BLOB_DB_LOG_LEVEL);
