#-------------------------------------------------------------------------------
# app_cbor_persondb — CBOR person/credential database over the L2 Map shape
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app_cbor_persondb LANGUAGES C)

# Layers, bottom-up (see DESIGN.md §7). Each one depends only on the line
# above it; main.c depends on nothing but the selected frontend.
target_sources(app PRIVATE
	src/dataset.c        # pure generator: index -> person
	src/person_cbor.c    # CBOR codec (zcbor)
	src/persondb.c       # the person management API, on L2 map_ops
	src/scenario.c       # operations on a whole population
	src/main.c
)

target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_BENCH app PRIVATE src/ui_bench.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL app PRIVATE src/ui_shell.c)
