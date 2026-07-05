# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_CUSTOM custom)
add_subdirectory_ifdef(CONFIG_BLOB_DB blob_db)
add_subdirectory_ifdef(CONFIG_BLOB_ROOTREG rootreg)
add_subdirectory(containers)
add_subdirectory_ifdef(CONFIG_BLOBDB_KVDB kvdb)
add_subdirectory_ifdef(CONFIG_BLOBDB_BLOBFS blobfs)
