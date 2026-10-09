cmake_minimum_required(VERSION 3.25)
project(lob_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOB_NATIVE "Optimize for this machine's CPU (-march=native)" ON)
option(LOB_BENCHMARKS "Build the benchmarks (downloads Google Benchmark)" ON)

# Strict warnings for our own code only (not third-party dependencies).
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Werror)

if(LOB_NATIVE AND CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-march=native)
endif()

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(lob src/map_book.cpp src/ladder_book.cpp src/lobster.cpp)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PRIVATE lob_warnings)

add_executable(lob_replay src/replay_main.cpp)
target_link_libraries(lob_replay PRIVATE lob lob_warnings)

# Tests
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(lob_tests tests/order_book_test.cpp tests/differential_test.cpp
                         tests/lobster_test.cpp tests/order_id_map_test.cpp)
target_compile_definitions(lob_tests PRIVATE
  LOB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  LOB_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)

# Separate binary: it replaces the global operator new to count allocations.
add_executable(lob_allocation_tests tests/allocation_test.cpp)
target_link_libraries(lob_allocation_tests PRIVATE lob lob_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)
gtest_discover_tests(lob_allocation_tests)

# Benchmarks
if(LOB_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lob_micro_bench bench/micro_bench.cpp)
  target_link_libraries(lob_micro_bench PRIVATE lob lob_warnings benchmark::benchmark)

  add_executable(lob_replay_bench bench/replay_bench.cpp)
  target_compile_definitions(lob_replay_bench PRIVATE LOB_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  target_link_libraries(lob_replay_bench PRIVATE lob lob_warnings)
endif()
