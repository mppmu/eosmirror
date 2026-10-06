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

TEST_CASE("temporary names") {
  std::string t = temporary_name("data.bin");
  CHECK(t.starts_with(".data.bin.eosmirror-"));
  CHECK(t.size() == 1 + 8 + 11 + 12);
  CHECK(is_temporary_name(t));
  CHECK(temporary_name(std::string(300, 'x')).size() == 255);
  CHECK(is_temporary_name(temporary_name(std::string(300, 'x'))));
  CHECK(temporary_name(std::string(300, 'x'), 100).size() == 100);

  CHECK(is_temporary_name(".a.eosmirror-0123456789ab"));
  for (std::string_view name : {"a.eosmirror-0123456789ab", "..eosmirror-0123456789ab",
                                ".a.eosmirror-0123456789", ".a.eosmirror-0123456789aB",
                                ".a.eosmirror-0123456789abc", ".a.eosmirror-0123456789ab.txt",
                                ".eosmirror-selftest-0123456789ab", ".notes.eosmirror-old",
                                ".a.eosmirror-0123456789xy"}) {
    INFO("name ", name);
    CHECK_FALSE(is_temporary_name(name));
  }
}
