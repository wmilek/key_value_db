#-------------------------------------------------------------------------------
# model-container performance benchmark app
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app_perf_mc LANGUAGES C)

# The model containers are test/proof artifacts that live with the L1
# contract suite (see tests/lib/blob_db_contract/CMakeLists.txt); this
# benchmark measures that exact code, so it compiles the same sources.
set(MC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tests/lib/blob_db_contract/src)

target_sources(app PRIVATE
	src/main.c
	${MC_DIR}/model_container_map.c
	${MC_DIR}/model_container_list.c
)
target_include_directories(app PRIVATE ${MC_DIR})
