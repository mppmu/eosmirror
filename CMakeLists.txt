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

# The XRootD client library, for XRootD and EOS endpoints. CERN's eos-xrootd
# lives under /opt/eos/xrootd and is preferred when present; its Ubuntu
# packages ship no CMake config, hence the manual search as fallback.
option(EOSMIRROR_WITH_XROOTD "Build the XRootD and EOS endpoints" ON)
set(EOSMIRROR_XROOTD_PREFIX "/opt/eos/xrootd" CACHE PATH "Preferred XRootD installation (empty: system only)")
if(EOSMIRROR_WITH_XROOTD)
  if(EOSMIRROR_XROOTD_PREFIX)
    list(PREPEND CMAKE_PREFIX_PATH ${EOSMIRROR_XROOTD_PREFIX})
  endif()
  find_package(XRootD QUIET COMPONENTS CLIENT)
  if(XRootD_FOUND)
    message(STATUS "XRootD ${XRootD_VERSION} from ${XRootD_DIR}")
    # The config lives in <libdir>/cmake/XRootD.
    get_filename_component(EOSMIRROR_XROOTD_LIBDIR "${XRootD_DIR}/../.." ABSOLUTE)
  else()
    find_path(XROOTD_INCLUDE_DIR XrdCl/XrdClFile.hh PATH_SUFFIXES xrootd)
    find_library(XRDCL_LIBRARY NAMES XrdCl PATH_SUFFIXES lib64)
    if(NOT XROOTD_INCLUDE_DIR OR NOT XRDCL_LIBRARY)
      message(FATAL_ERROR "XRootD client library not found (set EOSMIRROR_WITH_XROOTD=OFF to build without)")
    endif()
    add_library(XRootD::XrdCl SHARED IMPORTED)
    set_target_properties(XRootD::XrdCl PROPERTIES
      IMPORTED_LOCATION ${XRDCL_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${XROOTD_INCLUDE_DIR})
    get_filename_component(EOSMIRROR_XROOTD_LIBDIR ${XRDCL_LIBRARY} DIRECTORY)
    message(STATUS "XRootD client: ${XRDCL_LIBRARY}, headers in ${XROOTD_INCLUDE_DIR}")
  endif()
endif()

add_subdirectory(src)

if(EOSMIRROR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
