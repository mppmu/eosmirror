// SPDX-License-Identifier: GPL-3.0-or-later
#include "eosmirror/checksum.hh"

#include <cstring>
#include <vector>

#include "doctest/doctest.h"

using namespace eosmirror;

namespace {

std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace

TEST_CASE("adler32 reference values") {
  Adler32 empty;
  CHECK(empty.value() == 1);

  Adler32 wiki;
  wiki.update(bytes("Wikipedia"));
  CHECK(wiki.value() == 0x11E60398);  // the RFC 1950 example

  Adler32 a;
  a.update(bytes("abc"));
  CHECK(a.value() == 0x024d0127);
}

TEST_CASE("adler32 is independent of chunking and handles long runs") {
  std::vector<std::byte> data(100000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>((i * 7 + 3) & 0xff);

  Adler32 whole;
  whole.update(data);

  Adler32 chunked;
  size_t pos = 0;
  for (size_t chunk : {1u, 5551u, 5552u, 5553u, 777u, 40000u}) {
    size_t n = std::min(chunk, data.size() - pos);
    chunked.update(std::span(data).subspan(pos, n));
    pos += n;
  }
  chunked.update(std::span(data).subspan(pos));
  CHECK(chunked.value() == whole.value());

  // All 0xff bytes stress the overflow bound of the inner loop.
  std::vector<std::byte> ones(3 * 5552 + 1, std::byte{0xff});
  Adler32 o1, o2;
  o1.update(ones);
  for (auto b : ones) o2.update(std::span(&b, 1));
  CHECK(o1.value() == o2.value());
}

TEST_CASE("hasher formats checksums like EOS") {
  Hasher h(ChecksumType::Adler32);
  h.update(bytes("Wikipedia"));
  CHECK(h.finish() == Checksum{ChecksumType::Adler32, "11e60398"});

  Hasher zero(ChecksumType::Adler32);
  CHECK(zero.finish().hex == "00000001");

  Hasher none(ChecksumType::None);
  none.update(bytes("ignored"));
  CHECK(none.finish() == Checksum{});

  CHECK(parse_checksum_type("adler") == ChecksumType::Adler32);
  CHECK(parse_checksum_type("adler32") == ChecksumType::Adler32);
  CHECK(parse_checksum_type("none") == ChecksumType::None);
  CHECK_FALSE(parse_checksum_type("md5").has_value());
  CHECK(to_string(ChecksumType::Adler32) == "adler32");
}
