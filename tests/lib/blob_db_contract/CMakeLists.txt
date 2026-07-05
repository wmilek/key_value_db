# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blob_db_contract_test)

# The model container is the executable form of the L1 sufficiency proof
# (doc/layers/l1_model_container.md). It is built here, not in lib/, because
# it is a test/proof artifact, not shipped code.
target_sources(app PRIVATE
	src/main.c
	src/model_container.c
)
target_include_directories(app PRIVATE src)
