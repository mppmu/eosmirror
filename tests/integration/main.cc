// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests need live servers, named by environment variables. When
// none is set, the run is reported as skipped (exit status 77 for ctest).
#define DOCTEST_CONFIG_IMPLEMENT
#include <cstdio>
#include <cstdlib>

#include "doctest/doctest.h"

int main(int argc, char** argv) {
  const char* xrootd = std::getenv("EOSMIRROR_XROOTD_URL");
  const char* eos = std::getenv("EOSMIRROR_EOS_URL");
  if ((!xrootd || !*xrootd) && (!eos || !*eos)) {
    std::fprintf(stderr,
                 "neither EOSMIRROR_XROOTD_URL nor EOSMIRROR_EOS_URL is set, skipping the "
                 "integration tests\n");
    return 77;
  }
  doctest::Context context(argc, argv);
  return context.run();
}
