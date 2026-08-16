#-------------------------------------------------------------------------------
# app_cbor_persondb — CBOR person/credential database over the L2 Map shape
#
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app_cbor_persondb LANGUAGES C)

# One directory per layer, added bottom-up (DESIGN.md §7). Each subdirectory
# contributes its own sources and publishes its own header directory, so the
# build file has the same shape as the design — and a new dependency between
# layers is a visible edit here, not an #include someone slipped in.
#
#   ui/        frontends: parse and print only                    (F15)
#     |
#   scenario/  operations on a population: fill, verify, measure   (F14)
#     |   \_______________________
#     |                           |
#   persondb/                  dataset/
#     the person management       the synthetic population: a pure
#     API, and the only layer     function of an index. Uses persondb's
#     that touches storage        record type; touches no storage.
#     (F12, R-H)                                              (F6)
#     \__ person_cbor: the wire format, private to persondb
#
add_subdirectory(dataset)
add_subdirectory(persondb)
add_subdirectory(scenario)
add_subdirectory(ui)
