// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace eosmirror {

enum class ChecksumType : uint8_t { None, Adler32 };

std::string_view to_string(ChecksumType type);
std::optional<ChecksumType> parse_checksum_type(std::string_view name);

// A checksum as the endpoints exchange it: lowercase hex, zero padded to the
// type's width (8 digits for adler32, as EOS prints it).
struct Checksum {
  ChecksumType type = ChecksumType::None;
  std::string hex;

  bool operator==(const Checksum&) const = default;
};

// Streaming adler32 (RFC 1950), the checksum EOS computes while writing.
class Adler32 {
 public:
  void update(std::span<const std::byte> data);
  uint32_t value() const { return value_; }

 private:
  uint32_t value_ = 1;
};

// Computes a checksum of a given type over a stream of data.
class Hasher {
 public:
  explicit Hasher(ChecksumType type) : type_(type) {}

  ChecksumType type() const { return type_; }
  void update(std::span<const std::byte> data);
  Checksum finish() const;

 private:
  ChecksumType type_;
  Adler32 adler_;
};

}  // namespace eosmirror
