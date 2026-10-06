// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/log.hh"

#include <algorithm>

#include "doctest/doctest.h"
#include "stderr_capture.hh"

using namespace eosmirror;

TEST_CASE("printable replaces control characters and keeps UTF-8") {
  CHECK(printable("plain name.txt") == "plain name.txt");
  CHECK(printable("a\x1b[2Jb\nc\rd\te\x7f") == "a?[2Jb?c?d?e?");
  CHECK(printable(std::string("nul\0byte", 8)) == "nul?byte");
  CHECK(printable("Größe → 日本 🙂") == "Größe → 日本 🙂");
  // C1 controls (U+009B is a terminal's CSI), stray continuation bytes,
  // overlong encodings and truncated sequences.
  CHECK(printable("x\xc2\x9b" "2Jy") == "x?" "?2Jy");
  CHECK(printable("\x9b\xbf") == "??");
  CHECK(printable("\xc0\x9b") == "??");
  CHECK(printable("end\xe2\x82") == "end??");
}

TEST_CASE("log lines are sanitized as a whole") {
  StderrCapture capture;
  log::warn("file \x1b[2Jevil\nINJECTED line");
  std::string out = capture.text();
  CHECK(out.find('\x1b') == std::string::npos);
  CHECK(out.find("file ?[2Jevil?INJECTED line\n") != std::string::npos);
  CHECK(std::count(out.begin(), out.end(), '\n') == 1);
}
