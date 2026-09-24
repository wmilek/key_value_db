# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blobfs_test)

# blobfs and its interop shim are built from lib/ by the module CMake when
# their Kconfigs are enabled; we add the test and Zephyr's own file system
# conformance bodies, compiled straight out of the Zephyr tree. ZEPHYR_BASE
# is the CMake variable find_package(Zephyr) exports — present however the
# Zephyr tree was located, unlike the environment variable of the same name.
set(FS_COMMON ${ZEPHYR_BASE}/tests/subsys/fs/common)

target_sources(app PRIVATE
  src/main.c
  ${FS_COMMON}/test_fs_util.c
  ${FS_COMMON}/test_fs_basic.c
  ${FS_COMMON}/test_fs_open_flags.c
)
target_include_directories(app PRIVATE ${FS_COMMON})

# The recovery test forges a mid-rename crash state, so it speaks the
# persisted meta format from the library's internal header.
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/blobfs)
