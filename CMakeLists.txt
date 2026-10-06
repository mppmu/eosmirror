# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(eosmirror VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(EOSMIRROR_WERROR "Treat compiler warnings as errors" ON)
option(EOSMIRROR_BUILD_TESTS "Build the unit tests" ON)
set(EOSMIRROR_SANITIZER "" CACHE STRING "Build with a sanitizer: address, thread or undefined")

add_compile_options(
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
if(EOSMIRROR_WERROR)
  add_compile_options(-Werror)
endif()

if(EOSMIRROR_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(EOSMIRROR_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(EOSMIRROR_SANITIZER STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(NOT EOSMIRROR_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown EOSMIRROR_SANITIZER '${EOSMIRROR_SANITIZER}'")
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_subdirectory(src)

if(EOSMIRROR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
