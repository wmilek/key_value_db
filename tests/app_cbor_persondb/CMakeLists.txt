# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_cbor_persondb_test)

# persondb and its codec, built from the application's own sources: what is
# under test is the store format the application ships, not a copy of it.
set(PERSONDB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app_cbor_persondb/persondb)

target_sources(app PRIVATE
	src/main.c
	${PERSONDB_DIR}/persondb.c
	${PERSONDB_DIR}/person_cbor.c)
target_include_directories(app PRIVATE ${PERSONDB_DIR})
