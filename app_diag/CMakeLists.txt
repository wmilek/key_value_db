#-------------------------------------------------------------------------------
# kvdb diagnostics: a shell over the storage stack
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app_diag LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# `diag`: structure recognition, with the formats compiled in here.
target_sources_ifdef(CONFIG_APP_DIAG_STRUCT app PRIVATE
	src/diag_shell.c
	src/diag_decode.c
	src/diag_cbor.c
)
