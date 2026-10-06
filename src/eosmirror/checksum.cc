// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/checksum.hh"

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
  constexpr uint32_t kBase = 65521;
  constexpr size_t kMaxRun = 5552;  // the largest run before the sums can overflow
  uint32_t a = a_, b = b_;
  const auto* p = reinterpret_cast<const uint8_t*>(data.data());
  size_t left = data.size();
  while (left > 0) {
    size_t run = left < kMaxRun ? left : kMaxRun;
    left -= run;
    for (const auto* end = p + run; p != end; ++p) {
      a += *p;
      b += a;
    }
    a %= kBase;
    b %= kBase;
  }
  a_ = a;
  b_ = b;
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
