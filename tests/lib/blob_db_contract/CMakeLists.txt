# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blob_db_contract_test)

# The model containers are the executable form of the L1 sufficiency proof
# (doc/layers/l1_model_container.md, l1_model_container_list.md). They are
# built here, not in lib/, because they are test/proof artifacts, not
# shipped code.
target_sources(app PRIVATE
	src/main.c
	src/model_container_map.c
	src/list.c
	src/model_container_list.c
)
target_include_directories(app PRIVATE src)
