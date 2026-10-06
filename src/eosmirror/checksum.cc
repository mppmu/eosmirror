// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/checksum.hh"

#include <zlib.h>

#include <algorithm>
#include <cstdio>

namespace eosmirror {

std::string_view to_string(ChecksumType type) {
  switch (type) {
    case ChecksumType::None: return "none";
    case ChecksumType::Adler32: return "adler32";
  }
  return "none";
}

std::optional<ChecksumType> parse_checksum_type(std::string_view name) {
  if (name == "none") return ChecksumType::None;
  if (name == "adler32" || name == "adler") return ChecksumType::Adler32;
  return std::nullopt;
}

void Adler32::update(std::span<const std::byte> data) {
  const auto* p = reinterpret_cast<const Bytef*>(data.data());
  size_t left = data.size();
  while (left > 0) {
    auto run = static_cast<uInt>(std::min<size_t>(left, 1u << 30));
    value_ = static_cast<uint32_t>(adler32(value_, p, run));
    p += run;
    left -= run;
  }
}

void Hasher::update(std::span<const std::byte> data) {
  if (type_ == ChecksumType::Adler32) adler_.update(data);
}

Checksum Hasher::finish() const {
  if (type_ == ChecksumType::None) return {};
  char hex[9];
  std::snprintf(hex, sizeof hex, "%08x", adler_.value());
  return {type_, hex};
}

}  // namespace eosmirror
