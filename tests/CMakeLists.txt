# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(eosmirror_tests
  unit/test_main.cc
  unit/test_checksum.cc
  unit/test_cli.cc
  unit/test_engine.cc
  unit/test_error.cc
  unit/test_journal.cc
  unit/test_posix_endpoint.cc
  unit/test_queue.cc
)
target_include_directories(eosmirror_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(eosmirror_tests PRIVATE eosmirror_core)

# ThreadSanitizer (up to GCC 13) cannot cope with the address space
# randomization of recent kernels ("unexpected memory mapping"), so TSan
# builds run the tests with randomization turned off.
set(test_launcher "")
if(EOSMIRROR_SANITIZER STREQUAL "thread")
  find_program(SETARCH setarch)
  if(SETARCH)
    set(test_launcher ${SETARCH} ${CMAKE_SYSTEM_PROCESSOR} -R)
  endif()
endif()

add_test(NAME unit COMMAND ${test_launcher} $<TARGET_FILE:eosmirror_tests>)

if(EOSMIRROR_WITH_XROOTD)
  add_executable(eosmirror_integration_tests
    integration/main.cc
    integration/test_eos.cc
    integration/test_xrootd.cc
  )
  target_include_directories(eosmirror_integration_tests
    PRIVATE ${PROJECT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/unit)
  target_link_libraries(eosmirror_integration_tests PRIVATE eosmirror_core)
  add_test(NAME integration COMMAND ${test_launcher} $<TARGET_FILE:eosmirror_integration_tests>)
  set_tests_properties(integration PROPERTIES SKIP_RETURN_CODE 77)
endif()
add_test(NAME cli_smoke
  COMMAND ${test_launcher} bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/smoke.sh $<TARGET_FILE:eosmirror>)
