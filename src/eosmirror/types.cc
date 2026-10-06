// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/types.hh"

#include <cstdio>
#include <random>

namespace eosmirror {

std::string_view to_string(EntryType type) {
  switch (type) {
    case EntryType::File: return "file";
    case EntryType::Directory: return "directory";
    case EntryType::Symlink: return "symlink";
    case EntryType::Other: return "special file";
  }
  return "entry";
}

bool valid_entry_name(std::string_view name) {
  if (name.empty() || name == "." || name == "..") return false;
  return name.find_first_of(std::string_view("/\0\n\r", 4)) == std::string_view::npos;
}

std::string random_suffix() {
  thread_local std::mt19937_64 rng = [] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(),
                       device(), device(), device(), device()};
    return std::mt19937_64(seed);
  }();
  char buf[17];
  std::snprintf(buf, sizeof buf, "%012llx",
                static_cast<unsigned long long>(rng() & 0xffffffffffffULL));
  return buf;
}

}  // namespace eosmirror
