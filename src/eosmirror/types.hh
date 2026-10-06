// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "eosmirror/checksum.hh"

namespace eosmirror {

enum class EntryType : uint8_t { File, Directory, Symlink, Other };

std::string_view to_string(EntryType type);

// A point in time with nanosecond resolution, like struct timespec.
struct Timespec {
  int64_t sec = 0;
  int32_t nsec = 0;

  auto operator<=>(const Timespec&) const = default;

  // The time truncated to the given resolution in nanoseconds (1 = unchanged,
  // 1000000000 = whole seconds).
  Timespec truncated(int32_t nsec_resolution) const {
    return {sec, nsec / nsec_resolution * nsec_resolution};
  }
};

// Permission bits (07777), without the file type bits.
using ModeBits = uint32_t;

// Which metadata fields an operation applies.
enum class MetaFields : uint8_t { None = 0, Owner = 1, Mode = 2, Mtime = 4, All = 7 };

constexpr MetaFields operator|(MetaFields a, MetaFields b) {
  return static_cast<MetaFields>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr MetaFields operator&(MetaFields a, MetaFields b) {
  return static_cast<MetaFields>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr MetaFields operator~(MetaFields a) {
  return static_cast<MetaFields>(~static_cast<uint8_t>(a) & 7);
}
constexpr bool has(MetaFields fields, MetaFields flag) { return (fields & flag) != MetaFields::None; }

// A directory entry or stat result. Directory listings fill it completely,
// so that the engine never needs a stat per entry.
struct Entry {
  std::string name;
  EntryType type = EntryType::Other;
  uint64_t size = 0;  // files only
  Timespec mtime;
  uint32_t uid = 0;
  uint32_t gid = 0;
  ModeBits mode = 0;
  std::string link_target;  // symlinks only
  // Identifies a directory however it was reached (such as device and inode),
  // to find cycles in listings that follow symlinks; empty where unknown.
  std::string id;
  // The checksum stored with a file, where listings carry one (EOS); None
  // otherwise.
  Checksum checksum;
};

// Paths handed to endpoints are relative to the endpoint's root: "" for the
// root itself, otherwise "a/b/c" without leading or trailing slashes.
using RelPath = std::string;

inline RelPath join(const RelPath& dir, std::string_view name) {
  if (dir.empty()) return RelPath(name);
  RelPath result;
  result.reserve(dir.size() + 1 + name.size());
  result.append(dir).append("/").append(name);
  return result;
}

// Whether a name from a listing can be joined into a path: not empty, "." or
// "..", and without '/', NUL or line breaks.
bool valid_entry_name(std::string_view name);

// 48 random bits as 12 hex digits, for temporary names.
std::string random_suffix();

// The name of a temporary for a file name, ".<name>.eosmirror-<random>", with
// the name cut so that the whole is at most max_size bytes long.
std::string temporary_name(std::string_view name, size_t max_size = 255);

// Whether a name has the form of temporary_name().
bool is_temporary_name(std::string_view name);

}  // namespace eosmirror
