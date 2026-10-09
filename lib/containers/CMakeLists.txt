# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_BLOB_CONTAINERS_INTENT intent)
add_subdirectory_ifdef(CONFIG_BLOB_CONTAINER_LOGRING logring)
add_subdirectory_ifdef(CONFIG_BLOB_CONTAINER_SEQ     seq)
add_subdirectory_ifdef(CONFIG_BLOB_CONTAINER_KVLIST  kvlist)
add_subdirectory_ifdef(CONFIG_BLOB_CONTAINER_KVHASH  kvhash)
add_subdirectory_ifdef(CONFIG_BLOB_CONTAINER_KVTREE  kvtree)
