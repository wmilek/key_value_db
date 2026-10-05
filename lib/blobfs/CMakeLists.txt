# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(blobfs)
zephyr_library_sources(blobfs.c)
zephyr_library_sources_ifdef(CONFIG_BLOBFS_FS_INTEROP blobfs_zephyr_fs.c)
