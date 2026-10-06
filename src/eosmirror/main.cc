// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

#include "eosmirror/version.hh"

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("eosmirror %s\n", EOSMIRROR_VERSION);
    return 0;
  }
  std::fprintf(stderr, "eosmirror: not implemented yet\n");
  return 2;
}
