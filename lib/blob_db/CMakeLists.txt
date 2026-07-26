# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(blob_db)
zephyr_library_sources(blob_db.c)
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_BACKEND_FLASH_AREA blob_db_store_flash.c)
zephyr_library_sources_ifdef(CONFIG_BLOB_DB_BACKEND_UBI        blob_db_store_ubi.c)
