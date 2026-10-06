# SPDX-License-Identifier: GPL-3.0-or-later
configure_file(eosmirror/version.hh.in ${CMAKE_CURRENT_BINARY_DIR}/generated/eosmirror/version.hh @ONLY)

add_library(eosmirror_core STATIC
  eosmirror/checksum.cc
  eosmirror/cli.cc
  eosmirror/copy.cc
  eosmirror/endpoints.cc
  eosmirror/engine.cc
  eosmirror/error.cc
  eosmirror/journal.cc
  eosmirror/log.cc
  eosmirror/posix_endpoint.cc
  eosmirror/report.cc
  eosmirror/selftest.cc
  eosmirror/types.cc
)
target_include_directories(eosmirror_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(eosmirror_core PUBLIC Threads::Threads SQLite::SQLite3)

if(EOSMIRROR_WITH_XROOTD)
  target_sources(eosmirror_core PRIVATE eosmirror/xrootd_endpoint.cc eosmirror/eos_endpoint.cc)
  target_compile_definitions(eosmirror_core PUBLIC EOSMIRROR_HAVE_XROOTD=1)
  target_link_libraries(eosmirror_core PUBLIC XRootD::XrdCl)
endif()

add_executable(eosmirror eosmirror/main.cc)
target_link_libraries(eosmirror PRIVATE eosmirror_core)

install(TARGETS eosmirror RUNTIME DESTINATION bin)
