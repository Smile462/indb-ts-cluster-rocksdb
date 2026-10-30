#  Copyright (c) 2026 The in-database time-series clustering authors.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# Standalone build for the in-database clustering add-on. It is deliberately
# not wired into RocksDB's own build files: the kernel must build without the
# database, and the add-on links against an already built rocksdb library.
cmake_minimum_required(VERSION 3.16)
project(indb_ts_cluster_rocksdb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # /fp:precise keeps floating point contraction off so results stay
  # comparable with the Java reference implementation.
  add_compile_options(/W3 /permissive- /fp:precise /EHsc)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -O2 -ffp-contract=off)
endif()

set(ROCKSDB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    CACHE PATH "RocksDB public headers")
set(ROCKSDB_LIBRARY "" CACHE FILEPATH "Path to rocksdb.lib")

if(NOT ROCKSDB_LIBRARY)
  message(FATAL_ERROR "Set -DROCKSDB_LIBRARY=<path to rocksdb.lib>")
endif()

add_library(indb_core STATIC
  core/src/math.cpp
  core/src/meta.cpp
  core/src/query.cpp
)
target_include_directories(indb_core PUBLIC core/include)

add_library(indb_store STATIC rocksdb/store.cpp rocksdb/store_props.cpp rocksdb/stats_props.cpp)
target_include_directories(indb_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ROCKSDB_INCLUDE_DIR})
target_link_libraries(indb_store PUBLIC indb_core ${ROCKSDB_LIBRARY})

add_executable(indb_cli tools/main.cpp)
target_link_libraries(indb_cli PRIVATE indb_store)

add_executable(indb_tests tests/test_core.cpp)
target_link_libraries(indb_tests PRIVATE indb_store)

if(WIN32)
  # RocksDB's Windows port needs these system libraries.
  target_link_libraries(indb_store PUBLIC shlwapi rpcrt4)
endif()

enable_testing()
add_test(NAME core_and_store COMMAND indb_tests)
