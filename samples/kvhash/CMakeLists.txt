#-------------------------------------------------------------------------------
# kvhash sample — the L2 Map shape, one operation at a time
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sample_kvhash LANGUAGES C)

target_sources(app PRIVATE src/main.c)
