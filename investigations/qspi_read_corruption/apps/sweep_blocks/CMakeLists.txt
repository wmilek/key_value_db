cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(qspi_sweep_blocks)
target_sources(app PRIVATE src/main.c)
# trace.inc: the 43,300 reads captured before the corrupted read
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
