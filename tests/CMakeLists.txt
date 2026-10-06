# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(eosmirror_tests
  unit/test_main.cc
  unit/test_checksum.cc
  unit/test_cli.cc
  unit/test_engine.cc
  unit/test_error.cc
  unit/test_journal.cc
  unit/test_log.cc
  unit/test_pipeline.cc
  unit/test_posix_endpoint.cc
  unit/test_progress_display.cc
  unit/test_queue.cc
  unit/test_selftest.cc
  unit/test_types.cc
)
if(EOSMIRROR_WITH_XROOTD)
  target_sources(eosmirror_tests PRIVATE unit/test_xrootd_eos.cc)
endif()
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
# The tests of what users other than root cannot do, as nobody if needed.
add_test(NAME unit_unprivileged
  COMMAND ${test_launcher} sh ${CMAKE_CURRENT_SOURCE_DIR}/unit/unprivileged.sh
          $<TARGET_FILE:eosmirror_tests> --test-suite=unprivileged)
set_tests_properties(unit_unprivileged PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME cli_smoke
  COMMAND ${test_launcher} bash ${CMAKE_CURRENT_SOURCE_DIR}/cli/smoke.sh $<TARGET_FILE:eosmirror>)

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
  # Under a sanitizer, XRootD's plugins (the security protocols) are loaded
  # through the sanitizer's dlopen, which ignores the library's own search
  # path, so the tests get it in LD_LIBRARY_PATH. Leaks in XRootD's and
  # OpenSSL's global state are not ours (lsan.supp), nor is a race TSan
  # sees in glibc's time zone setup (tsan.supp). The unit tests load the
  # client library too.
  if(EOSMIRROR_SANITIZER AND EOSMIRROR_XROOTD_LIBDIR)
    set_tests_properties(unit unit_unprivileged integration cli_smoke PROPERTIES ENVIRONMENT
      "LD_LIBRARY_PATH=${EOSMIRROR_XROOTD_LIBDIR};LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp;TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
  endif()
endif()
