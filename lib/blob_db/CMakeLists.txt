# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(blob_db)
zephyr_library_sources(blob_db.c)
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_BACKEND_FLASH_AREA blob_db_store_flash.c)
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_BACKEND_UBI        blob_db_store_ubi.c)
# blob_db_maintain() on a work queue (<app/lib/blob_db.h>, blob_db_maint_start()).
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_MAINT_WORK          blob_db_maint_work.c)
# Diagnostics outside the blob_db contract (<app/lib/blob_db_inspect.h>).
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_INSPECT            blob_db_inspect.c)
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_SHELL              blob_db_shell.c)
