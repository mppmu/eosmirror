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

namespace {

constexpr std::string_view kTempMarker = ".eosmirror-";
constexpr size_t kTempFixed = 1 + kTempMarker.size() + 12;  // '.', marker, random_suffix()

}  // namespace

std::string temporary_name(std::string_view name, size_t max_size) {
  if (name.size() + kTempFixed > max_size) name = name.substr(0, max_size - kTempFixed);
  std::string result = ".";
  result.append(name).append(kTempMarker).append(random_suffix());
  return result;
}

bool is_temporary_name(std::string_view name) {
  if (name.size() <= kTempFixed || name[0] != '.') return false;
  std::string_view tail = name.substr(name.size() - kTempFixed + 1);
  return tail.starts_with(kTempMarker) &&
         tail.find_first_not_of("0123456789abcdef", kTempMarker.size()) == std::string_view::npos;
}

}  // namespace eosmirror
