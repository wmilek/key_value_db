# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(containers_test)

# The container providers are built from lib/containers by the module CMake
# when their Kconfigs are enabled; we only add the test itself.
target_sources(app PRIVATE src/main.c)
