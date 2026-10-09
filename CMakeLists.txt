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

# Strict warnings for our own code only (not third-party dependencies).
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Werror)

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(lob src/map_book.cpp src/lobster.cpp)
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
                         tests/lobster_test.cpp)
target_compile_definitions(lob_tests PRIVATE
  LOB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
  LOB_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lob_tests)
