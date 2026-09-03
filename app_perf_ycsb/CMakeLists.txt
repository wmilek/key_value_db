#-------------------------------------------------------------------------------
# YCSB core-workload benchmark over kvdb (L3)
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app_perf_ycsb LANGUAGES C)

target_sources(app PRIVATE src/main.c src/generator.c)
