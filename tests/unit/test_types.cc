// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/types.hh"

#include <set>

#include "doctest/doctest.h"

using namespace eosmirror;

TEST_CASE("valid entry names") {
  for (const char* name : {"a", ".hidden", "..x", "a b", "a?b", "a=b&c", "x\"y", "日本", "\x1b[2J"})
    CHECK(valid_entry_name(name));
  for (std::string_view name : {"", ".", "..", "a/b", "/", "a\nb", "a\rb"}) {
    INFO("name ", name);
    CHECK_FALSE(valid_entry_name(name));
  }
  CHECK_FALSE(valid_entry_name(std::string_view("a\0b", 3)));
}

TEST_CASE("random suffixes are 12 hex digits and differ") {
  std::set<std::string> seen;
  for (int i = 0; i < 100; ++i) {
    std::string s = random_suffix();
    CHECK(s.size() == 12);
    CHECK(s.find_first_not_of("0123456789abcdef") == std::string::npos);
    seen.insert(s);
  }
  CHECK(seen.size() == 100);
}
